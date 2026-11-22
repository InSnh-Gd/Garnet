// bootargs_alias_test.cpp - the rdna4-* -> garnet-* boot-arg fallback (src/GarnetBootArgs.h).
//
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -DKERNEL \
//     -I src/garnet/tests/bootargs_stub -include src/garnet/src/GarnetBootArgs.h \
//     src/garnet/tests/bootargs_alias_test.cpp -o /tmp/bootargs_alias && /tmp/bootargs_alias .
//
// Checks: the pure name mapping; the KERNEL wrapper reads the new name first, falls back to the
// legacy one only when the new one is absent, logs every legacy use; the two header copies are
// identical; both kext Makefiles force-include the header for C++; every boot-arg the kexts read
// has a legacy spelling; no C source reads a boot-arg (C is not force-included).
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

static int g_checks, g_fail;
static void expect(bool ok, const std::string &what) { g_checks++; if (!ok) { g_fail++; std::printf("FAIL: %s\n", what.c_str()); } }

// ---- fake kernel: a boot-args line and a log
static std::string g_args, g_log;
extern "C" void IOLog(const char *fmt, ...) { char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap); g_log += b; }
#undef PE_parse_boot_argn
extern "C" bool PE_parse_boot_argn(const char *name, void *val, int size) {
    std::istringstream in(g_args); std::string tok;
    while (in >> tok) {
        const size_t eq = tok.find('=');
        const std::string k = tok.substr(0, eq);
        if (k != name) continue;
        if (eq == std::string::npos) { if (size >= 4) *(unsigned *)val = 1; return true; }
        unsigned long v = std::strtoul(tok.c_str() + eq + 1, nullptr, 0);
        if (size == 4) *(unsigned *)val = (unsigned)v; else if (size == 8) *(unsigned long long *)val = v; else if (size == 1) *(unsigned char *)val = (unsigned char)v;
        return true;
    }
    return false;
}
// what the kext source text compiles to under -include GarnetBootArgs.h
#define PE_parse_boot_argn(n, v, s) garnet_parse_boot_argn((n), (v), (s))
static bool kext_read(const char *name, unsigned &v) { v = 0; return PE_parse_boot_argn(name, &v, sizeof(v)); }
#undef PE_parse_boot_argn

