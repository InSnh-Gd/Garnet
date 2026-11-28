#include "dcn/dcn41_atom_pure.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

using Bytes = std::vector<uint8_t>;
using rdna4atom::Board;
using rdna4atom::PciIdentity;

static void expect(bool pass, const char *what) {
    if (!pass) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

static void write(Bytes &bytes, size_t offset, uint32_t value, size_t width) {
    for (size_t index = 0; index < width; ++index)
        bytes.at(offset + index) = static_cast<uint8_t>(value >> (8u * index));
}

static void checksum(Bytes &bytes, size_t offset) {
    bytes.at(offset) = 0;
    uint8_t sum = 0;
    for (const auto value : bytes) sum += value;
    bytes.at(offset) = static_cast<uint8_t>(0u - sum);
}

static constexpr PciIdentity kBoard { 1, 2, 0, 0x1002, 0x7590, 0x1234, 0xabcd };

static Bytes rom_fixture() {
    Bytes bytes(1024);
    write(bytes, 0, 0x0002aa55, 3);
    write(bytes, 0x18, 0xa0, 2);
    write(bytes, 0x48, 0x60, 2);
    write(bytes, 0x60, 0x0302002c, 4);
    write(bytes, 0x64, 0x4d4f5441, 4);
    write(bytes, 0x78, 0xabcd1234, 4);
    write(bytes, 0x7c, 0xa0, 2);
    write(bytes, 0x80, 0xc0, 2);
    write(bytes, 0xa0, 0x52494350, 4);
    write(bytes, 0xa4, 0x75901002, 4);
    write(bytes, 0xaa, 0x18, 2);
    write(bytes, 0xb0, 2, 2);
    write(bytes, 0xc0, 0x0102004a, 4);
    write(bytes, 0xf4, 0x120, 2);
    write(bytes, 0xfa, 0x180, 2);
    write(bytes, 0x120, 0x00040054, 4);
    write(bytes, 0x13a, 0x70, 1);
    write(bytes, 0x160, 0x0506, 2);
    write(bytes, 0x180, 0x050400d4, 4);
    write(bytes, 0x18c, 4000, 2);
    write(bytes, 0x1b8, 120, 4);
    write(bytes, 0x1bc, 34, 4);
    write(bytes, 0x1c0, 56, 4);
    checksum(bytes, bytes.size() - 1);
    return bytes;
}

static Bytes vfct_fixture(const Bytes &rom) {
    Bytes bytes(0x4c + 28);
    write(bytes, 0, 0x54434656, 4);
    write(bytes, 4, bytes.size() + rom.size(), 4);
    write(bytes, 8, 1, 1);
    write(bytes, 0x34, 0x4c, 4);
    write(bytes, 0x4c, 1, 4);
    write(bytes, 0x50, 2, 4);
    write(bytes, 0x58, 0x75901002, 4);
    write(bytes, 0x60, 1, 4);
    write(bytes, 0x64, rom.size(), 4);
    bytes.insert(bytes.end(), rom.begin(), rom.end());
    checksum(bytes, 9);
    return bytes;
}

static void fixtures() {
    const auto rom = rom_fixture();
    const auto vfct = vfct_fixture(rom);
    Board board {};
    expect(rdna4atom::parse_vfct(vfct.data(), vfct.size(), kBoard, board), "decode matching VFCT with omitted subsystem and matching ATOM subsystem");
    expect(board.channels == 6 && board.channelWidthBytes == 4 && board.crystalKhz == 40000 &&
           board.dramClockChange100Ns == 120 && board.stutterExit100Ns == 34 &&
           board.stutterEnterExit100Ns == 56, "decode independent UMC and DCE values in the documented units");

    auto wrongPci = kBoard;
    wrongPci.bus = 2;
    expect(!rdna4atom::parse_vfct(vfct.data(), vfct.size(), wrongPci, board) && board.channels == 0,
           "do not reuse board data after a rejected exact BDF match");
    wrongPci = kBoard;
    wrongPci.subDevice = 0xabce;
    expect(!rdna4atom::parse_vfct(vfct.data(), vfct.size(), wrongPci, board), "reject mismatched ATOM subsystem");

    auto malformed = vfct;
    write(malformed, 0x5c, 0xabce1234, 4);
    checksum(malformed, 9);
    expect(!rdna4atom::parse_vfct(malformed.data(), malformed.size(), kBoard, board), "reject contradictory VFCT subsystem");
    malformed = vfct;
    malformed.insert(malformed.end(), vfct.begin() + 0x4c, vfct.end());
    write(malformed, 4, malformed.size(), 4);
    checksum(malformed, 9);
    expect(!rdna4atom::parse_vfct(malformed.data(), malformed.size(), kBoard, board), "reject duplicate matching VFCT images");
    malformed = vfct;
    write(malformed, 0x64, UINT32_MAX, 4);
    checksum(malformed, 9);
    expect(!rdna4atom::parse_vfct(malformed.data(), malformed.size(), kBoard, board), "reject VFCT image crossing the table boundary");

    malformed = rom;
    write(malformed, 0x180, 0x05040040, 4);
    checksum(malformed, malformed.size() - 1);
    expect(!rdna4atom::parse_rom(malformed.data(), malformed.size(), kBoard, board), "reject DCE latency fields beyond the declared table");
    write(malformed, 0x180, 0x060400d4, 4);
    checksum(malformed, malformed.size() - 1);
    expect(!rdna4atom::parse_rom(malformed.data(), malformed.size(), kBoard, board), "reject unknown DCE revision");
    malformed = rom;
    write(malformed, 0x161, 255, 1);
    checksum(malformed, malformed.size() - 1);
    expect(!rdna4atom::parse_rom(malformed.data(), malformed.size(), kBoard, board), "reject invalid width encoding before shift");
}

int main(int argc, char **argv) {
    if (argc != 1) {
        expect(argc == 4 || argc == 7, "usage: dcn41_atom_test FILE SUBVENDOR SUBDEVICE [BUS SLOT FUNCTION]");
        std::ifstream in(argv[1], std::ios::binary);
        expect(static_cast<bool>(in), "open firmware file");
        const Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto pci = kBoard;
        pci.subVendor = static_cast<uint16_t>(std::strtoul(argv[2], nullptr, 0));
        pci.subDevice = static_cast<uint16_t>(std::strtoul(argv[3], nullptr, 0));
        Board board {};
        if (argc == 7) {
            pci.bus = static_cast<uint8_t>(std::strtoul(argv[4], nullptr, 0));
            pci.slot = static_cast<uint8_t>(std::strtoul(argv[5], nullptr, 0));
            pci.function = static_cast<uint8_t>(std::strtoul(argv[6], nullptr, 0));
            expect(rdna4atom::parse_vfct(bytes.data(), bytes.size(), pci, board), "decode external VFCT");
        } else {
            expect(rdna4atom::parse_rom(bytes.data(), bytes.size(), pci, board), "decode external ATOM ROM");
        }
        std::printf("channels=%u channel_width_bytes=%u crystal_khz=%u mclk_change_100ns=%u sr_exit_100ns=%u sr_enter_exit_100ns=%u\n",
                    board.channels, board.channelWidthBytes, board.crystalKhz, board.dramClockChange100Ns,
                    board.stutterExit100Ns, board.stutterEnterExit100Ns);
        return 0;
    }
    fixtures();
    std::puts("DCN 4.01 ATOM board inputs: PASS");
    return 0;
}
