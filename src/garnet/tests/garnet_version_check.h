// Shared host-test helper verifying single-source kext version consistency
// across src/garnet/VERSION, Info.plist, garnet_version.h, kmod_info.c,
// native_s1c.h, GarnetMetalNub.cpp, and GarnetDisplayNub.cpp.
#ifndef GARNET_VERSION_CHECK_H
#define GARNET_VERSION_CHECK_H

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace garnet_test_ver {

inline std::string read_all(const std::string &path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline size_t count_sub(const std::string &s, const std::string &sub) {
    if (sub.empty()) return 0;
    size_t n = 0, pos = 0;
    while ((pos = s.find(sub, pos)) != std::string::npos) {
        n++;
        pos += sub.size();
    }
    return n;
}

inline std::string read_version(const std::string &root) {
    std::string v = read_all(root + "/src/garnet/VERSION");
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ' || v.back() == '\t'))
        v.pop_back();
    return v;
}

inline uint32_t parse_build(const std::string &ver) {
    // Extract final numeric component (e.g. "0.0.673" -> 673, "0.2.1" -> 1;
    // if patch is 0 like "0.2.0", encode major*10000 + minor*100 + patch so build > 0).
    const size_t d1 = ver.find('.');
    if (d1 == std::string::npos) return 0;
    const size_t d2 = ver.find('.', d1 + 1);
    if (d2 == std::string::npos) return 0;
    const uint32_t maj = (uint32_t)std::strtoul(ver.c_str(), nullptr, 10);
    const uint32_t min = (uint32_t)std::strtoul(ver.c_str() + d1 + 1, nullptr, 10);
    const uint32_t pat = (uint32_t)std::strtoul(ver.c_str() + d2 + 1, nullptr, 10);
    if (maj == 0 && min == 0) return pat;
    return maj * 10000u + min * 100u + pat;
}

inline bool plist_matches_version(const std::string &root, const std::string &plist) {
    const std::string ver = read_version(root);
    if (ver.empty()) return false;
    const std::string tag = "<string>" + ver + "</string>";
    return count_sub(plist, tag) == 2;
}

inline bool header_and_sources_match(const std::string &root, const std::string &s1ch) {
    const std::string ver = read_version(root);
    const uint32_t build = parse_build(ver);
    if (ver.empty() || build == 0) return false;
    const std::string vh = read_all(root + "/src/garnet/src/garnet_version.h");
    const std::string kmod = read_all(root + "/src/garnet/src/kmod_info.c");
    const std::string mnub = read_all(root + "/src/garnet/src/GarnetMetalNub.cpp");
    const std::string dnub = read_all(root + "/src/garnet/src/GarnetDisplayNub.cpp");
    const std::string wantStr = "GARNET_KEXT_VERSION_STR \"" + ver + "\"";
    const std::string wantBuild = "GARNET_KEXT_BUILD";
    const std::string wantBuildVal = std::to_string(build) + "u";
    return vh.find(wantStr) != std::string::npos &&
           vh.find(wantBuild) != std::string::npos &&
           vh.find(wantBuildVal) != std::string::npos &&
           kmod.find("GARNET_KEXT_VERSION_STR") != std::string::npos &&
           s1ch.find("constexpr uint32_t kN1cKextBuild = GARNET_KEXT_BUILD;") != std::string::npos &&
           count_sub(mnub, "GARNET_KEXT_BUILD") >= 2 &&
           count_sub(dnub, "GARNET_KEXT_BUILD") >= 1;
}

inline bool all_match(const std::string &root) {
    const std::string plist = read_all(root + "/src/garnet/Info.plist");
    const std::string s1ch = read_all(root + "/src/garnet/src/amd/native_s1c.h");
    return plist_matches_version(root, plist) && header_and_sources_match(root, s1ch);
}

inline std::string read_native_engine(const std::string &amdDir) {
    return read_all(amdDir + "/garnet_ring_submit.cpp") + "\n" +
           read_all(amdDir + "/garnet_session_vm.cpp") + "\n" +
           read_all(amdDir + "/garnet_vram_alloc.cpp") + "\n" +
           read_all(amdDir + "/garnet_submit_internal.hpp");
}

}  // namespace garnet_test_ver

#endif  // GARNET_VERSION_CHECK_H