static std::string slurp(const std::string &p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static void walk(const std::string &dir, std::vector<std::string> &out) {
    DIR *d = opendir(dir.c_str()); if (!d) return;
    while (dirent *e = readdir(d)) {
        const std::string n = e->d_name; if (n == "." || n == "..") continue;
        const std::string p = dir + "/" + n;
        if (e->d_type == DT_DIR) walk(p, out); else out.push_back(p);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    char o[64];
    // ---- pure mapping
    expect(garnet_legacy_boot_arg_name("garnet", o, sizeof(o)) && !std::strcmp(o, "rdna4bringup"), "garnet -> rdna4bringup");
    expect(garnet_legacy_boot_arg_name("garnet-stage", o, sizeof(o)) && !std::strcmp(o, "rdna4-stage"), "garnet-stage -> rdna4-stage");
    expect(garnet_legacy_boot_arg_name("garnet-metal-ws", o, sizeof(o)) && !std::strcmp(o, "rdna4-metal-ws"), "garnet-metal-ws -> rdna4-metal-ws");
    expect(!garnet_legacy_boot_arg_name("garnet-", o, sizeof(o)) && !o[0], "garnet- alone has no legacy name");
    expect(!garnet_legacy_boot_arg_name("garnetx", o, sizeof(o)), "garnetx has no legacy name");
    expect(!garnet_legacy_boot_arg_name("garnetbringupx", o, sizeof(o)), "garnetbringupx has no legacy name");
    expect(!garnet_legacy_boot_arg_name("rdna4-stage", o, sizeof(o)), "a legacy name is not mapped again");
    expect(!garnet_legacy_boot_arg_name("debug", o, sizeof(o)), "other boot-args are untouched");
    expect(!garnet_legacy_boot_arg_name("garnet-stage", o, 11) && !o[0], "a too-small buffer is refused, never truncated");
    expect(garnet_legacy_boot_arg_name("garnet-stage", o, 12) && !std::strcmp(o, "rdna4-stage"), "an exact-size buffer fits");
    expect(!garnet_legacy_boot_arg_name(nullptr, o, sizeof(o)) && !garnet_legacy_boot_arg_name("garnet-x", nullptr, 8), "null inputs are refused");

    // ---- wrapper behaviour
    unsigned v;
    g_args = "debug=0x100 garnet-stage=17"; g_log.clear();
    expect(kext_read("garnet-stage", v) && v == 17 && g_log.empty(), "new name only: read, nothing logged");
    g_args = "rdna4bringup=1 rdna4-stage=5"; g_log.clear();
    expect(kext_read("garnet", v) && v == 1, "legacy rdna4bringup is honoured when garnet is absent");
    expect(kext_read("garnet-stage", v) && v == 5, "legacy rdna4-stage is honoured when garnet-stage is absent");
    expect(g_log.find("Garnet: boot-arg rdna4-stage is deprecated; use garnet-stage") != std::string::npos &&
           g_log.find("Garnet: boot-arg rdna4bringup is deprecated; use garnet") != std::string::npos, "each legacy use is logged");
    g_args = "garnet-stage=17 rdna4-stage=5"; g_log.clear();
    expect(kext_read("garnet-stage", v) && v == 17, "both set: the new name wins");
    expect(g_log.find("Garnet: boot-arg garnet-stage overrides deprecated rdna4-stage") != std::string::npos, "both set: the stale legacy token is logged");
    g_args = "navi44-stage=5 garnet-other=1"; g_log.clear();
    expect(!kext_read("garnet-stage", v) && v == 0 && g_log.empty(), "neither set: absent, value untouched, nothing logged");
    g_args = "rdna4-m6"; g_log.clear();
    expect(kext_read("garnet-m6", v) && v == 1, "a valueless legacy flag is honoured like the new one");

    // ---- tree checks
    const std::string a = slurp(root + "/src/garnet/src/GarnetBootArgs.h"), b = slurp(root + "/src/garnet-accel/src/GarnetBootArgs.h");
    expect(!a.empty() && a == b, "the Garnet and GarnetAccel copies of GarnetBootArgs.h are identical");
    for (const char *mk : {"/src/garnet/Makefile", "/src/garnet-accel/Makefile"}) {
        const std::string m = slurp(root + mk);
        const size_t cx = m.find("CXXFLAGS :="), inc = m.find("-include src/GarnetBootArgs.h");
        expect(cx != std::string::npos && inc != std::string::npos && inc > cx && m.find("CXXFLAGS", cx + 1) > inc, std::string(mk) + " force-includes GarnetBootArgs.h in CXXFLAGS");
    }
    std::vector<std::string> files; walk(root + "/src/garnet/src", files); walk(root + "/src/garnet-accel/src", files);
    std::set<std::string> names; int cfiles = 0, redefs = 0;
    const std::regex lit("PE_parse_boot_argn\\(\\s*\"([^\"]+)\"");
    for (const auto &f : files) {
        if (f.find("GarnetBootArgs.h") != std::string::npos) continue;
        const std::string s = slurp(f);
        if (s.find("PE_parse_boot_argn") == std::string::npos) continue;
        if (f.size() > 2 && f.compare(f.size() - 2, 2, ".c") == 0) cfiles++;
        if (s.find("#define PE_parse_boot_argn") != std::string::npos || s.find("#undef PE_parse_boot_argn") != std::string::npos) redefs++;
        for (std::sregex_iterator it(s.begin(), s.end(), lit), end; it != end; ++it) names.insert((*it)[1]);
    }
    expect(cfiles == 0, "no C source reads a boot-arg (only C++ is force-included)");
    expect(redefs == 0, "no kext source redefines PE_parse_boot_argn");
    expect(names.size() >= 30, "the kexts' boot-arg reads were found (" + std::to_string(names.size()) + ")");
    for (const auto &n : names) expect(garnet_legacy_boot_arg_name(n.c_str(), o, sizeof(o)), "boot-arg " + n + " has a legacy rdna4 spelling");
    std::printf("bootargs_alias_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
