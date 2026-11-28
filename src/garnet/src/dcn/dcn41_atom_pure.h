#pragma once

#include <stddef.h>
#include <stdint.h>

namespace rdna4atom {

struct PciIdentity {
    uint8_t bus, slot, function;
    uint16_t vendor, device, subVendor, subDevice;
};

struct Board {
    uint32_t channels, channelWidthBytes;
    uint32_t crystalKhz;
    uint32_t dramClockChange100Ns, stutterExit100Ns, stutterEnterExit100Ns;
};

inline uint16_t le16(const uint8_t *bytes) {
    return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

inline uint32_t le32(const uint8_t *bytes) {
    return le16(bytes) | (static_cast<uint32_t>(le16(bytes + 2)) << 16);
}

inline bool checksum_ok(const uint8_t *bytes, size_t length) {
    uint8_t sum = 0;
    for (size_t index = 0; index < length; ++index) sum += bytes[index];
    return sum == 0;
}

inline bool contains(size_t size, size_t offset, size_t length) {
    return offset <= size && length <= size - offset;
}

inline bool table(const uint8_t *image, size_t length, size_t offset, uint16_t minimum,
                  uint8_t format, uint8_t content) {
    if (offset == 0 || !contains(length, offset, 4)) return false;
    const uint16_t declared = le16(image + offset);
    return declared >= minimum && contains(length, offset, declared) &&
           image[offset + 2] == format && image[offset + 3] == content;
}

inline bool parse_rom(const uint8_t *image, size_t length, const PciIdentity &pci, Board &out) {
    out = {};
    if (!image || length < 0x4a || le16(image) != 0xaa55) return false;
    const size_t romSize = static_cast<size_t>(image[2]) * 512u;
    if (romSize < 0x4a || romSize > length || !checksum_ok(image, romSize)) return false;

    // atomfirmware.h: atom_rom_header_v2_2 prefix, ATOM master data v2.1.
    const size_t rom = le16(image + 0x48);
    if (!table(image, romSize, rom, 0x28, 2, 2) && !table(image, romSize, rom, 0x28, 2, 3)) return false;
    if (le32(image + rom + 4) != 0x4d4f5441u ||
        le16(image + rom + 0x18) != pci.subVendor || le16(image + rom + 0x1a) != pci.subDevice)
        return false;
    const size_t pciOffset = le16(image + 0x18);
    if (pciOffset != le16(image + rom + 0x1c) || !contains(romSize, pciOffset, 0x18) ||
        le32(image + pciOffset) != 0x52494350u || le16(image + pciOffset + 4) != pci.vendor ||
        le16(image + pciOffset + 6) != pci.device || le16(image + pciOffset + 0x10) * 512u != romSize)
        return false;
    const size_t master = le16(image + rom + 0x20);
    if (!table(image, romSize, master, 0x4a, 2, 1)) return false;

    // DCN4x moved geometry to atom_umc_info_v4_0. DCE 4.5 has the SoC latency fields.
    const size_t umc = le16(image + master + 0x34);
    const size_t dce = le16(image + master + 0x3a);
    if (!table(image, romSize, umc, 0x54, 4, 0) || !table(image, romSize, dce, 0x44, 4, 5) ||
        image[umc + 0x1a] != 0x70)
        return false;
    const uint32_t channels = image[umc + 0x40];
    const uint32_t width = image[umc + 0x41];
    const uint32_t crystalKhz = le16(image + dce + 0x0c) * 10u;
    if (!channels || channels > 32 || width < 3 || width > 6 || !crystalKhz) return false;
    out = {channels, (1u << width) / 8u, crystalKhz, le32(image + dce + 0x38),
           le32(image + dce + 0x3c), le32(image + dce + 0x40)};
    return true;
}

inline bool parse_vfct(const uint8_t *bytes, size_t length, const PciIdentity &pci, Board &out) {
    out = {};
    constexpr size_t kTableHeader = 0x4c;
    constexpr size_t kImageHeader = 28;
    constexpr size_t kMaxVfctBytes = 4u << 20;
    if (!bytes || length < kTableHeader || length > kMaxVfctBytes || le32(bytes) != 0x54434656u ||
        le32(bytes + 4) != length || bytes[8] != 1 || !checksum_ok(bytes, length))
        return false;

    const size_t images = le32(bytes + 0x34);
    const size_t library = le32(bytes + 0x38);
    const size_t end = library ? library : length;
    if (images < kTableHeader || end > length || !contains(end, images, kImageHeader)) return false;
    const uint8_t *matched = nullptr;
    size_t matchedSize = 0;
    for (size_t offset = images; offset < end;) {
        if (!contains(end, offset, kImageHeader)) return false;
        const uint8_t *entry = bytes + offset;
        const size_t size = le32(entry + 24);
        offset += kImageHeader;
        if (!contains(end, offset, size)) return false;
        if (le32(entry) == pci.bus && le32(entry + 4) == pci.slot && le32(entry + 8) == pci.function &&
            le16(entry + 12) == pci.vendor && le16(entry + 14) == pci.device) {
            const uint16_t subVendor = le16(entry + 16), subDevice = le16(entry + 18);
            if (matched || !size || ((subVendor || subDevice) &&
                (subVendor != pci.subVendor || subDevice != pci.subDevice))) return false;
            matched = bytes + offset;
            matchedSize = size;
        }
        offset += size;
    }
    return matched && parse_rom(matched, matchedSize, pci, out);
}

} // namespace rdna4atom
