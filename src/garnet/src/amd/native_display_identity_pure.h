#pragma once

#include "../dcn/display_state_pure.h"

namespace rdna4primary {

// Local primary consumers currently require the console mode on head/OTG0.
inline bool valid(const rdna4peer::PrimaryView &p, const rdna4peer::Expected &e,
                  uint64_t framebufferID, uint64_t consoleOffset,
                  uint64_t consoleLength, uint32_t width, uint32_t height,
                  uint32_t rowBytes) {
    if (p.version != rdna4peer::kVersion || !p.generation || !framebufferID ||
        p.fbEntryID != framebufferID || !e.providerEntryID ||
        p.providerEntryID != e.providerEntryID || p.vendor != e.vendor ||
        p.device != e.device || !e.asic || p.asic != e.asic)
        return false;
    if (!e.bar0Phys || !e.bar0Size || p.bar0Phys != e.bar0Phys ||
        p.bar0Size != e.bar0Size || p.displayIndex != 0 || p.head != 0 || p.otg != 0)
        return false;
    uint64_t physical = 0, physicalEnd = 0;
    if (!rdna4peer::add_ok(e.bar0Phys, consoleOffset, physical) ||
        !rdna4peer::add_ok(physical, consoleLength, physicalEnd) ||
        consoleOffset > e.bar0Size || consoleLength > e.bar0Size - consoleOffset)
        return false;
    if (!width || !height || !rowBytes || !consoleLength ||
        static_cast<uint64_t>(width) * 4 > rowBytes ||
        static_cast<uint64_t>(rowBytes) * height != consoleLength ||
        ((physical | consoleLength | rowBytes) & 3u))
        return false;
    return p.consolePhysBase == physical && p.consoleVramOffset == consoleOffset &&
           p.consoleLength == consoleLength && p.consoleWidth == width &&
           p.consoleHeight == height && p.consoleRowBytes == rowBytes &&
           p.consoleDepth == 32 && p.modeWidth == width && p.modeHeight == height &&
           p.modeRowBytes == rowBytes && p.modeDepth == 32;
}

inline bool valid_snapshot_view(const rdna4peer::Snapshot &s, const rdna4peer::PrimaryView &p,
                                const rdna4peer::Expected &e, uint64_t framebufferID) {
    return s.version == rdna4peer::kVersion && p.version == rdna4peer::kVersion &&
           s.displayIndex == 0 && s.generation != 0 && s.generation == p.generation &&
           rdna4peer::validate_peer(s, e, true) == rdna4peer::kOk &&
           valid(p, e, framebufferID, s.console.offset, s.console.length,
                 s.consoleWidth, s.consoleHeight, s.consoleRowBytes);
}

} // namespace rdna4primary
