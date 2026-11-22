#include "ipdiscovery.hpp"
#include <cstdio>
#include <cstdint>
#include <vector>

static unsigned runs = 0, failures = 0;
static void expect(bool ok, const char *message) {
    ++runs;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", message);
}
static uint16_t get16(const std::vector<uint8_t> &b, size_t p) {
    return static_cast<uint16_t>(b[p] | (b[p + 1] << 8));
}
static void put16(std::vector<uint8_t> &b, size_t p, uint16_t v) {
    b[p] = v & 255; b[p + 1] = v >> 8;
}
static void put32(std::vector<uint8_t> &b, size_t p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) b[p + i] = (v >> (8 * i)) & 255;
}
static uint16_t sum(const std::vector<uint8_t> &b, size_t p, size_t count) {
    uint16_t result = 0;
    for (size_t i = 0; i < count; ++i) result = static_cast<uint16_t>(result + b[p + i]);
    return result;
}
static void seal(std::vector<uint8_t> &b) {
    const size_t list = get16(b, 4) == 2 ? 16 : 12;
    put16(b, list + 2, sum(b, get16(b, list), get16(b, list + 4)));
    put16(b, 8, sum(b, 10, get16(b, 10) - 10));
}
static std::vector<uint8_t> binary(bool wide = false, uint16_t version = 1) {
    std::vector<uint8_t> b(1024, 0);
    put32(b, 0, 0x28211407);
    put16(b, 4, version); put16(b, 10, 512);
    const size_t list = version == 2 ? 16 : 12;
    if (version == 2) put16(b, 12, 2);
    put16(b, list, 0x80); put16(b, list + 4, 0xc0);
    put32(b, 0x80, 0x53445049); put16(b, 0x84, wide ? 4 : 3);
    put16(b, 0x86, 0xc0); put16(b, 0x8c, 1);
    put16(b, 0x90, 0x100);
    if (wide) b[0xce] = 1;
    put16(b, 0x100, 0); put16(b, 0x102, 2);
    put16(b, 0x104, IpDiscovery::HwGc);
    b[0x107] = 1; b[0x108] = 12;
    put32(b, 0x10c, wide ? 0xc0001260 : 0x1260);
    if (wide) put32(b, 0x110, 0x12345678);
    const size_t second = wide ? 0x114 : 0x110;
    put16(b, second, IpDiscovery::HwMp0);
    b[second + 3] = 1; b[second + 4] = 14; b[second + 6] = 2;
    put32(b, second + 8, 0x16000);
    seal(b);
    return b;
}

int main() {
    IpDiscovery d;
    IpDiscovery::IpEntry ip {};
    uint32_t address = 0;
    auto b = binary();
    expect(d.init(b.data(), b.size()), "valid binary v1 / IPDS v3");
    expect(d.binaryLength() == 512, "binary length excludes trailing TMR bytes");
    expect(d.findIp(IpDiscovery::HwGc, 0, ip) && ip.major == 12 && ip.revision == 0 && ip.bases[0] == 0x1260,
           "Navi44 GC record and base decoded");
    expect(d.regByteOffset(IpDiscovery::HwMp0, 0, 0, 0x63, address) && address == (0x16000 + 0x63) * 4,
           "PSP register dword-to-byte address");
    expect(!d.ipAt(2, ip), "out-of-range IP index rejected");
    expect(!d.regByteOffset(IpDiscovery::HwGc, 0, 1, 0, address), "missing base segment rejected");
    b = binary(false, 2);
    expect(d.init(b.data(), b.size()), "binary v2 table list decoded");
    put16(b, 12, 0); seal(b);
    expect(!d.init(b.data(), b.size()), "v2 empty table directory rejected");
    b = binary(); put16(b, 4, 3); seal(b);
    expect(!d.init(b.data(), b.size()), "unknown binary header version rejected");
    b = binary(); put16(b, 0x84, 5); seal(b);
    expect(!d.init(b.data(), b.size()), "unknown IPDS record layout rejected");
    b = binary(); b[0x98] ^= 1;
    put16(b, 8, sum(b, 10, get16(b, 10) - 10));
    expect(!d.init(b.data(), b.size()), "IPDS checksum checked independently of binary checksum");
    b = binary(); put16(b, 0x90, 0x280); put16(b, 0x282, 1); seal(b);
    expect(!d.init(b.data(), b.size()), "die outside declared binary rejected despite readable trailing bytes");
    b = binary(); put16(b, 16, 80); put16(b, 0x86, 80); seal(b);
    expect(!d.init(b.data(), b.size()), "die outside declared IPDS table rejected");
    b = binary(); b[0x113] = 255; seal(b);
    expect(!d.init(b.data(), b.size()), "entire record array validated before accepting discovery");
    b = binary(); put16(b, 0x8c, 17); seal(b);
    expect(!d.init(b.data(), b.size()), "die count exceeding header capacity rejected");
    b = binary(); put16(b, 0x100, 1); seal(b);
    expect(!d.init(b.data(), b.size()), "die header ID must match selected die");
    b = binary(); put16(b, 0x8e, 1); seal(b);
    expect(!d.init(b.data(), b.size()), "a table without die zero is not a supported single-die GPU");
    b = binary(true);
    expect(d.init(b.data(), b.size()) && d.findIp(IpDiscovery::HwGc, 0, ip) && ip.bases[0] == 0x1260,
           "IPDS v4 64-bit base normalized as Linux lower32 & 0x3fffffff");
    expect(d.findIp(IpDiscovery::HwMp0, 0, ip) && ip.major == 14 && ip.revision == 2 && ip.bases[0] == 0x16000,
           "64-bit record stride preserves the following PSP record");
    b = binary(); put32(b, 0x10c, 0x3fffffff); seal(b);
    expect(d.init(b.data(), b.size()) && !d.regByteOffset(IpDiscovery::HwGc, 0, 0, 1, address),
           "byte address overflow rejected");
    b = binary(); seal(b);
    expect(d.init(b.data(), b.size()) && !d.regByteOffset(IpDiscovery::HwGc, 0, 0, UINT32_MAX, address),
           "base plus register overflow rejected");
    b = binary();
    std::vector<uint8_t> rom(2048, 0);
    for (size_t i = 0; i < b.size(); ++i) rom[256 + i] = b[i];
    expect(d.init(rom.data(), rom.size()) && d.binaryOffset() == 256 && d.binaryLength() == 512,
           "embedded ROM binary uses its own declared extent");
    expect(!d.init(nullptr, 0) && !d.isValid() && !d.binary() && d.binaryLength() == 0,
           "failed reinitialization clears previous parser state");
    std::printf("%u/%u checks passed\n", runs - failures, runs);
    return failures ? 1 : 0;
}
