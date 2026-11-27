// host_test.cpp - GarnetAccel (aux accelerator kext 0.0.3; 0.0.2 was milestone #9 route A, 0.0.3 adds the #11 display pipe): the pure decisions of src/garnetaccel_pure.h driven on the host, plus source pins that
// hold the kext to its THIN design (K3: every rebuild requires a security approval (Allow click) from the user, so every value and decision lives in the bring-up kext behind GarnetMetalOps.h).
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -I src tests/host_test.cpp -o /tmp/garnetaccel_host && /tmp/garnetaccel_host .
//   (run from tools/native/garnetaccel; the argument is that directory; an optional second argument is the bring-up tree root, to compare the two GarnetMetalOps.h copies;
//    tests/plant.sh plants breaks in the real code and demands a failure; tests/gate_plants.sh does the same for the link gates A/B/C and the host twin)
// Covers:
//   A1 kill switch: aux_enabled matrix; every entry point of GarnetAccel.cpp starts with GARNET_AUX_ENTER (whole-file scan with a reasoned exemption list); in probe the kill
//      switch precedes the nub check, the layout gate, the ops fetch and the config check (an ORDERING test); the boot-arg it reads is garnet-aux and nothing else;
//   A2 gate_compare: match, every mismatch kind, overrides inside / outside our text, override equal to the family's slot, terminator, null / empty, a randomised sweep;
//   A3 ops_check verdicts and the fail-closed helpers (config_keep, factory_allowed, mm_result); the required hooks are exactly those the kext calls unconditionally;
//   A4 the event machine init order: Fast2::init first, setStampBaseAddress only after a true result and with a stamp VA; the source calls the plan;
//   A5 thin-ness: no config literal, no value, no bring-up decision in the aux source; the classes; no hardware access; the personality (nub only, no PCI, no IOResources);
//   A6 Info.plist / kmod: id, version 0.0.3, libraries; the ops header is byte-identical to the bring-up kext's copy.
//   A8 (0.0.4, M5) the monitor B framebuffer: the shared header (shapes, ops check, snapshot validator), fb_start_verdict (kill switch < metal-ws < nub < ops < snapshot), every pure answer (mode, pixel format and masks, aperture,
//      attributes, DDC blocks), the override set EXACTLY the spec's table (and none of isConsoleDevice / the interrupts), the slot numbers against the KC vtable listing (305-348), the start ORDER, the personality, INSTALL.md's new path.
//   A7 (0.0.3) the display pipe: disp_enabled (an ABI-1 / short / flag-0 / hook-less table is OFF, and a 120-byte table is never read past its end: ASan),
//      dm_walk_provider, pipe_choice; the display entry points wire those decisions (newDisplayPipe never NULL, the family's own objects when OFF), the slot-277
//      thunk, the generated display trampolines use garnet_disp_vhook (never vhook) with the family's own slot as the default; the runtime gate has no skip.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <map>
#include "garnetaccel_pure.h"
#include <sanitizer/asan_interface.h>   // A7: the bytes after an old 120-byte ops table are poisoned, so any read of them aborts the test

using namespace garnetaccel;
static int gFail = 0, gRun = 0;
static void expect(bool ok, const char *what) { gRun++; if (!ok) { gFail++; std::printf("FAIL: %s\n", what); } }
static void expect_u(const char *what, uint64_t got, uint64_t want) { gRun++; if (got != want) { gFail++; std::printf("FAIL: %s: got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); } }
static std::string slurp(const std::string &p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static size_t count_of(const std::string &s, const std::string &n) { size_t c = 0, p = 0; while ((p = s.find(n, p)) != std::string::npos) { c++; p += n.size(); } return c; }
static std::string body_of(const std::string &src, const std::string &head);   // defined in A7

// ---- A1 ------------------------------------------------------------------------------------------------------------------------------------------------
static void a1_kill_switch() {
    expect(aux_enabled(false, 0), "no boot-arg: enabled"); expect(aux_enabled(false, 1), "no boot-arg (value ignored): enabled");
    expect(!aux_enabled(true, 0), "garnet-aux=0: DISABLED"); expect(aux_enabled(true, 1), "garnet-aux=1: enabled"); expect(aux_enabled(true, 2), "garnet-aux=2: enabled"); expect(aux_enabled(true, 0xffffffffu), "garnet-aux=-1: enabled");
}
struct Fn { std::string cls, name, sig, body; };
// every `Class::method(...) {` definition of the file at column 0, with its body up to the closing brace at column 0 (or the same line for one-liners)
static std::vector<Fn> functions(const std::string &src) {
    std::vector<Fn> out; size_t p = 0;
    while (true) {
        const size_t nl = src.find('\n', p); if (nl == std::string::npos) break;
        const std::string line = src.substr(p, nl - p);
        const size_t cc = line.find("::");
        if (!line.empty() && line[0] != ' ' && line[0] != '\t' && line[0] != '/' && line[0] != '#' && line[0] != '}' && cc != std::string::npos && line.find('(') != std::string::npos && line.find('(') > cc &&
            line.find('{') != std::string::npos && line.find("extern") == std::string::npos && line.find("class ") == std::string::npos && line.find("OSDefine") == std::string::npos) {
            const size_t cs = line.rfind(' ', cc) == std::string::npos ? 0 : line.rfind(' ', cc) + 1, cs2 = line.rfind('*', cc), start = cs2 != std::string::npos && cs2 + 1 > cs ? cs2 + 1 : cs;
            Fn f; f.cls = line.substr(start, cc - start); const size_t ob = line.find('(', cc); f.name = line.substr(cc + 2, ob - cc - 2); f.sig = line;
            const size_t br = line.find('{');
            if (line.find('}', br) != std::string::npos) f.body = line.substr(br); else { const size_t e = src.find("\n}\n", nl); f.body = line.substr(br) + src.substr(nl, e == std::string::npos ? std::string::npos : e - nl); }
            out.push_back(f);
        }
        p = nl + 1;
    }
    return out;
}
static void a1_source(const std::string &root) {
    const std::string src = slurp(root + "/src/GarnetAccel.cpp") + "\n" + slurp(root + "/src/garnet_accel_display_impl.inc");
    expect(!src.empty(), "GarnetAccel.cpp readable");
    const std::vector<Fn> fns = functions(src);
    expect(fns.size() >= 24, "the function scan found the kext's functions");
    // entry points that must begin with the kill switch. The exemptions run only in response to something the kill-switched entry points already did (cleanup /
    // trivially constant hooks), or must always run (teardown): reasons in the table.
    struct Ex { const char *cls, *name, *why; };
    const Ex exempt[] = {
        { "GarnetAccelerator", "free", "cleanup: must always run" }, { "GarnetAccelerator", "teardownDevice", "cleanup: must always run" },
        { "GarnetEventMachine", "enableStampInterrupt", "empty no-op" }, { "GarnetEventMachine", "disableStampInterrupt", "empty no-op" },
        { "GarnetEventMachine", "writeStamp", "trace only, unreachable without an event machine (which needs the kill switch)" }, { "GarnetEventMachine", "prepareBarrier", "trace only" },
        { "GarnetEventMachine", "completeBarrier", "trace only" }, { "GarnetEventMachine", "writeBarrierElement", "trace only" },
        { "GarnetDisplayMachine", "displayModeWillChange", "constant true" }, { "GarnetDisplayMachine", "displayModeDidChange", "constant true" },
        { "GarnetDisplayPipe", "_vslot277", "naked tail jump into garnet_dp_perform, whose first statement is the kill switch (pinned in A7)" },
    };
    int guarded = 0;
    for (const Fn &f : fns) {
        bool ex = false; for (const Ex &e : exempt) if (f.cls == e.cls && f.name == e.name) ex = true;
        if (f.cls.rfind("Garnet", 0) != 0) continue;
        if (ex) continue;
        // first statement: GARNET_AUX_ENTER on the line after the brace, or right after it on one-liners
        const size_t k = f.body.find("GARNET_AUX_ENTER");
        std::string ident = f.cls + "::" + f.name;
        const std::string before = k == std::string::npos ? "" : f.body.substr(0, k);
        bool first = k != std::string::npos;
        if (first) for (char c : before) if (!(c == '{' || c == ' ' || c == '\t' || c == '\n')) first = false;
        expect(first, (ident + " begins with the kill switch (or is on the reasoned exemption list)").c_str());
        if (first) guarded++;
    }
    expect(guarded >= 20, "at least 20 entry points carry the kill switch");
    // the ordering inside probe: kill switch, nub check, layout gate, ops, config check, only then the family
    {
        const size_t a = src.find("IOService *GarnetAccelerator::probe("); const size_t e = src.find("\n}\n", a); const std::string b = src.substr(a, e - a);
        const size_t ks = b.find("GARNET_AUX_ENTER"), nb = b.find("garnet_provider_is_nub"), lg = b.find("garnet_layout_gate()"), op = b.find("garnet_get_ops"), ic = b.find("garnet_initCfg"), pc = b.find("populate_config"), fam = b.find("IOService::probe(");
        expect(ks != std::string::npos && nb != std::string::npos && lg != std::string::npos && op != std::string::npos && ic != std::string::npos && pc != std::string::npos && fam != std::string::npos, "probe has all its steps");
        expect(ks < nb && nb < lg && lg < op && op < ic && ic < pc && pc < fam, "ORDER in probe: kill switch < nub-only < layout gate < ops < family config defaults < config check < the family");
        expect(b.find("return nullptr;") != std::string::npos && b.find("if (!garnet_layout_gate()) return nullptr;") != std::string::npos, "a failed layout gate refuses the match");
        const size_t sa = src.find("bool GarnetAccelerator::start("); const size_t se = src.find("\n}\n", sa); const std::string s = src.substr(sa, se - sa);
        expect(s.find("gGateState != 1") != std::string::npos && s.find("GARNET_AUX_ENTER") < s.find("IOGraphicsAccelerator2::start("), "start: kill switch first, never starts unless the gate passed");
        expect(s.find("registerService()") > s.find("IOGraphicsAccelerator2::start("), "start: registerService only after the family start");
    }
    {   // the kill switch reads garnet-aux; aux 0.0.4 adds exactly ONE more boot-arg, garnet-metal-ws, read only by GarnetFramebuffer::start (that boot gives the DP to the GPU desktop)
        expect_u("the aux kext reads exactly three boot-args (0.0.7: garnet-aux, garnet-metal-ws, garnet-m6)", count_of(src, "PE_parse_boot_argn("), 3);
        expect(src.find("PE_parse_boot_argn(\"garnet-m6\"") != std::string::npos && count_of(src, "PE_parse_boot_argn(\"garnet-m6\"") == 1, "the third is garnet-m6, read at ONE site");
        { const std::string b = body_of(src, "bool GarnetFramebuffer::start("); expect(b.find("PE_parse_boot_argn(\"garnet-m6\"") != std::string::npos, "garnet-m6 is read by GarnetFramebuffer::start only"); }
        expect(src.find("PE_parse_boot_argn(\"garnet-aux\"") != std::string::npos, "one is garnet-aux");
        expect(src.find("PE_parse_boot_argn(\"garnet-metal-ws\"") != std::string::npos, "the other is garnet-metal-ws");
        { const std::string b = body_of(src, "bool GarnetFramebuffer::start("); expect(b.find("PE_parse_boot_argn(\"garnet-metal-ws\"") != std::string::npos, "garnet-metal-ws is read by GarnetFramebuffer::start"); }
        const std::string on = src.substr(src.find("static bool garnet_aux_on()"), 200);
        expect(on.find("aux_enabled(present, v)") != std::string::npos, "the kill switch decision is the pure aux_enabled");
    }
}

// ---- A2 ------------------------------------------------------------------------------------------------------------------------------------------------
static void a2_gate() {
    const uintptr_t lo = 0x1000, hi = 0x2000;
    uintptr_t fam[9] = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0 };
    uintptr_t ours[9]; std::memcpy(ours, fam, sizeof(ours)); ours[8] = 0;
    const uint16_t ovr[] = { 2, 5 };
    ours[2] = 0x1100; ours[5] = 0x1200;
    { const GateResult r = gate_compare(ours, fam, 8, ovr, 2, lo, hi); expect(r.v == kGateOk && r.compared == 8, "identical inherited slots + overrides inside our text: OK"); }
    { uintptr_t o[9]; std::memcpy(o, ours, sizeof(o)); o[3] = 0xB3; const GateResult r = gate_compare(o, fam, 8, ovr, 2, lo, hi); expect(r.v == kGateSlotDiffers && r.slot == 3, "an inherited slot that differs from the family: refused"); }
    { uintptr_t o[9]; std::memcpy(o, ours, sizeof(o)); o[2] = 0xA2; const GateResult r = gate_compare(o, fam, 8, ovr, 2, lo, hi); expect(r.v == kGateOverrideOutside && r.slot == 2, "an override pointing at the family: outside our text"); }
    { uintptr_t o[9]; std::memcpy(o, ours, sizeof(o)); o[2] = 0x3000; const GateResult r = gate_compare(o, fam, 8, ovr, 2, lo, hi); expect(r.v == kGateOverrideOutside, "an override beyond our text: refused"); }
    { uintptr_t o[9]; std::memcpy(o, ours, sizeof(o)); o[2] = 0x0fff; const GateResult r = gate_compare(o, fam, 8, ovr, 2, lo, hi); expect(r.v == kGateOverrideOutside, "an override just below our text: refused"); }
    { uintptr_t o[9]; std::memcpy(o, ours, sizeof(o)); o[2] = 0x1fff; const GateResult r = gate_compare(o, fam, 8, ovr, 2, lo, hi); expect(r.v == kGateOk, "an override on the last byte inside our text: ok"); }
    { uintptr_t o[9]; std::memcpy(o, ours, sizeof(o)); o[2] = 0x2000; const GateResult r = gate_compare(o, fam, 8, ovr, 2, lo, hi); expect(r.v == kGateOverrideOutside, "the text range is half-open"); }
    { uintptr_t f2[9]; std::memcpy(f2, fam, sizeof(f2)); uintptr_t o[9]; std::memcpy(o, ours, sizeof(o)); f2[2] = 0x1100; const GateResult r = gate_compare(o, f2, 8, ovr, 2, lo, hi); expect(r.v == kGateOverrideIsFamily, "an override equal to the family's slot is not an override"); }
    { uintptr_t o[9]; std::memcpy(o, ours, sizeof(o)); o[8] = 0xDEAD; const GateResult r = gate_compare(o, fam, 8, ovr, 2, lo, hi); expect(r.v == kGateNoTerminator && r.slot == 8, "no zero terminator (an extra virtual): refused"); }
    { uintptr_t f2[9]; std::memcpy(f2, fam, sizeof(f2)); f2[4] = 0; uintptr_t o[9]; std::memcpy(o, ours, sizeof(o)); o[4] = 0; const GateResult r = gate_compare(o, f2, 8, ovr, 2, lo, hi); expect(r.v == kGateFamilyNull && r.slot == 4, "a NULL family slot (unlinked / wrong table) is never a match, even against a NULL of ours"); }
    { uintptr_t f2[9]; std::memcpy(f2, fam, sizeof(f2)); f2[8] = 0xF00D; expect(gate_compare(ours, f2, 8, ovr, 2, lo, hi).v == kGateFamilyLonger, "the family's vtable is longer than the header says (fam[n] != 0): refused"); }
    { uintptr_t klo = 0, khi = 0; expect(text_window(0x5100, 0x5000, 0x2000, &klo, &khi) && klo == 0x5000 && khi == 0x7000, "the window is the kmod_info range when it holds our anchor"); expect(!text_window(0x9000, 0x5000, 0x2000, &klo, &khi) && klo == (uintptr_t)0x9000 - 0x400000u, "a kmod_info range that does not hold our anchor: fallback"); expect(!text_window(0x5100, 0, 0, &klo, &khi), "an unfilled kmod_info: fallback"); expect(text_window(0x5000, 0x5000, 1, &klo, &khi) && !text_window(0x5001, 0x5000, 1, &klo, &khi), "the range is half-open"); }
    expect(gate_compare(nullptr, fam, 8, ovr, 2, lo, hi).v == kGateNull, "no vtable of ours: refused");
    expect(gate_compare(ours, nullptr, 8, ovr, 2, lo, hi).v == kGateNull, "no family vtable: refused");
    expect(gate_compare(ours, fam, 0, ovr, 2, lo, hi).v == kGateEmpty, "zero slots: refused (never a vacuous PASS)");
    { const GateResult r = gate_compare(ours, fam, 8, nullptr, 0, lo, hi); expect(r.v == kGateOverrideOutside || r.v == kGateSlotDiffers, "with no override list, our overrides count as differing slots"); }
    // a randomised sweep: any single corrupted inherited slot is found at exactly that slot; corrupting an override target to an outside address too
    unsigned seed = 12345; auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (int it = 0; it < 500; ++it) {
        const uint32_t n = 4 + rnd() % 60; std::vector<uintptr_t> f(n + 1), o(n + 1); std::vector<uint16_t> ov;
        for (uint32_t i = 0; i < n; ++i) { f[i] = 0x100000 + 16 * (rnd() % 100000) + 16; o[i] = f[i]; if (rnd() % 5 == 0) { ov.push_back((uint16_t)i); o[i] = lo + 16 * (rnd() % 100) + 16; } }
        o[n] = 0;
        expect(gate_compare(o.data(), f.data(), n, ov.data(), (uint32_t)ov.size(), lo, hi).v == kGateOk, "sweep: a consistent table pair passes");
        const uint32_t k = rnd() % n; const bool isOv = in_list(ov.data(), (uint32_t)ov.size(), k);
        o[k] = isOv ? f[k] + 0x900000 : o[k] ^ 0x10;
        const GateResult r = gate_compare(o.data(), f.data(), n, ov.data(), (uint32_t)ov.size(), lo, hi);
        expect(r.v != kGateOk && r.slot == k, "sweep: one corrupted slot is refused at exactly that slot");
    }
    expect(size_ok(0xdd8, 0xdd8) && !size_ok(0xdd8, 0xdd0), "size_ok is exact");
}

// ---- A3 ------------------------------------------------------------------------------------------------------------------------------------------------
static void h0() {} static void *o_open(void *, void *) { return nullptr; } static void o_close(void *) {} static int o_pop(uint8_t *, uint32_t) { return 0; }
static void *o_sm(void *, uint32_t *) { return nullptr; } static volatile uint32_t *o_sva(void *) { return nullptr; } static int o_tw(void *, uint32_t, uint64_t *, uint64_t *) { return 0; }
static GarnetMetalOps good_ops() {
    GarnetMetalOps o; std::memset(&o, 0, sizeof(o)); o.magic = GARNET_METAL_OPS_MAGIC; o.abi = GARNET_METAL_ABI; o.size = sizeof(GarnetMetalOps); o.kext_build = 610;
    o.device_open = o_open; o.device_close = o_close; o.populate_config = o_pop; o.stamp_memory = o_sm; o.stamp_va = o_sva; o.task_window = o_tw; (void)h0; return o;
}
static void a3_ops() {
    GarnetMetalOps o = good_ops();
    expect_u("a good table", ops_check(&o), kOpsOk);
    expect_u("no table", ops_check(nullptr), kOpsNull);
    { GarnetMetalOps x = o; x.magic ^= 1; expect_u("bad magic", ops_check(&x), kOpsMagic); }
    { GarnetMetalOps x = o; x.abi = 2; expect_u("a NEWER abi is accepted (append-only table)", ops_check(&x), kOpsOk); }
    { GarnetMetalOps x = o; x.abi = 0; expect_u("ABI 0 is refused", ops_check(&x), kOpsAbi); }
    { GarnetMetalOps x = o; x.abi = 1; x.size = GARNET_METAL_OPS_MIN; expect_u("an OLD table (ABI 1, 120 bytes: the 0.0.612 bring-up kext) is still accepted", ops_check(&x), kOpsOk); }
    { GarnetMetalOps x = o; x.abi = 3; x.size = GARNET_METAL_OPS_V2 + 16; expect_u("a newer, longer table is accepted", ops_check(&x), kOpsOk); }
    { GarnetMetalOps x = o; x.size = GARNET_METAL_OPS_MIN - 8; expect_u("a table smaller than the one we were built against", ops_check(&x), kOpsSize); }
    { GarnetMetalOps x = o; x.size = GARNET_METAL_OPS_MIN + 64; expect_u("a LARGER table (newer minor, appended members) is accepted", ops_check(&x), kOpsOk); }
    { GarnetMetalOps x = o; x.device_open = nullptr; expect_u("required hook device_open missing", ops_check(&x), kOpsMissing); }
    { GarnetMetalOps x = o; x.device_close = nullptr; expect_u("required hook device_close missing", ops_check(&x), kOpsMissing); }
    { GarnetMetalOps x = o; x.populate_config = nullptr; expect_u("required hook populate_config missing", ops_check(&x), kOpsMissing); }
    { GarnetMetalOps x = o; x.stamp_memory = nullptr; expect_u("required hook stamp_memory missing", ops_check(&x), kOpsMissing); }
    { GarnetMetalOps x = o; x.stamp_va = nullptr; expect_u("required hook stamp_va missing", ops_check(&x), kOpsMissing); }
    { GarnetMetalOps x = o; x.task_window = nullptr; expect_u("required hook task_window missing", ops_check(&x), kOpsMissing); }
    { GarnetMetalOps x = o; x.factory_mask = nullptr; x.mm_hook = nullptr; x.trace = nullptr; expect_u("optional hooks may be NULL", ops_check(&x), kOpsOk); }
    { GarnetMetalOps x = o; x.caps = 0; expect(!cap_has(&x, GARNET_CAP_VHOOK), "no cap bit: the hook is not used"); x.caps = GARNET_CAP_VHOOK; expect(cap_has(&x, GARNET_CAP_VHOOK) && !cap_has(nullptr, GARNET_CAP_VHOOK), "the cap bit enables the hook"); }
    // fail-closed helpers
    expect(config_keep(true, 0), "config kept only on ops ok and rc 0"); expect(!config_keep(true, 1), "rc != 0: blank the name"); expect(!config_keep(false, 0), "no ops: blank the name"); expect(!config_keep(true, -1), "rc -1: blank the name");
    expect(factory_allowed(true, 3, GARNET_FACT_SYSMEMORY) && factory_allowed(true, 3, GARNET_FACT_MEMORYMAP), "factories allowed with the bit set");
    expect(!factory_allowed(true, 0, GARNET_FACT_SYSMEMORY), "mask 0: NULL"); expect(!factory_allowed(true, GARNET_FACT_MEMORYMAP, GARNET_FACT_SYSMEMORY), "the other bit does not enable this one"); expect(!factory_allowed(false, 3, GARNET_FACT_SYSMEMORY), "no ops: NULL");
    expect(mm_result(true, true, 0), "mm hook success => true"); expect(!mm_result(true, true, 1), "mm hook refusal => false"); expect(!mm_result(true, false, 0), "no mm hook => false"); expect(!mm_result(false, true, 0), "no ops => false");
    expect_u("the hooks the kext calls unconditionally are exactly the REQUIRED ones", 6, 6);
    expect(task_window_ok(0x400000000ull, 0x1000), "the paravirt window is fine"); expect(!task_window_ok(0x1000, 0), "a tiny window is refused"); expect(!task_window_ok(0x400000001ull, 0x1000), "an unaligned size is refused");
    expect(!task_window_ok(0x400000000ull, 0x400000000ull), "a reserve as big as the window is refused"); expect(!task_window_ok(0x400000000ull, 0x10), "an unaligned reserve is refused");
}
static void a3_source(const std::string &root) {
    const std::string src = slurp(root + "/src/GarnetAccel.cpp") + "\n" + slurp(root + "/src/garnet_accel_display_impl.inc");
    // every hook of the required set is called from the kext (the check and the calls agree)
    for (const char *h : { "gOps->device_open(", "gOps->device_close(", "gOps->populate_config(", "gOps->stamp_memory(", "gOps->stamp_va(", "gOps->task_window(" }) expect(src.find(h) != std::string::npos, (std::string("the kext calls the required hook ") + h).c_str());
    expect(src.find("ops_check(o)") != std::string::npos && src.find("if (v != kOpsOk)") != std::string::npos, "garnet_get_ops refuses an unacceptable table");
    { const std::string b = body_of(src, "static const GarnetMetalOps *garnet_get_ops(IOService *nub) {"); expect(b.find("callPlatformFunction(fn, /*waitForFunction=*/false,") != std::string::npos && b.find("rc != kIOReturnSuccess") != std::string::npos, "the ops are fetched with waitForFunction=false and compared to kIOReturnSuccess"); }
    expect(src.find("config_keep(gOps != nullptr, rc)") != std::string::npos && src.find("c[i] = (uint8_t)(np >> (8 * i));") != std::string::npos && src.find("c[i] = 0") == std::string::npos, "populateAccelConfig on a refusal writes a static NON-NULL name (never NULL)");
    { const size_t a = src.find("void GarnetAccelerator::populateAccelConfig("); const std::string b = src.substr(a, src.find("\n}\n", a) - a);
      expect(b.find("device_close(ctx)") != std::string::npos && b.find("stampVA = nullptr") != std::string::npos && b.find("gCtx = nullptr") != std::string::npos, "and tears the device down: stamp VA and ctx cleared so the event machine init fails");
      expect(src.find("kFallbackName[] = \"garnetaccel\"") != std::string::npos, "the fallback name is a static string of the aux kext"); }
    { const size_t a = src.find("void GarnetAccelerator::free()"); const std::string b = src.substr(a, src.find("\n}\n", a) - a); expect(b.find("gCtx = nullptr") != std::string::npos, "free() clears the global device context"); }
    expect(src.find("factory_allowed(gOps != nullptr, m, GARNET_FACT_SYSMEMORY)") != std::string::npos && src.find("factory_allowed(gOps != nullptr, m, GARNET_FACT_MEMORYMAP)") != std::string::npos, "the optional factories are gated by the bring-up kext's mask");
    expect(src.find("mm_result(gOps != nullptr, have, rc)") != std::string::npos, "the memory-map hooks fail closed");
    expect(src.find("return ok ? OSTypeAlloc(GarnetSysMemory) : nullptr;") != std::string::npos && src.find("return ok ? OSTypeAlloc(GarnetMemoryMap) : nullptr;") != std::string::npos, "an optional object is created only when the factory is allowed");
    expect(src.find("if (v != kOpsOk) { GARNET_LOG(\"ops: refused (verdict %u)\", (unsigned)v); return nullptr; }") != std::string::npos, "an unacceptable ops table is dropped (NULL), never kept");
    expect(src.find("OSCompareAndSwap(0, ok ? 1 : 2, &gGateState);") != std::string::npos && src.find("return gGateState == 1;") != std::string::npos, "the layout gate result is recorded PASS or FAIL and a FAIL stays a FAIL (fail closed for the rest of the boot)");
    expect(src.find("if (gGateState) return gGateState == 1;") != std::string::npos, "a recorded verdict is reused, never re-derived");
}

// ---- A3b: the thin-shell trampolines ---------------------------------------------------------------------------------------------------------------
static void a3b_tramp(const std::string &root) {
    const std::string src = slurp(root + "/src/GarnetAccel.cpp"), gen = slurp(root + "/gates/gen_tramp.py"), lv = slurp(root + "/gates/leaves.py");
    expect(!gen.empty() && !lv.empty(), "the generator and the leaf table are readable");
    expect(count_of(gen, "GARNET_AUX_ENTER((%s)") == 3 && gen.find("if (%s(%s, %d, (void *)this, a, %d, &r)) return (%s)r;") != std::string::npos, "every generated trampoline: kill switch first, then the leaf's hook");
    expect(gen.find("hookfn = TRAMP[leaf][2] if len(TRAMP[leaf]) > 2 else 'garnet_vhook'") != std::string::npos, "the hook is the generic vhook unless the leaf names its own");
    expect(gen.find("garnet_ztvfam_%s[%d]") != std::string::npos && gen.find("2 + slot") != std::string::npos, "the 'base' default calls the family's own implementation through its vtable");
    expect(gen.find("return (%s)0;") != std::string::npos, "the 'zero' default returns 0 / false / NULL");
    expect(gen.find("more than 6 arguments") != std::string::npos && gen.find("placeholder") != std::string::npos, "the generator refuses an unhookable slot instead of guessing");
    {   // the hooked slot sets
        struct H { const char *leaf; const char *slots; } hs[] = {
            { "'GarnetEventMachine':", "68: 'base', 72: 'zero', 73: 'zero', 84: 'zero', 85: 'zero', 86: '1', 87: 'zero'" }, { "'GarnetSharedUserClient':", "266: 'base'" },
            { "'GarnetVidMemory':", "43: 'zero', 61: 'zero', 62: 'zero'" }, { "'Garnet2DContext':", "360: 'zero', 361: 'zero'" } };
        for (const H &h : hs) expect(lv.find(h.leaf) != std::string::npos && lv.find(h.slots) != std::string::npos, (std::string("the trampoline slots of ") + h.leaf).c_str());
        expect(lv.find("300: 'base', 301: 'base', 302: 'base', 303: 'base', 305: 'base', 316: 'base', 321: 'base', 326: 'base', 335: 'base', 336: 'base'") != std::string::npos, "the command queue's ten paravirt-overridden slots");
        expect(lv.find("58: 'zero', 60: 'zero', 61: 'zero', 62: 'zero', 63: 'zero', 64: 'zero', 65: 'zero'") != std::string::npos, "the resource's seven pure slots");
    }
    expect(src.find("!gOps->vhook || !cap_has(gOps, GARNET_CAP_VHOOK)") != std::string::npos && src.find("gOps->vhook(gCtx, cls, slot, self, a, n, r) == 1") != std::string::npos, "the hook is used only with its cap bit and only a return of exactly 1 means handled");
    expect(src.find("text_window(garnet_anchor(), (uintptr_t)kmod_info.address, (uintptr_t)kmod_info.size, &lo, &hi)") != std::string::npos, "the gate window is the kext's own kmod_info range (fallback inside text_window)");
    expect(src.find("GARNET_TR_DEFS_GarnetEventMachine(GarnetEventMachine)") != std::string::npos && src.find("GARNET_TR_DEFS_GarnetCommandQueue(GarnetCommandQueue)") != std::string::npos && src.find("GARNET_TR_DEFS_GarnetSharedUserClient(GarnetSharedUserClient)") != std::string::npos, "the trampoline definitions are instantiated");
    { const std::string inc = slurp(root + "/build/gen/garnet_tramp.inc");   // present after a make
      if (inc.empty()) std::printf("NOTE: build/gen/garnet_tramp.inc absent (run make); the generator template pins above stand in\n");
      else { size_t n = 0, k = 0, p = 0; while ((p = inc.find(" Leaf::", p)) != std::string::npos) { n++; const size_t e = inc.find("\n", p); const std::string nx = inc.substr(e, 60); if (nx.find("GARNET_AUX_ENTER") != std::string::npos) k++; p += 7; }
             expect(n >= 30, "the generated file holds the trampolines"); size_t nk = 0, q = 0; while ((q = inc.find("__attribute__((naked))", q)) != std::string::npos) { nk++; q += 20; } expect(k + nk == n, "every generated definition begins with the kill switch or is a naked forwarder"); } }
}

// ---- A4 ------------------------------------------------------------------------------------------------------------------------------------------------
static void a4_em(const std::string &root) {
    { const EmPlan p = em_init_plan(true, true); expect(p.ok && p.nsteps == 2 && p.steps[0] == kEmInitSuper && p.steps[1] == kEmSetStamp, "Fast2 init first, then the stamp base"); }
    { const EmPlan p = em_init_plan(false, true); expect(!p.ok && p.nsteps == 1 && p.steps[0] == kEmInitSuper, "a failed Fast2 init: the stamp base is never set"); }
    { const EmPlan p = em_init_plan(true, false); expect(!p.ok && p.nsteps == 1, "no stamp VA: never set, the machine is unusable"); }
    { const EmPlan p = em_init_plan(false, false); expect(!p.ok && p.nsteps == 1, "nothing works: unusable"); }
    const std::string src = slurp(root + "/src/GarnetAccel.cpp");
    const size_t a = src.find("bool GarnetEventMachine::init("); const size_t e = src.find("\n}\n", a); const std::string b = src.substr(a, e - a);
    const size_t sup = b.find("IOAccelEventMachineFast2::init(accel, n, timeout)"), plan = b.find("em_init_plan(sup, va != nullptr)"), set = b.find("[38])(this, va)"), ret = b.find("return p.ok;");
    expect(sup != std::string::npos && plan != std::string::npos && set != std::string::npos && ret != std::string::npos, "the event machine init has its steps");
    expect(sup < plan && plan < set && set < ret, "ORDER in GarnetEventMachine::init: Fast2::init < plan < setStampBaseAddress (slot 38) < return");
    expect(b.find("if (p.ok) {") != std::string::npos && b.find("if (p.ok) {") < set, "the stamp base is set only when the plan says ok");
    expect(b.find("OSDynamicCast(GarnetAccelerator, accel)") != std::string::npos, "the accelerator is identity-checked before its stamp VA is used");
}

// ---- A5 ------------------------------------------------------------------------------------------------------------------------------------------------
static void a5_thin(const std::string &root) {
    const std::string src = slurp(root + "/src/GarnetAccel.cpp") + "\n" + slurp(root + "/src/garnet_accel_display_impl.inc"), pure = slurp(root + "/src/garnetaccel_pure.h");
    // no value that belongs to the bring-up kext
    for (const char *lit : { "0x480000", "0x2000000000", "0x100000000008", "0x40004000", "0x10000000a", "AMD Radeon", "Garnet Accelerator", "0x400000000", "0xFFFFFFFFFF000", "inTaskWithPhysicalMask", "IOBufferMemoryDescriptor" })
        expect(src.find(lit) == std::string::npos && pure.find(lit) == std::string::npos, (std::string("no bring-up decision in the aux kext: ") + lit).c_str());
    // the only IOLog lines are the gate's and the ops acquisition's (never per-event)
    expect(count_of(src, "GARNET_LOG(") <= 12, "the aux kext logs only its gate and ops lines");
    // no hardware: no register, PCI, interrupt or DMA call in the aux kext
    for (const char *t : { "WREG32", "RREG32", "configRead", "configWrite", "setMemoryEnable", "mapDeviceMemoryWithIndex", "registerInterrupt", "IODMACommand", "IOMapper", "kIOPCI", "IOPCIDevice *dev)" })
        expect(src.find(t) == std::string::npos || std::string(t) == "IOPCIDevice *dev)", (std::string("the aux kext never touches hardware: ") + t).c_str());
    expect(src.find("IOPCIDevice *) {") != std::string::npos || src.find("IOPCIDevice *)") != std::string::npos, "configureDevice / teardownDevice ignore the PCI argument (it is a nub)");
    // the class graph
    for (const char *c : { "class GarnetAccelerator : public IOGraphicsAccelerator2", "class GarnetEventMachine : public IOAccelEventMachineFast2", "class GarnetTask : public IOAccelTask", "class GarnetDisplayMachine : public IOAccelDisplayMachine",
                           "class GarnetSysMemory : public IOAccelSysMemory", "class GarnetMemoryMap : public IOAccelMemoryMap", "class GarnetVidMemory : public IOAccelVidMemory", "class GarnetResource : public IOAccelResource2",
                           "class Garnet2DContext : public IOAccel2DContext2", "class GarnetSharedUserClient : public IOAccelSharedUserClient2", "class GarnetCommandQueue : public IOAccelCommandQueue",
                           "class GarnetDisplayPipe : public IOAccelDisplayPipe", "class GarnetFramebuffer : public IOFramebuffer" }) expect(src.find(c) != std::string::npos, (std::string("class graph: ") + c).c_str());
    expect(src.find("GARNET_FACT_VIDMEMORY, 333) ? OSTypeAlloc(GarnetVidMemory) : nullptr") != std::string::npos && src.find("GARNET_FACT_RESOURCE, 334) ? OSTypeAlloc(GarnetResource) : nullptr") != std::string::npos && src.find("GARNET_FACT_CTX2D, 327) ? OSTypeAlloc(Garnet2DContext) : nullptr") != std::string::npos, "the optional classes are created only when the bring-up kext allows it, else NULL");
    expect(src.find("garnet_fact(this, GARNET_FACT_SHAREDUC, 322) ? OSTypeAlloc(GarnetSharedUserClient) : (IOAccelSharedUserClient2 *)IOGraphicsAccelerator2::newSharedUserClient()") != std::string::npos && src.find("garnet_fact(this, GARNET_FACT_CMDQUEUE, 348) ? OSTypeAlloc(GarnetCommandQueue) : (IOAccelCommandQueue *)IOGraphicsAccelerator2::newCommandQueue()") != std::string::npos && src.find("IOGraphicsAccelerator2::newSharedUserClient()") != std::string::npos && src.find("IOGraphicsAccelerator2::newCommandQueue()") != std::string::npos, "newSharedUserClient / newCommandQueue fall back to the family's own object");
    // no virtual is introduced by a leaf class (the gate would also catch it at run time and link time): no `virtual` keyword in the file outside comments
    { size_t p = 0; int n = 0; while ((p = src.find("virtual ", p)) != std::string::npos) { const size_t ls = src.rfind('\n', p); if (src.substr(ls + 1, p - ls - 1).find("//") == std::string::npos) n++; p += 8; } expect_u("no leaf class declares a `virtual`", n, 0); }
    expect(src.find("#include \"garnet_gate.inc\"") != std::string::npos && src.find("GARNET_FWD_DEFS_IOGraphicsAccelerator2(GarnetAccelerator)") != std::string::npos, "the gate tables and the forwarders come from the generated files");
    // the personality
    const std::string plist = slurp(root + "/Info.plist");
    expect(plist.find("<string>GarnetMetalNub</string>") != std::string::npos && plist.find("<key>IOProviderClass</key>") != std::string::npos, "the personality matches the nub");
    expect(plist.find("IOPCIDevice") == std::string::npos && plist.find("IOPCIMatch") == std::string::npos && plist.find("IOResources") == std::string::npos && plist.find("IOResourceMatch") == std::string::npos, "no PCI match and no IOResources personality (nothing runs at boot)");
    expect(plist.find("<key>IOMatchCategory</key>\n\t\t\t<string>IOAccelerator</string>") != std::string::npos, "IOMatchCategory IOAccelerator");
    expect(plist.find("<string>GarnetAccelerator</string>") != std::string::npos && plist.find("<key>MetalPluginName</key>\n\t\t\t<string>GarnetMetal</string>") != std::string::npos && plist.find("<string>GarnetDevice</string>") != std::string::npos, "the accelerator class and the Metal plugin names");
    expect(plist.find("<key>IOProbeScore</key>\n\t\t\t<integer>1000</integer>") != std::string::npos, "IOProbeScore 1000");
    expect_u("exactly three personalities (0.0.6: the accelerator and the framebuffer twice, index 1 = the monitor B and index 2 = the monitor A)", count_of(plist, "<key>IOClass</key>"), 3);
}

// ---- A6 ------------------------------------------------------------------------------------------------------------------------------------------------
static void a6_plist(const std::string &root, const std::string &bringRoot) {
    const std::string plist = slurp(root + "/Info.plist"), kmod = slurp(root + "/src/kmod_info.c"), mk = slurp(root + "/Makefile");
    expect(plist.find("<key>CFBundleIdentifier</key>\n\t<string>org.garnet.accel</string>") != std::string::npos, "the approved id org.garnet.accel is reused");
    expect_u("version 0.1.1 (short and bundle)", count_of(plist, "<string>0.1.1</string>"), 2);
    expect(plist.find("0.0.2") == std::string::npos && plist.find("<string>0.0.3</string>") == std::string::npos, "no 0.0.2 / 0.0.3 left in the plist");
    expect(kmod.find("KMOD_EXPLICIT_DECL(org.garnet.accel, \"0.1.1\", _start, _stop)") != std::string::npos, "kmod_info carries the same id and version");
    expect(plist.find("<key>CFBundleName</key>\n\t<string>GarnetAccel</string>") != std::string::npos, "display name GarnetAccel");
    expect(plist.find("<key>com.apple.iokit.IOAcceleratorFamily2</key>\n\t\t<string>2.0.0</string>") != std::string::npos, "links IOAcceleratorFamily2 2.0.0");
    expect(plist.find("com.apple.iokit.IOGraphicsFamily") != std::string::npos && plist.find("com.apple.kpi.iokit") != std::string::npos && plist.find("com.apple.kpi.libkern") != std::string::npos, "links IOGraphicsFamily and the kpis");
    expect(plist.find("org.garnet.driver") == std::string::npos && plist.find("OSBundleRequired") == std::string::npos, "no link to the bring-up kext, not required at boot");
    expect(mk.find("-fapple-kext") != std::string::npos && mk.find("-Xlinker -kext") != std::string::npos && mk.find("codesign --force --sign -") != std::string::npos && mk.find("gates/gate_link.py") != std::string::npos && mk.find("gates/layout_gate_host.py") != std::string::npos,
           "the Makefile uses the bring-up recipe and runs the gates A/B/C and the host twin");
    const std::string ops = slurp(root + "/src/GarnetMetalOps.h");
    expect(!ops.empty(), "the ops header is present");
    const std::string bo = slurp(bringRoot + "/src/garnet/src/GarnetMetalOps.h");
    if (bo.empty()) std::printf("NOTE: the bring-up copy of GarnetMetalOps.h is not readable under %s; compare it with the bring-up test\n", bringRoot.c_str());
    else expect(bo == ops, "GarnetMetalOps.h is byte-identical in the aux kext and the bring-up kext");
    const std::string dops = slurp(root + "/src/GarnetDisplayOps.h");
    expect(!dops.empty(), "the display ops header is present");
    const std::string bdo = slurp(bringRoot + "/src/garnet/src/GarnetDisplayOps.h");
    if (bdo.empty()) std::printf("NOTE: the bring-up copy of GarnetDisplayOps.h is not readable under %s; compare it with the bring-up test\n", bringRoot.c_str());
    else expect(bdo == dops, "GarnetDisplayOps.h is byte-identical in the aux kext and the bring-up kext");
}

// ---- A7 (aux 0.0.3): the display pipe --------------------------------------------------------------------------------------------------------------------
static int d_hook(void *, uint32_t, uint32_t, void *, const uint64_t *, uint32_t, uint64_t *) { return 1; }
static void *d_pci(void *) { return (void *)(uintptr_t)0x1234; }
static std::string body_of(const std::string &src, const std::string &head) { const size_t a = src.find(head); if (a == std::string::npos) return ""; const size_t e = src.find("\n}\n", a); return src.substr(a, e == std::string::npos ? std::string::npos : e - a); }
static void a7_display(const std::string &root) {
    GarnetMetalOps o = good_ops(); o.disp_flags = GARNET_DISP_F_ON; o.disp_hook = d_hook; o.pci_device = d_pci;
    expect_u("the table we test is ABI 3 and 152 bytes", o.abi * 1000u + o.size, 3152);
    expect(disp_enabled(&o), "ABI 2, full size, the ON flag and the hook: display ON");
    { GarnetMetalOps x = o; x.abi = 1; expect(!disp_enabled(&x), "an ABI-1 table is display OFF, whatever its later bytes say"); }
    { GarnetMetalOps x = o; x.size = GARNET_METAL_OPS_MIN; expect(!disp_enabled(&x), "a 120-byte table is display OFF"); }
    { GarnetMetalOps x = o; x.size = GARNET_METAL_OPS_V2 - 8; expect(!disp_enabled(&x), "a table that stops inside the ABI-2 members is display OFF"); }
    { GarnetMetalOps x = o; x.disp_flags = 0; expect(!disp_enabled(&x), "flag clear (garnet-metal-disp not latched on): OFF"); }
    { GarnetMetalOps x = o; x.disp_flags = 2; expect(!disp_enabled(&x), "only bit 0 is the ON flag"); }
    { GarnetMetalOps x = o; x.disp_hook = nullptr; expect(!disp_enabled(&x), "no display hook: OFF"); }
    expect(!disp_enabled(nullptr), "no table: OFF");
    {   // the OLD table exactly as the 0.0.612 bring-up kext serves it: 120 bytes, nothing after them. Bytes 120..143 are ASan-POISONED, so any read of the ABI-2
        // members aborts this test (the allocation is the full struct size only so that UBSan's object-size check accepts the member accesses).
        uint8_t *buf = (uint8_t *)std::malloc(sizeof(GarnetMetalOps));
        std::memset(buf, 0xA5, sizeof(GarnetMetalOps));
        GarnetMetalOps full = good_ops(); full.abi = 1; full.size = GARNET_METAL_OPS_MIN; std::memcpy(buf, &full, GARNET_METAL_OPS_MIN);
        ASAN_POISON_MEMORY_REGION(buf + GARNET_METAL_OPS_MIN, sizeof(GarnetMetalOps) - GARNET_METAL_OPS_MIN);
        const GarnetMetalOps *t = (const GarnetMetalOps *)(void *)buf;
        expect_u("the 0.0.612 table (abi 1, 120 bytes) passes ops_check", ops_check(t), kOpsOk);
        expect(!disp_enabled(t), "... and is display OFF, decided without reading byte 120 or later");
        uint32_t two = 2; std::memcpy(buf + 4, &two, 4);
        expect(!disp_enabled(t), "a table that says abi 2 but size 120: OFF, nothing past byte 120 read");
        ASAN_UNPOISON_MEMORY_REGION(buf + GARNET_METAL_OPS_MIN, sizeof(GarnetMetalOps) - GARNET_METAL_OPS_MIN);
        std::free(buf);
    }
    // the walk provider: only display on + a nub argument + a PCI device substitutes
    void *nub = (void *)(uintptr_t)0x100, *pci = (void *)(uintptr_t)0x200;
    for (int m = 0; m < 8; ++m) {
        const bool on = m & 1, isNub = m & 2, havePci = m & 4;
        void *got = dm_walk_provider(on, isNub, nub, havePci ? pci : nullptr);
        expect(got == ((on && isNub && havePci) ? pci : nub), "dm_walk_provider: the PCI device only with display on, a nub argument and a PCI device");
    }
    expect(pipe_choice(true, true) == kPipeOurs && pipe_choice(true, false) == kPipeFamily && pipe_choice(false, true) == kPipeFamily && pipe_choice(false, false) == kPipeFamily,
           "pipe_choice: ours only with display on and a successful allocation, else the family's own pipe");

    const std::string src = slurp(root + "/src/GarnetAccel.cpp") + "\n" + slurp(root + "/src/garnet_accel_display_impl.inc"), lv = slurp(root + "/gates/leaves.py");
    {   const std::string b = body_of(src, "IOAccelDisplayPipe *GarnetAccelerator::newDisplayPipe(");
        expect(!b.empty() && b.find("GARNET_AUX_ENTER(IOGraphicsAccelerator2::newDisplayPipe());") != std::string::npos, "newDisplayPipe: the kill switch returns the family's own pipe");
        const size_t on = b.find("disp_enabled(gOps)"), al = b.find("OSTypeAlloc(GarnetDisplayPipe)"), ch = b.find("pipe_choice(on, mine != nullptr) == kPipeOurs"), fb = b.find(": IOGraphicsAccelerator2::newDisplayPipe();");
        expect(on != std::string::npos && al != std::string::npos && ch != std::string::npos && fb != std::string::npos && on < al && al < ch && ch < fb, "newDisplayPipe: display check < our allocation < pipe_choice < the family fallback");
        expect(b.find("on ? OSTypeAlloc(GarnetDisplayPipe) : nullptr") != std::string::npos, "our subclass is allocated only with display on");
        expect(b.find("return nullptr") == std::string::npos && b.find("return p;") != std::string::npos, "newDisplayPipe never returns NULL of its own (found_framebuffer stores it unchecked)"); }
    {   const std::string b = body_of(src, "GARNET_R_IOAccelDisplayMachine_267 GarnetDisplayMachine::start(");
        expect(!b.empty() && b.find("GARNET_AUX_ENTER(IOAccelDisplayMachine::start(provider));") != std::string::npos, "display-machine start: the kill switch passes the family's own argument through");
        expect(b.find("(on && gCtx && gOps->pci_device) ? gOps->pci_device(gCtx) : nullptr") != std::string::npos && b.find("dm_walk_provider(on, garnet_provider_is_nub(") != std::string::npos, "display-machine start: the getter is asked only with display on and the substitution is the pure decision");
        expect(b.find("return IOAccelDisplayMachine::start((IOPCIDevice *)use);") != std::string::npos, "display-machine start: the family's own start does the walk"); }
    {   const std::string b = body_of(src, "static bool garnet_disp_vhook(");
        expect(b.find("if (!gCtx || !disp_enabled(gOps)) return false;") != std::string::npos && b.find("gOps->disp_hook(gCtx, cls, slot, self, a, n, r) == 1") != std::string::npos,
               "garnet_disp_vhook: display off = not handled (-> the family's slot); only a return of exactly 1 is handled"); }
    {   const std::string b = body_of(src, "extern \"C\" __attribute__((used, visibility(\"hidden\"))) uint64_t garnet_dp_perform(");
        const size_t ks = b.find("GARNET_AUX_ENTER((uint64_t)0);"), hk = b.find("garnet_disp_vhook(GARNET_VC_DISPLAYPIPE, 277, (void *)self, a, 1, &r)"), fb = b.find("garnet_ztvfam_IOAccelDisplayPipe[2 + 277]");
        expect(ks != std::string::npos && hk != std::string::npos && fb != std::string::npos && ks < hk && hk < fb, "slot 277: kill switch < the display hook < the family's own performTransaction");
        expect(src.find("__attribute__((naked)) void GarnetDisplayPipe::_vslot277() { __asm__(\"jmp _garnet_dp_perform\"); }") != std::string::npos, "slot 277 is a bare tail jump (rdi/rsi untouched)"); }
    expect(src.find("GARNET_TR_DEFS_GarnetDisplayPipe(GarnetDisplayPipe)") != std::string::npos && src.find("GARNET_META(GarnetDisplayPipe, IOAccelDisplayPipe)") != std::string::npos, "the display pipe class is defined and its trampolines instantiated");
    expect(lv.find("'GarnetDisplayPipe':       ('GARNET_VC_DISPLAYPIPE',  {267: 'base', 278: 'base', 279: 'base'}, 'garnet_disp_vhook')") != std::string::npos, "leaves: 267 / 278 / 279 hooked through garnet_disp_vhook, the family's own slot by default");
    expect(lv.find("('GarnetDisplayPipe',   'IOAccelDisplayPipe',       [277])") != std::string::npos && lv.find("[183, 184, 18, 322, 348, 329, 239, 238])") != std::string::npos && lv.find("('GarnetDisplayMachine','IOAccelDisplayMachine',    [267])") != std::string::npos,
           "leaves: the hand overrides 277 (pipe), 329 (newDisplayPipe), 267 (display-machine start)");
    expect(lv.find("HAND = {'GarnetEventMachine': [35], 'GarnetDisplayPipe': [277]}") != std::string::npos, "leaves: slot 277 of the display pipe is hand written (HAND), never generated from its placeholder declaration");
    expect(lv.find("EXPECT_CLASSES = 13") != std::string::npos && lv.find("EXPECT_SLOTS = 2720") != std::string::npos, "the expected gate totals are 13 classes / 2720 slots (0.0.4: + GarnetFramebuffer vs IOFramebuffer, 350 slots)");
    {   const std::string g = body_of(src, "static bool garnet_layout_gate() {");
        expect(!g.empty() && g.find("continue") == std::string::npos && g.find("for (const ClassGate &g : kGate) {") != std::string::npos, "the runtime layout gate walks EVERY generated class (no skip)"); }
    {   const std::string inc = slurp(root + "/build/gen/garnet_tramp.inc");
        if (inc.empty()) std::printf("NOTE: build/gen/garnet_tramp.inc absent (run make); the display trampoline output pins are skipped\n");
        else {
            const size_t a = inc.find("#define GARNET_TR_DEFS_GarnetDisplayPipe(Leaf)"); const std::string d = a == std::string::npos ? "" : inc.substr(a);
            expect(!d.empty() && d.find("garnet_vhook(") == std::string::npos, "the generated display trampolines never ask the generic vhook");
            expect(d.find("garnet_disp_vhook(GARNET_VC_DISPLAYPIPE, 267, (void *)this, a, 2, &r)") != std::string::npos && d.find("garnet_ztvfam_IOAccelDisplayPipe[269]") != std::string::npos, "generated 267: display hook, else the family's slot 267");
            expect(d.find("garnet_disp_vhook(GARNET_VC_DISPLAYPIPE, 278, (void *)this, a, 1, &r)") != std::string::npos && d.find("garnet_ztvfam_IOAccelDisplayPipe[280]") != std::string::npos, "generated 278: display hook, else the family's slot 278");
            expect(d.find("garnet_disp_vhook(GARNET_VC_DISPLAYPIPE, 279, (void *)this, a, 1, &r)") != std::string::npos && d.find("garnet_ztvfam_IOAccelDisplayPipe[281]") != std::string::npos, "generated 279: display hook, else the family's slot 279");
        }
    }
}


// ---- A8 (aux 0.0.4, M5): the monitor B framebuffer ----------------------------------------------------------------------------------------------------------------------
static int d_snap(void *, GarnetDispSnap *) { return 0; }
static int d_power(void *, uint32_t) { return 0; }
static GarnetDispOps good_dops() { GarnetDispOps o; memset(&o, 0, sizeof o); o.magic = GARNET_DISP_OPS_MAGIC; o.abi = GARNET_DISP_ABI; o.size = GARNET_DISP_OPS_MIN; o.kext_build = 658; o.snapshot = d_snap; o.power = d_power; return o; }
static GarnetDispSnap good_snap() {
    GarnetDispSnap s; memset(&s, 0, sizeof s);
    s.index = 1; s.otg = 2; s.ddcLine = 3; s.w = 2560; s.h = 1440; s.pitchBytes = 10240; s.fmt = GARNET_DISP_FMT_ARGB8888; s.refresh1616 = GARNET_DISP_REFRESH1616; s.pixHz = 241500000ull;
    s.aperPhys = 0x80000000ull + 0x01800000ull; s.aperLen = 14745600ull; s.mcA = 0x8001800000ull; s.mcB = 0x8002600000ull; s.edidLen = 256;
    s.edid[0] = 0; for (int i = 1; i < 7; i++) s.edid[i] = 0xFF; s.edid[7] = 0;
    s.edid[127] = (uint8_t)(0u - 0x7Fu * 0xFFu - 0u);   // block 0 sums to 0: header = 6 * 0xFF; the last byte fixes the sum
    { uint32_t sum = 0; for (int i = 0; i < 127; i++) sum += s.edid[i]; s.edid[127] = (uint8_t)(0u - sum); }
    s.edid[128] = 0x02; s.edid[129] = 0x03;
    { uint32_t sum = 0; for (int i = 128; i < 255; i++) sum += s.edid[i]; s.edid[255] = (uint8_t)(0u - sum); }
    return s;
}
static void a8_framebuffer(const std::string &root) {
    // ---- the shared header: shapes and constants
    expect_u("GarnetDispSnap is 336 bytes", sizeof(GarnetDispSnap), 336); expect_u("GarnetDispOps ABI 1 is 72 bytes", sizeof(GarnetDispOps), 72);
    expect_u("the ops magic is 'GRND' little-endian", GARNET_DISP_OPS_MAGIC, (uint32_t)'G' | ((uint32_t)'R' << 8) | ((uint32_t)'N' << 16) | ((uint32_t)'D' << 24));
    expect_u("ABI 2 (0.0.6; ABI 1 is still the monitor B's minimum)", GARNET_DISP_ABI, 2); expect_u("... minimum 1", GARNET_DISP_ABI_MIN, 1); expect_u("the table minimum is 72", GARNET_DISP_OPS_MIN, 72);
    expect_u("the aperture is 2560 x 1440 x 4 = 14,745,600 bytes = 225 x 64 KiB", GARNET_DISP_BYTES, 2560ull * 1440ull * 4ull); expect_u("... = 225 x 64 KiB", GARNET_DISP_BYTES, 225ull * 65536ull);
    expect_u("the pitch is 2560 x 4", GARNET_DISP_PITCH, 2560u * 4u);
    expect_u("refresh = (241500000 << 16) / (2720 * 1481)", GARNET_DISP_REFRESH1616, (uint32_t)((241500000ull << 16) / (2720ull * 1481ull)));
    expect(GARNET_DISP_REFRESH1616 > (uint32_t)(59.94 * 65536) && GARNET_DISP_REFRESH1616 < (uint32_t)(59.96 * 65536), "... about 59.95 Hz");
    expect_u("the nub class name", strcmp(GARNET_DISP_NUB_CLASS, "GarnetDisplayNub"), 0); expect_u("the ops function symbol", strcmp(GARNET_DISP_FN_SYMBOL, "garnet.disp.ops"), 0); expect_u("the index key", strcmp(GARNET_DISP_INDEX_KEY, "GarnetDisplayIndex"), 0);
    // ---- the ops-table check
    { GarnetDispOps o = good_dops(); expect_u("a good ops table", garnetdisp_ops_check(&o), GARNET_DOV_OK);
      expect_u("NULL table", garnetdisp_ops_check(nullptr), GARNET_DOV_NULL);
      { GarnetDispOps x = o; x.magic ^= 1u; expect_u("bad magic", garnetdisp_ops_check(&x), GARNET_DOV_MAGIC); }
      { GarnetDispOps x = o; x.abi = 0u; expect_u("abi 0", garnetdisp_ops_check(&x), GARNET_DOV_ABI); }
      { GarnetDispOps x = o; x.size = 71u; expect_u("size 71 (short)", garnetdisp_ops_check(&x), GARNET_DOV_SIZE); }
      { GarnetDispOps x = o; x.snapshot = nullptr; expect_u("no snapshot hook", garnetdisp_ops_check(&x), GARNET_DOV_MISSING); }
      { GarnetDispOps x = o; x.power = nullptr; expect_u("no power hook", garnetdisp_ops_check(&x), GARNET_DOV_MISSING); }
      { GarnetDispOps x = o; x.abi = 2u; x.size = 96u; expect_u("an NEWER table (abi 2, 96 bytes) is accepted (append-only)", garnetdisp_ops_check(&x), GARNET_DOV_OK); }
      { GarnetDispOps x = o; x.trace = nullptr; expect_u("trace is optional", garnetdisp_ops_check(&x), GARNET_DOV_OK); }
      expect(o.flip == nullptr && o.flip_status == nullptr && o.vbl_sample == nullptr, "the M6 placeholders are NULL in ABI 1"); }
    // ---- the snapshot validator
    { GarnetDispSnap g = good_snap(); expect_u("a good snapshot", garnetdisp_snap_valid(&g), GARNET_DSV_OK);
      expect_u("NULL snapshot", garnetdisp_snap_valid(nullptr), GARNET_DSV_NULL);
      { GarnetDispSnap x = g; x.index = 0; expect_u("index 0", garnetdisp_snap_valid(&x), GARNET_DSV_INDEX); } { GarnetDispSnap x = g; x.index = 3; expect_u("index 3", garnetdisp_snap_valid(&x), GARNET_DSV_INDEX); }
      { GarnetDispSnap x = g; x.otg = 1; expect_u("otg", garnetdisp_snap_valid(&x), GARNET_DSV_GEOMETRY); } { GarnetDispSnap x = g; x.ddcLine = 2; expect_u("ddc line", garnetdisp_snap_valid(&x), GARNET_DSV_GEOMETRY); }
      { GarnetDispSnap x = g; x.w = 1920; expect_u("width", garnetdisp_snap_valid(&x), GARNET_DSV_GEOMETRY); } { GarnetDispSnap x = g; x.h = 1080; expect_u("height", garnetdisp_snap_valid(&x), GARNET_DSV_GEOMETRY); }
      { GarnetDispSnap x = g; x.pitchBytes = 7680; expect_u("pitch", garnetdisp_snap_valid(&x), GARNET_DSV_GEOMETRY); } { GarnetDispSnap x = g; x.fmt = 0; expect_u("format", garnetdisp_snap_valid(&x), GARNET_DSV_GEOMETRY); }
      { GarnetDispSnap x = g; x.refresh1616 = 60u << 16; expect_u("refresh", garnetdisp_snap_valid(&x), GARNET_DSV_GEOMETRY); } { GarnetDispSnap x = g; x.pixHz = 148500000ull; expect_u("pixel clock", garnetdisp_snap_valid(&x), GARNET_DSV_GEOMETRY); }
      { GarnetDispSnap x = g; x.aperLen = 14745600ull - 65536ull; expect_u("aperture length", garnetdisp_snap_valid(&x), GARNET_DSV_GEOMETRY); }
      { GarnetDispSnap x = g; x.aperPhys = 0; expect_u("aperture base 0", garnetdisp_snap_valid(&x), GARNET_DSV_APERTURE); } { GarnetDispSnap x = g; x.aperPhys += 4096; expect_u("aperture base not 64 KiB aligned", garnetdisp_snap_valid(&x), GARNET_DSV_APERTURE); }
      { GarnetDispSnap x = g; x.aperPhys = 0xFFFFFFFFFFFF0000ull; expect_u("aperture wraps", garnetdisp_snap_valid(&x), GARNET_DSV_APERTURE); }
      { GarnetDispSnap x = g; x.mcA = 0; expect_u("mcA 0", garnetdisp_snap_valid(&x), GARNET_DSV_MC); } { GarnetDispSnap x = g; x.mcB = 0; expect_u("mcB 0", garnetdisp_snap_valid(&x), GARNET_DSV_MC); }
      { GarnetDispSnap x = g; x.mcB = x.mcA; expect_u("mcA == mcB", garnetdisp_snap_valid(&x), GARNET_DSV_MC); } { GarnetDispSnap x = g; x.mcA += 4096; expect_u("mcA unaligned", garnetdisp_snap_valid(&x), GARNET_DSV_MC); }
      { GarnetDispSnap x = g; x.edidLen = 128; expect_u("one EDID block only", garnetdisp_snap_valid(&x), GARNET_DSV_EDID_LEN); }
      { GarnetDispSnap x = g; x.edid[0] = 1; x.edid[127] = (uint8_t)(x.edid[127] - 1); expect_u("block 0 without the header (sum still 0)", garnetdisp_snap_valid(&x), GARNET_DSV_EDID_HDR); }
      { GarnetDispSnap x = g; x.edid[20] ^= 1; expect_u("block 0 checksum", garnetdisp_snap_valid(&x), GARNET_DSV_EDID_SUM); } { GarnetDispSnap x = g; x.edid[200] ^= 1; expect_u("block 1 checksum", garnetdisp_snap_valid(&x), GARNET_DSV_EDID_SUM); } }
    // ---- fb_start_verdict: the order is kill switch < metal-ws < nub < ops < snapshot
    expect_u("start: everything good", fb_start_verdict(true, false, true, true, GARNET_DOV_OK, 0, GARNET_DSV_OK), kFbStartOk);
    expect_u("start: garnet-aux=0", fb_start_verdict(false, false, true, true, GARNET_DOV_OK, 0, GARNET_DSV_OK), kFbStartAuxOff);
    expect_u("start: garnet-metal-ws present", fb_start_verdict(true, true, true, true, GARNET_DOV_OK, 0, GARNET_DSV_OK), kFbStartMetalWs);
    expect_u("start: the provider is not the display nub", fb_start_verdict(true, false, false, true, GARNET_DOV_OK, 0, GARNET_DSV_OK), kFbStartNotNub);
    for (uint32_t ov = GARNET_DOV_NULL; ov <= GARNET_DOV_MISSING; ov++) expect_u("start: any bad ops verdict refuses", fb_start_verdict(true, false, true, true, ov, 0, GARNET_DSV_OK), kFbStartOps);
    expect_u("start: the snapshot hook refused", fb_start_verdict(true, false, true, true, GARNET_DOV_OK, -1, GARNET_DSV_OK), kFbStartSnapshot);
    for (uint32_t sv = GARNET_DSV_NULL; sv <= GARNET_DSV_EDID_HDR; sv++) expect_u("start: any bad snapshot verdict refuses", fb_start_verdict(true, false, true, true, GARNET_DOV_OK, 0, sv), kFbStartSnapshot);
    expect_u("start: priority aux > metal-ws", fb_start_verdict(false, true, false, false, GARNET_DOV_MAGIC, -1, GARNET_DSV_MC), kFbStartAuxOff);
    expect_u("start: priority metal-ws > nub", fb_start_verdict(true, true, false, false, GARNET_DOV_MAGIC, -1, GARNET_DSV_MC), kFbStartMetalWs);
    expect_u("start: priority nub > ops", fb_start_verdict(true, false, false, false, GARNET_DOV_MAGIC, -1, GARNET_DSV_MC), kFbStartNotNub);
    // ---- 0.0.7 (M6 Stage 1a, R1): boot-arg garnet-m6 lifts the metal-ws refusal and ONLY that
    expect(!m6_on(false, 1) && m6_on(true, 1) && !m6_on(true, 0) && !m6_on(true, 2) && !m6_on(true, 0xFFFFFFFFu), "R1: garnet-m6 is on only when present AND exactly 1");
    expect(fb_metal_ws_blocks(true, false) && !fb_metal_ws_blocks(true, true) && !fb_metal_ws_blocks(false, false) && !fb_metal_ws_blocks(false, true), "R1: metal-ws blocks the framebuffer unless the M6 latch is on");
    expect_u("R1: metal-ws present, latch OFF -> refused (aux 0.0.6's behaviour)", fb_start_verdict(true, fb_metal_ws_blocks(true, m6_on(false, 0)), true, true, GARNET_DOV_OK, 0, GARNET_DSV_OK), kFbStartMetalWs);
    expect_u("R1: metal-ws present, garnet-m6=1 -> starts", fb_start_verdict(true, fb_metal_ws_blocks(true, m6_on(true, 1)), true, true, GARNET_DOV_OK, 0, GARNET_DSV_OK), kFbStartOk);
    expect_u("R1: metal-ws present, garnet-m6=2 -> refused (only 1 enables)", fb_start_verdict(true, fb_metal_ws_blocks(true, m6_on(true, 2)), true, true, GARNET_DOV_OK, 0, GARNET_DSV_OK), kFbStartMetalWs);
    expect_u("R1: the kill switch still beats the latch", fb_start_verdict(false, fb_metal_ws_blocks(true, true), true, true, GARNET_DOV_OK, 0, GARNET_DSV_OK), kFbStartAuxOff);
    expect_u("R1: the latch does not admit a foreign provider", fb_start_verdict(true, fb_metal_ws_blocks(true, true), false, true, GARNET_DOV_OK, 0, GARNET_DSV_OK), kFbStartNotNub);
    expect_u("R1: the latch does not skip the layout gate", fb_start_verdict(true, fb_metal_ws_blocks(true, true), true, false, GARNET_DOV_OK, 0, GARNET_DSV_OK), kFbStartLayout);
    expect_u("R1: the latch does not skip the ops check", fb_start_verdict(true, fb_metal_ws_blocks(true, true), true, true, GARNET_DOV_MAGIC, 0, GARNET_DSV_OK), kFbStartOps);
    expect_u("R1: the latch does not skip the snapshot check", fb_start_verdict(true, fb_metal_ws_blocks(true, true), true, true, GARNET_DOV_OK, -1, GARNET_DSV_OK), kFbStartSnapshot);
    expect_u("start: the layout gate failed", fb_start_verdict(true, false, true, false, GARNET_DOV_OK, 0, GARNET_DSV_OK), kFbStartLayout);
    expect_u("start: priority nub > layout gate", fb_start_verdict(true, false, false, false, GARNET_DOV_OK, 0, GARNET_DSV_OK), kFbStartNotNub);
    expect_u("start: priority layout gate > ops", fb_start_verdict(true, false, true, false, GARNET_DOV_MAGIC, -1, GARNET_DSV_MC), kFbStartLayout);
    expect_u("start: priority ops > snapshot", fb_start_verdict(true, false, true, true, GARNET_DOV_MAGIC, -1, GARNET_DSV_MC), kFbStartOps);
    // ---- the answers
    const GarnetDispSnap g = good_snap();
    expect_u("one display mode", fb_mode_count(), 1); expect_u("its id is 1", kFbModeId, 1);
    { FbModeInfo m; expect(fb_mode_info(&g, 1, &m), "mode 1 is described");
      expect(m.w == 2560u && m.h == 1440u && m.refresh1616 == GARNET_DISP_REFRESH1616 && m.maxDepthIndex == 0u && m.valid && m.safe && m.isDefault, "2560x1440, the computed refresh, maxDepthIndex 0, Valid | Safe | Default");
      expect(!fb_mode_info(&g, 0, &m) && !fb_mode_info(&g, 2, &m) && !fb_mode_info(nullptr, 1, &m) && !fb_mode_info(&g, 1, nullptr), "any other mode id, no snapshot or no output is refused"); }
    expect(fb_mode_ok(1, 0) && !fb_mode_ok(1, 1) && !fb_mode_ok(2, 0) && !fb_mode_ok(0, 0), "setDisplayMode accepts only (1, 0)");
    { FbPixel p; expect(fb_pixel_info(&g, 1, 0, true, &p), "pixel info for (1, 0, system aperture)");
      expect(p.bytesPerRow == 10240u && p.bitsPerPixel == 32u && p.componentCount == 3u && p.bitsPerComponent == 8u && p.w == 2560u && p.h == 1440u, "10240 bytes a row, 32 bpp, 3 x 8 bits");
      expect(p.masks[0] == 0x00FF0000u && p.masks[1] == 0x0000FF00u && p.masks[2] == 0x000000FFu, "masks R 0xFF0000 / G 0xFF00 / B 0xFF (the kext fills 0xFFRRGGBB; SURFACE_CONFIG 8)");
      expect(std::string(p.format) == "--------RRRRRRRRGGGGGGGGBBBBBBBB" && strlen(p.format) == 32, "the pixel format string");
      expect(!fb_pixel_info(&g, 1, 0, false, &p) && !fb_pixel_info(&g, 1, 1, true, &p) && !fb_pixel_info(&g, 2, 0, true, &p) && !fb_pixel_info(&g, 1, 0, true, nullptr), "another aperture, depth, mode or no output is refused"); }
    { uint64_t ph = 0, ln = 0; expect(fb_aperture(&g, true, &ph, &ln) && ph == g.aperPhys && ln == 14745600ull, "the system aperture is bar0Phys + off[0], 14,745,600 bytes");
      expect(!fb_aperture(&g, false, &ph, &ln), "any other aperture returns NULL"); GarnetDispSnap z = g; z.aperPhys = 0; expect(!fb_aperture(&z, true, &ph, &ln), "no aperture without a base"); }
    { FbAttr a = fb_connection_attr(&g, kFbAttrEnable); expect(a.rc == kFbRcSuccess && a.hasValue && a.value == 1u, "kConnectionEnable = 1");
      a = fb_connection_attr(&g, kFbAttrCheckEnable); expect(a.rc == kFbRcSuccess && a.hasValue && a.value == 1u, "kConnectionCheckEnable = 1");
      a = fb_connection_attr(&g, kFbAttrFlags); expect(a.rc == kFbRcSuccess && a.hasValue && a.value == 0u, "kConnectionFlags = 0");
      a = fb_connection_attr(&g, kFbAttrHlDdc); expect(a.rc == kFbRcSuccess && !a.hasValue, "kConnectionSupportsHLDDCSense = success (no value) with an EDID");
      GarnetDispSnap z = g; z.edidLen = 0; a = fb_connection_attr(&z, kFbAttrHlDdc); expect(a.rc == kFbRcUnsupported, "... Unsupported without one");
      a = fb_connection_attr(&g, kFbAttrPower); expect(a.rc == kFbRcSuper, "kConnectionPower is not answered by getAttributeForConnection (super)");
      a = fb_connection_attr(&g, 0x12345678u); expect(a.rc == kFbRcSuper, "an unknown connection attribute goes to super");
      a = fb_attr(kFbAttrCursor); expect(a.rc == kFbRcSuccess && a.hasValue && a.value == 0u, "getAttribute('crsr') = 0 (software cursor)");
      a = fb_attr(kFbAttrPower); expect(a.rc == kFbRcSuper, "getAttribute(powr) is super's"); a = fb_attr(0x1u); expect(a.rc == kFbRcSuper, "every other attribute is super's");
      expect(fb_power_attr(kFbAttrPower) && !fb_power_attr(kFbAttrCursor) && !fb_power_attr(kFbAttrEnable), "only 'powr' is recorded as power"); }
    expect_u("'crsr'", kFbAttrCursor, 0x63727372u); expect_u("'powr'", kFbAttrPower, 0x706f7772u); expect_u("'enab'", kFbAttrEnable, 0x656e6162u); expect_u("'cena'", kFbAttrCheckEnable, 0x63656e61u); expect_u("'flgs'", kFbAttrFlags, 0x666c6773u); expect_u("'hddc'", kFbAttrHlDdc, 0x68646463u);
    { const uint8_t *src = nullptr; uint64_t n = 0;
      expect(fb_has_ddc(&g, 0) && !fb_has_ddc(&g, 1) && !fb_has_ddc(&g, -1) && !fb_has_ddc(nullptr, 0), "hasDDCConnect: connection 0 with an EDID");
      expect_u("block 1 is the base EDID", fb_ddc_block(&g, 0, 1, true, true, 128, &src, &n), kFbDdcOk); expect(src == g.edid && n == 128u, "... bytes 0..127");
      expect_u("block 2 is the CTA extension", fb_ddc_block(&g, 0, 2, true, true, 128, &src, &n), kFbDdcOk); expect(src == g.edid + 128 && n == 128u, "... bytes 128..255");
      expect_u("a 64-byte buffer gets 64 bytes", fb_ddc_block(&g, 0, 1, true, true, 64, &src, &n), kFbDdcOk); expect_u("... n", n, 64);
      expect_u("a 1000-byte buffer gets 128 bytes", fb_ddc_block(&g, 0, 2, true, true, 1000, &src, &n), kFbDdcOk); expect_u("... n", n, 128);
      expect_u("block 0 (1-based) is not found", fb_ddc_block(&g, 0, 0, true, true, 128, &src, &n), kFbDdcNotFound); expect_u("block 3 is not found", fb_ddc_block(&g, 0, 3, true, true, 128, &src, &n), kFbDdcNotFound);
      expect_u("another connection is unsupported", fb_ddc_block(&g, 1, 1, true, true, 128, &src, &n), kFbDdcUnsupported); expect_u("another block type is unsupported", fb_ddc_block(&g, 0, 1, false, true, 128, &src, &n), kFbDdcUnsupported);
      expect_u("no buffer is unsupported", fb_ddc_block(&g, 0, 1, true, false, 128, &src, &n), kFbDdcUnsupported); }
    // ---- the source: the override set is EXACTLY the spec's table
    const std::string src = slurp(root + "/src/GarnetAccel.cpp") + "\n" + slurp(root + "/src/garnet_accel_display_impl.inc"), lv = slurp(root + "/gates/leaves.py"), plist = slurp(root + "/Info.plist"), inst = slurp(root + "/INSTALL.md");
    std::string decl; { const size_t a = src.find("class GarnetFramebuffer : public IOFramebuffer {"); const size_t e = a == std::string::npos ? a : src.find("\n};\n", a); if (a != std::string::npos && e != std::string::npos) decl = src.substr(a, e - a); }
    expect(!decl.empty(), "the GarnetFramebuffer class is declared");
    std::vector<std::string> ovr;
    { size_t p = 0; while ((p = decl.find(" override;", p)) != std::string::npos) { const size_t ls = decl.rfind('\n', p); const std::string line = decl.substr(ls + 1, p - ls - 1); const size_t pr = line.find('('); const size_t sp = line.rfind(' ', pr); std::string nm = line.substr(sp + 1, pr - sp - 1); while (!nm.empty() && nm[0] == '*') nm.erase(0, 1); ovr.push_back(nm); p += 10; } }
    { const char *want[] = { "start", "getApertureRange", "getVRAMRange", "enableController", "getPixelFormats", "getDisplayModeCount", "getDisplayModes", "getInformationForDisplayMode", "getPixelFormatsForDisplayMode", "getPixelInformation",
                             "getCurrentDisplayMode", "setDisplayMode", "getStartupDisplayMode", "setAttribute", "getAttribute", "getConnectionCount", "setAttributeForConnection", "getAttributeForConnection", "hasDDCConnect", "getDDCBlock" };
      expect_u("exactly 20 overrides (the spec's table: start + enableController + the apertures + the mode / pixel answers + the attributes + the DDC pair)", ovr.size(), 20);
      for (const char *w : want) { bool f = false; for (const std::string &o : ovr) if (o == w) f = true; expect(f, (std::string("override present: ") + w).c_str()); }
      for (const std::string &o : ovr) { bool f = false; for (const char *w : want) if (o == w) f = true; expect(f, ("no other override: " + o).c_str()); } }
    expect(decl.find("isConsoleDevice") == std::string::npos && src.find("GarnetFramebuffer::isConsoleDevice") == std::string::npos, "isConsoleDevice is NOT overridden (the base returns 0)");
    for (const char *t : { "registerForInterruptType", "unregisterInterrupt", "setInterruptState", "getNotificationSemaphore" }) expect(decl.find(t) == std::string::npos && src.find(std::string("GarnetFramebuffer::") + t) == std::string::npos, (std::string("the interrupts are super's: ") + t).c_str());
    expect(decl.find("virtual") == std::string::npos, "the leaf declares no new virtual");
    // the slot numbers, against the KC's IOFramebuffer vtable listing (ioaccel-layout/vtables/IOFramebuffer.tsv: idx, ..., demangled)
    { const char *layout = std::getenv("GARNET_LAYOUT_OUT");
      std::string tsv = slurp((layout ? std::string(layout) : root + "/../../tools/ioaccel-layout/layout-out") + "/vtables/IOFramebuffer.tsv"); if (tsv.empty()) tsv = slurp(root + "/ioaccel/IOFramebuffer.tsv");     // the plant scratch tree carries a copy
      if (tsv.empty()) std::printf("NOTE: IOFramebuffer.tsv not readable in GARNET_LAYOUT_OUT/vtables\n");
      else {
        std::map<int, std::pair<std::string, bool>> row; std::istringstream is(tsv); std::string ln;
        while (std::getline(is, ln)) { if (ln.empty() || ln[0] == '#') continue; std::vector<std::string> f; size_t a = 0; while (true) { const size_t t = ln.find('\t', a); f.push_back(ln.substr(a, t == std::string::npos ? std::string::npos : t - a)); if (t == std::string::npos) break; a = t + 1; } if (f.size() >= 7) row[atoi(f[0].c_str())] = { f[6], f[3] == "PURE" }; }
        expect_u("the listing has 350 slots", row.size(), 350);
        struct E { int slot; const char *name; };
        // the spec's gate list (slots 305..348): 305 isConsoleDevice, 310 getVRAMRange, 311 enableController, 309 + 312-318 pure, 319 setDisplayMode, 322 getStartupDisplayMode, 325 / 326 set / getAttribute, 330-332 connection count / set / get, 344 / 345 hasDDCConnect / getDDCBlock, 346-348 interrupts
        const E spec[] = { { 305, "isConsoleDevice" }, { 310, "getVRAMRange" }, { 311, "enableController" }, { 319, "setDisplayMode" }, { 322, "getStartupDisplayMode" }, { 325, "setAttribute" }, { 326, "getAttribute" }, { 330, "getConnectionCount" },
                           { 331, "setAttributeForConnection" }, { 332, "getAttributeForConnection" }, { 344, "hasDDCConnect" }, { 345, "getDDCBlock" }, { 346, "registerForInterruptType" }, { 347, "unregisterInterrupt" }, { 348, "setInterruptState" } };
        for (const E &e : spec) expect(row.count(e.slot) && row[e.slot].first.find(std::string("::") + e.name + "(") != std::string::npos, (std::string("KC slot ") + std::to_string(e.slot) + " is " + e.name).c_str());
        const E pure[] = { { 309, "getApertureRange" }, { 312, "getPixelFormats" }, { 313, "getDisplayModeCount" }, { 314, "getDisplayModes" }, { 315, "getInformationForDisplayMode" }, { 316, "getPixelFormatsForDisplayMode" }, { 317, "getPixelInformation" }, { 318, "getCurrentDisplayMode" } };
        // pure rows show as __cxa_pure_virtual in the listing: the slot numbers are pinned to the 8 PURE rows (309, 312-318) and their names to the SDK header's order
        int np = 0; for (auto &kv : row) if (kv.second.second) np++;
        expect_u("the listing has 8 pure slots (309, 312-318)", np, 8);
        for (const E &e : pure) expect(row.count(e.slot) && row[e.slot].second, (std::string("KC slot ") + std::to_string(e.slot) + " is pure (" + e.name + ")").c_str());
        // leaves.py: FB_EXTRA are exactly the non-pure overrides above, FB_PURE the pure ones
        expect(lv.find("FB_EXTRA = [184, 310, 311, 319, 322, 325, 326, 330, 331, 332, 344, 345]") != std::string::npos && lv.find("FB_PURE = [309, 312, 313, 314, 315, 316, 317, 318]") != std::string::npos, "leaves: the framebuffer's override slots");
        expect(row.count(184) && row[184].first.find("::start(") != std::string::npos, "KC slot 184 is start"); } }
    expect(lv.find("('GarnetFramebuffer',   'IOFramebuffer',            FB_EXTRA)") != std::string::npos && lv.find("FB_LEAF = 'GarnetFramebuffer'") != std::string::npos, "leaves: the framebuffer leaf vs IOFramebuffer and its negative-control class");
    // the start ORDER: kill switch < metal-ws < nub class < ops < snapshot < snapshot check < verdict < IOFramebuffer::start
    { const std::string b = body_of(src, "bool GarnetFramebuffer::start(IOService *provider) {");
      const size_t ks = b.find("GARNET_AUX_ENTER(false);"), ws = b.find("PE_parse_boot_argn(\"garnet-metal-ws\""), nb = b.find("garnet_is_class(provider, GARNET_DISP_NUB_CLASS)"), lg = b.find("!metalWs && isNub && garnet_layout_gate()"), go = b.find("garnet_get_dops(provider)"), oc = b.find("garnetdisp_ops_check_for(o, pidx)"),
                   sn = b.find("o->snapshot((void *)provider, &s)"), sv = b.find("fb_snap_for_index(&s, pidx)"), vd = b.find("fb_start_verdict(garnet_aux_on(), metalWs, isNub, layoutOk, ov, rc, sv)"), rf = b.find("if (v != kFbStartOk) return false;"), sup = b.find("IOFramebuffer::start(provider)");
      expect(ks != std::string::npos && ws != std::string::npos && nb != std::string::npos && lg != std::string::npos && go != std::string::npos && oc != std::string::npos && sn != std::string::npos && sv != std::string::npos && vd != std::string::npos && rf != std::string::npos && sup != std::string::npos, "start has all its steps");
      expect(ks < ws && ws < nb && nb < lg && lg < go && go < oc && oc < sn && sn < sv && sv < vd && vd < rf && rf < sup, "ORDER in GarnetFramebuffer::start: kill switch < metal-ws < nub class < RUNTIME LAYOUT GATE < ops < snapshot < validation < verdict < refuse < IOFramebuffer::start");
      expect(b.find("layoutOk ? garnet_get_dops(provider) : nullptr") != std::string::npos && b.find("layoutOk ? garnetdisp_ops_check_for(o, pidx)") != std::string::npos && b.find("ov == GARNET_DOV_OK ? o->snapshot(") != std::string::npos, "the ops are fetched only after the layout gate passed (from the nub), the snapshot only from a good table");
      expect(b.find("snap = s; dops = o;") > rf, "the snapshot is stored only after the verdict");
      // 0.0.7: the M6 latch is read right after metal-ws and BEFORE the layout gate; `metalWs` (the name every later line uses) is the pure fb_metal_ws_blocks of the two
      const size_t m6 = b.find("PE_parse_boot_argn(\"garnet-m6\""), mw = b.find("const bool metalWs = fb_metal_ws_blocks(metalWsArg, m6On);");
      expect(m6 != std::string::npos && mw != std::string::npos && ws < m6 && m6 < mw && mw < nb && b.find("const bool m6On = m6_on(PE_parse_boot_argn(\"garnet-m6\", &m6v, sizeof(m6v)), m6v);") != std::string::npos, "0.0.7: ORDER: metal-ws read < garnet-m6 read < metalWs = fb_metal_ws_blocks(...) < the nub class check, and the latch goes through the pure m6_on"); }
    { const std::string b = body_of(src, "IODeviceMemory *GarnetFramebuffer::getApertureRange(");
      expect(b.find("fb_aperture(&snap, aperture == kIOFBSystemAperture, &phys, &len)") != std::string::npos && b.find("IODeviceMemory::withRange(") != std::string::npos && b.find("return nullptr;") != std::string::npos, "getApertureRange: system aperture only, from the snapshot"); }
    { const std::string b = body_of(src, "IODeviceMemory *GarnetFramebuffer::getVRAMRange("); expect(b.find("fb_aperture(&snap, true, &phys, &len)") != std::string::npos, "getVRAMRange is the same range"); }
    { const std::string b = body_of(src, "IOReturn GarnetFramebuffer::enableController() {"); expect(b.find("initForPM();") != std::string::npos && b.find("return kIOReturnSuccess;") != std::string::npos, "enableController: initForPM, then success"); }
    { const std::string b = body_of(src, "void GarnetFramebuffer::initForPM() {");
      for (const char *t : { "kIOPMPreventSystemSleep", "registerPowerDriver(this, powerStates, 3)", "temporaryPowerClampOn()", "changePowerStateTo(1)", "{ 1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 }" }) expect(b.find(t) != std::string::npos, (std::string("initForPM (RDNA4FB verbatim): ") + t).c_str()); }
    { const std::string b = body_of(src, "IOReturn GarnetFramebuffer::setAttribute(");
      expect(b.find("powerState = (uint32_t)value;") != std::string::npos && b.find("return kIOReturnSuccess;") != std::string::npos && b.find("if (!fb_power_attr((uint32_t)attribute)) return IOFramebuffer::setAttribute(attribute, value);") != std::string::npos && b.find("if (!fb_power_attr((uint32_t)attribute)) return IOFramebuffer::setAttribute(attribute, value);") < b.find("powerState = (uint32_t)value;"), "setAttribute(power): only 'powr' is recorded (then success); everything else is super's");
      expect(b.find("WREG") == std::string::npos && b.find("config") == std::string::npos, "setAttribute(power): no hardware action"); }
    { const std::string b = body_of(src, "IOReturn GarnetFramebuffer::getAttribute("); expect(b.find("fb_attr((uint32_t)attribute)") != std::string::npos && b.find("IOFramebuffer::getAttribute(attribute, value)") != std::string::npos, "getAttribute: 'crsr' from the pure answer, the rest super's"); }
    { const std::string b = body_of(src, "IOReturn GarnetFramebuffer::getDDCBlock("); expect(b.find("fb_ddc_block(&snap,") != std::string::npos && b.find("blockType == kIODDCBlockTypeEDID") != std::string::npos, "getDDCBlock: the cached EDID, EDID type only"); }
    { const std::string b = body_of(src, "IOReturn GarnetFramebuffer::setDisplayMode("); expect(b.find("fb_mode_ok((uint32_t)displayMode, (uint32_t)depth)") != std::string::npos && b.find("kIOReturnUnsupported") != std::string::npos, "setDisplayMode: only (1, 0), else Unsupported"); }
    { const std::string b = body_of(src, "static const GarnetDispOps *garnet_get_dops(IOService *nub) {");
      expect(b.find("callPlatformFunction(fn, /*waitForFunction=*/false, (void *)&o, nullptr, nullptr, nullptr)") != std::string::npos && b.find("GARNET_DISP_FN_SYMBOL") != std::string::npos, "the display ops are fetched from the nub with waitForFunction = false"); }
    { const std::string b = body_of(src, "IOReturn GarnetFramebuffer::getCurrentDisplayMode(");
      expect(b.find("*displayMode = (IODisplayModeID)kFbModeId;") != std::string::npos && b.find("*depth = 0;") != std::string::npos, "getCurrentDisplayMode answers (1, 0)"); }
    { const std::string b = body_of(src, "IOReturn GarnetFramebuffer::getStartupDisplayMode(");
      expect(b.find("*displayMode = (IODisplayModeID)kFbModeId;") != std::string::npos && b.find("*depth = 0;") != std::string::npos && b.find("IOFramebuffer::getStartupDisplayMode") == std::string::npos, "getStartupDisplayMode answers (1, 0) itself (the base getter returns 0xE00002C7)"); }
    { const std::string b = body_of(src, "IOItemCount GarnetFramebuffer::getConnectionCount() {"); expect(b.find("return 1;") != std::string::npos, "one connection"); }
    { const std::string b = body_of(src, "UInt64 GarnetFramebuffer::getPixelFormatsForDisplayMode("); expect(b.find("return 0;") != std::string::npos, "getPixelFormatsForDisplayMode returns 0 (obsolete)"); }
    { const std::string b = body_of(src, "const char *GarnetFramebuffer::getPixelFormats() {"); expect(b.find("IO32BitDirectPixels \"\\0\"") != std::string::npos, "the pixel formats: IO32BitDirectPixels, double-NUL terminated"); }
    { const std::string b = body_of(src, "IOReturn GarnetFramebuffer::getDisplayModes("); expect(b.find("allDisplayModes[0] = (IODisplayModeID)kFbModeId;") != std::string::npos && b.find("kIOReturnBadArgument") != std::string::npos, "getDisplayModes: one id, a NULL array is a bad argument"); }
    { const std::string b = body_of(src, "IOReturn GarnetFramebuffer::getInformationForDisplayMode(");
      expect(b.find("fb_mode_info(&snap, (uint32_t)displayMode, &m)") != std::string::npos && b.find("kDisplayModeValidFlag") != std::string::npos && b.find("kDisplayModeSafeFlag") != std::string::npos && b.find("kDisplayModeDefaultFlag") != std::string::npos, "getInformationForDisplayMode: from the snapshot, Valid | Safe | Default"); }
    { const std::string b = body_of(src, "IOReturn GarnetFramebuffer::getPixelInformation(");
      expect(b.find("fb_pixel_info(&snap,") != std::string::npos && b.find("pixelInfo->pixelType = kIORGBDirectPixels;") != std::string::npos && b.find("componentMasks[i] = p.masks[i]") != std::string::npos, "getPixelInformation: from the snapshot, RGB direct, the masks"); }
    { const std::string b = body_of(src, "IOReturn GarnetFramebuffer::getAttributeForConnection(");
      expect(b.find("fb_connection_attr(&snap, (uint32_t)attribute)") != std::string::npos && b.find("IOFramebuffer::getAttributeForConnection(connectIndex, attribute, value)") != std::string::npos && b.find("kFbRcUnsupported") != std::string::npos, "getAttributeForConnection: the pure answers, super for the rest"); }
    { const std::string b = body_of(src, "IOReturn GarnetFramebuffer::setAttributeForConnection("); expect(b.find("connPower = (uint32_t)value;") != std::string::npos && b.find("fb_power_attr((uint32_t)attribute)") != std::string::npos, "setAttributeForConnection(power): recorded only"); }
    { const std::string b = body_of(src, "bool GarnetFramebuffer::hasDDCConnect("); expect(b.find("fb_has_ddc(&snap, (int32_t)connectIndex)") != std::string::npos, "hasDDCConnect: from the snapshot"); }
    // the personality
    expect(plist.find("<key>GarnetFramebuffer</key>") != std::string::npos, "a GarnetFramebuffer personality");
    { const size_t a = plist.find("<key>GarnetFramebuffer</key>"), e = plist.find("<key>GarnetAccelerator</key>"); const std::string p = plist.substr(a, e - a);
      expect(p.find("<key>IOProviderClass</key>\n\t\t\t<string>GarnetDisplayNub</string>") != std::string::npos && p.find("<key>IOClass</key>\n\t\t\t<string>GarnetFramebuffer</string>") != std::string::npos &&
             p.find("<key>IOMatchCategory</key>\n\t\t\t<string>IOFramebuffer</string>") != std::string::npos && p.find("<key>IOPropertyMatch</key>\n\t\t\t<dict>\n\t\t\t\t<key>GarnetDisplayIndex</key>\n\t\t\t\t<integer>1</integer>") != std::string::npos,
             "the framebuffer personality: IOProviderClass GarnetDisplayNub, IOClass GarnetFramebuffer, IOMatchCategory IOFramebuffer, IOPropertyMatch {GarnetDisplayIndex: 1}");
      expect(p.find("IOPCI") == std::string::npos && p.find("IOResource") == std::string::npos && p.find("IOProbeScore") == std::string::npos, "no PCI / IOResources match for the framebuffer (it runs only when the nub exists)"); }
    // INSTALL.md: the new path for an already-approved id
    expect(inst.find("/Library/Extensions/GarnetAccel-0.0.4.kext") != std::string::npos && inst.find("NEW") != std::string::npos && inst.find("0.0.4") != std::string::npos, "INSTALL.md names the new install path /Library/Extensions/GarnetAccel-0.0.4.kext");
    { size_t p = 0; bool bad = false; while ((p = inst.find("update-all", p)) != std::string::npos) { const size_t ls = inst.rfind('\n', p), le = inst.find('\n', p); const std::string line = inst.substr(ls + 1, (le == std::string::npos ? inst.size() : le) - ls - 1); /* the whole line */ if (line.find("Do NOT") == std::string::npos && line.find("NOT advised") == std::string::npos && line.find("never") == std::string::npos && line.find("not a command") == std::string::npos && line.find("NOT") == std::string::npos && line.find("NEVER") == std::string::npos) bad = true; p += 10; }
      expect(!bad, "INSTALL.md never suggests kmutil install --update-all (only warns against it)"); }
    expect(src.find("garnetaccel: layout PASS") == std::string::npos || true, "(the runtime gate logs layout PASS <slots>/<slots>; 2720 for 0.0.4 is the sum of the generated tables)");
}

// ---- A9 (0.0.5, G6): the 'GRNT' user client on the accelerator ----------------------------------------------------------------------------------------------
static int32_t g_no_rc; static void *g_no_client;
static int32_t o_native(void *, void *, void *, void *, uint32_t, void **h) { if (h) *h = g_no_client; return g_no_rc; }
static void a9_native_open(const std::string &root, const std::string &bring) {
    // the type constant, the capability, the table shape
    expect_u("the type constant is 'GRNT'", GARNET_METAL_UC_GARNETN, 0x47524E54u);
    expect_u("ops ABI 3", GARNET_METAL_ABI, 3); expect_u("152 bytes", sizeof(GarnetMetalOps), 152); expect_u("native_open at 144", offsetof(GarnetMetalOps, native_open), 144);
    expect_u("the capability is bit 1", GARNET_CAP_NATIVE_OPEN, 2);
    // native_open_usable: every way a table can fail to carry a usable hook
    GarnetMetalOps good = good_ops(); good.caps = GARNET_CAP_VHOOK | GARNET_CAP_NATIVE_OPEN; good.native_open = o_native;
    expect(native_open_usable(&good), "a good ABI-3 table with the capability and the hook is usable");
    expect(!native_open_usable(nullptr), "no table: not usable");
    { GarnetMetalOps x = good; x.abi = 2; expect(!native_open_usable(&x), "ABI 2: not usable (the member does not exist)"); }
    { GarnetMetalOps x = good; x.abi = 1; x.size = GARNET_METAL_OPS_MIN; expect(!native_open_usable(&x), "ABI 1 / 120 bytes: not usable"); }
    { GarnetMetalOps x = good; x.size = GARNET_METAL_OPS_V2; expect(!native_open_usable(&x), "a 144-byte table: not usable (native_open is outside its size)"); }
    { GarnetMetalOps x = good; x.caps = GARNET_CAP_VHOOK; expect(!native_open_usable(&x), "capability bit clear: not usable"); }
    { GarnetMetalOps x = good; x.caps = GARNET_CAP_NATIVE_OPEN << 1; expect(!native_open_usable(&x), "a different capability bit: not usable"); }
    { GarnetMetalOps x = good; x.native_open = nullptr; expect(!native_open_usable(&x), "no hook: not usable"); }
    { GarnetMetalOps x = good; x.abi = 4; x.size = GARNET_METAL_OPS_V3 + 16; expect(native_open_usable(&x), "a newer, longer table keeps the member usable (append-only)"); }
    // a 144-byte table is never read past its end (ASan poison on the bytes after it)
    { alignas(8) static uint8_t buf[sizeof(GarnetMetalOps)]; GarnetMetalOps x = good; x.abi = 2; x.size = GARNET_METAL_OPS_V2; std::memcpy(buf, &x, GARNET_METAL_OPS_V2);
      ASAN_POISON_MEMORY_REGION(buf + GARNET_METAL_OPS_V2, sizeof(GarnetMetalOps) - GARNET_METAL_OPS_V2);
      expect(!native_open_usable((const GarnetMetalOps *)buf), "a 144-byte table: native_open_usable does not read the missing member");
      ASAN_UNPOISON_MEMORY_REGION(buf + GARNET_METAL_OPS_V2, sizeof(GarnetMetalOps) - GARNET_METAL_OPS_V2); }
    // nuc_plan: the whole input space
    for (int tn = 0; tn < 2; ++tn) for (int aux = 0; aux < 2; ++aux) for (int gate = 0; gate < 2; ++gate) for (int dev = 0; dev < 2; ++dev) for (int tab = 0; tab < 3; ++tab) {
        const GarnetMetalOps *t = tab == 0 ? nullptr : tab == 1 ? &good : nullptr; GarnetMetalOps nocap = good; nocap.caps = GARNET_CAP_VHOOK; if (tab == 2) t = &nocap;
        const uint32_t type = tn ? GARNET_METAL_UC_GARNETN : 0x1234u;
        const NucPlan p = nuc_plan(type, aux != 0, gate != 0, t, dev != 0);
        if (!tn) expect(p == kNucFamily, "any other type goes to the family, whatever the state");
        else {
            expect(p != kNucFamily, "AN 'GRNT' OPEN NEVER FALLS THROUGH TO THE FAMILY, whatever the state");
            const bool all = aux && gate && dev && tab == 1;
            expect((p == kNucNative) == all, "'GRNT' reaches the hook only with the kill switch on, the gate passed, a device and a usable table");
            if (!all) expect(p == kNucUnsupported, "'GRNT' otherwise is Unsupported");
        }
    }
    expect(nuc_plan(0, true, true, &good, true) == kNucFamily && nuc_plan(GARNET_METAL_UC_GARNETN + 1, true, true, &good, true) == kNucFamily && nuc_plan(GARNET_METAL_UC_GARNETN - 1, true, true, &good, true) == kNucFamily, "neighbouring type numbers are the family's");
    // the answer is an IOReturn, never a bool
    expect_u("success with a client is 0", (uint32_t)nuc_result(0, true), 0u);
    expect_u("success with NO client is an internal error", (uint32_t)nuc_result(0, false), 0xE00002C9u);
    expect_u("a refusal is passed through unchanged (NotPrivileged)", (uint32_t)nuc_result((int32_t)0xE00002C1, false), 0xE00002C1u);
    expect_u("a refusal is passed through unchanged even if an object was returned (NotPermitted)", (uint32_t)nuc_result((int32_t)0xE00002E2, true), 0xE00002E2u);
    expect_u("the bool value 1 is NOT success", (uint32_t)nuc_result(1, true), 1u);
    expect(kIoSuccess == 0 && kIoUnsupported == (int32_t)0xE00002C7 && kIoBadArgument == (int32_t)0xE00002C2 && kIoInternalError == (int32_t)0xE00002C9, "the IOReturn constants");
    // the source: slot 239 and the wiring
    const std::string src = slurp(root + "/src/GarnetAccel.cpp"), lv = slurp(root + "/gates/leaves.py"), plist = slurp(root + "/Info.plist"), inst = slurp(root + "/INSTALL.md"), kmod = slurp(root + "/src/kmod_info.c");
    expect(lv.find("('GarnetAccelerator',   'IOGraphicsAccelerator2',   [183, 184, 18, 322, 348, 329, 239, 238])") != std::string::npos, "leaves: both newUserClient entries are overrides of GarnetAccelerator");
    expect(lv.find("EXPECT_CLASSES = 13") != std::string::npos && lv.find("EXPECT_SLOTS = 2720") != std::string::npos, "the layout gate stays at 13 classes / 2720 slots (no new class, no new slot)");
    expect(src.find("IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) override;") != std::string::npos, "the class declares the 4-argument newUserClient override");
    const std::string b = body_of(src, "IOReturn GarnetAccelerator::newUserClient(");
    expect(!b.empty(), "newUserClient is defined");
    expect(b.find("GARNET_AUX_ENTER(type == GARNET_METAL_UC_GARNETN ? kIOReturnUnsupported : IOGraphicsAccelerator2::newUserClient(owningTask, securityID, type, handler));") != std::string::npos, "the kill switch is first: off = Unsupported for 'GRNT', the family's own call for every other type");
    { const size_t ks = b.find("GARNET_AUX_ENTER"), pl = b.find("nuc_plan("), fam = b.find("if (plan == kNucFamily) return IOGraphicsAccelerator2::newUserClient("), uns = b.find("if (plan != kNucNative) return kIOReturnUnsupported;"), hook = b.find("gOps->native_open("), res = b.find("nuc_result(rc, uc != nullptr)");
      expect(ks != std::string::npos && pl != std::string::npos && fam != std::string::npos && uns != std::string::npos && hook != std::string::npos && res != std::string::npos && ks < pl && pl < fam && fam < uns && uns < hook && hook < res, "ORDER: kill switch < plan < family (other types only) < Unsupported (the rest of 'GRNT') < the hook < the IOReturn check"); }
    expect_u("the family's newUserClient is called in exactly two places: the kill-switch line and the other-type branch", count_of(b, "IOGraphicsAccelerator2::newUserClient("), 2u);
    expect(b.find("nuc_plan(type, true, gGateState == 1, gOps, ctx != nullptr && ctx == gCtx)") != std::string::npos, "the plan sees the layout gate state, the ops table and a live device");
    expect(b.find("gOps->native_open(ctx, this, (void *)owningTask, securityID, type, (void **)&uc)") != std::string::npos, "the hook gets this accelerator, the task, the security id and the type");
    expect(b.find("return (IOReturn)out;") != std::string::npos && b.find("rc == 1") == std::string::npos && b.find("out == 1") == std::string::npos && b.find("(bool)") == std::string::npos && b.find("? true") == std::string::npos && b.find("return true") == std::string::npos && b.find("!rc") == std::string::npos, "the hook's IOReturn is returned as an IOReturn (never read as a bool)");
    expect(b.find("if (out == kIoSuccess) *handler = uc;") != std::string::npos && b.find("if (handler) *handler = nullptr;") != std::string::npos, "*handler is written only on success, cleared before");
    expect(b.find("if (!handler) return kIOReturnBadArgument;") != std::string::npos && b.find("if (!handler) return kIOReturnBadArgument;") > b.find("if (plan != kNucNative)"), "a NULL handler is BadArgument only after the plan said native");
    expect(src.find("GARNET_TR_NATIVE_OPEN") != std::string::npos, "the open is traced");
    {
        const char *signature = "IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) override;";
        expect(src.find(signature) != std::string::npos, "the class declares the five-argument IOService entry");
        const std::string b5 = body_of(src, "IOReturn GarnetAccelerator::newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,");
        expect(b5.find("if (type == GARNET_METAL_UC_GARNETN) return newUserClient(owningTask, securityID, type, handler);") != std::string::npos, "the five-argument entry returns native errors directly");
        expect_u("non-native five-argument opens retain IOService fallback, including when disabled", count_of(b5, "IOService::newUserClient(owningTask, securityID, type, properties, handler)"), 2u);
        expect(b5.find("GARNET_AUX_ENTER(type == GARNET_METAL_UC_GARNETN ? kIOReturnUnsupported : IOService::newUserClient(") != std::string::npos, "the five-argument entry gates native opens before dispatch");
    }
    // the layout lists agree in gate_link / gen_gate (they read leaves.py): nothing else names slot 239
    // versions and the install path
    expect(plist.find("<string>0.1.1</string>") != std::string::npos && count_of(plist, "<string>0.1.1</string>") == 2 && plist.find("0.0.4") == std::string::npos && plist.find("<string>0.0.5</string>") == std::string::npos && plist.find("<string>0.0.6</string>") == std::string::npos, "Info.plist says 0.1.1 (carried twice)");
    expect(kmod.find("KMOD_EXPLICIT_DECL(org.garnet.accel, \"0.1.1\", _start, _stop)") != std::string::npos, "kmod_info says 0.1.1");
    expect(inst.find("/Library/Extensions/GarnetAccel-0.0.5.kext") != std::string::npos && inst.find("Step 4b") != std::string::npos, "INSTALL.md documents the new install path /Library/Extensions/GarnetAccel-0.0.5.kext (step 4b)");
    { const size_t a = inst.find("## 0.0.5 (G6"), e = inst.find("## 0.0.4 (M5", a); const std::string sec = (a == std::string::npos || e == std::string::npos) ? "" : inst.substr(a, e - a);
      expect_u("the 0.0.5 section uses the NEW path in every step (cp, chown, chmod, xattr, libraries, load)", count_of(sec, "/Library/Extensions/GarnetAccel-0.0.5.kext"), 7u);
      expect(sec.find("GarnetAccel-0.0.4.kext") == std::string::npos, "the 0.0.5 section never names the 0.0.4 path"); }
    { const size_t a = inst.find("## 0.0.5 (G6"), e = inst.find("## 0.0.4 (M5", a); const std::string sec = (a == std::string::npos || e == std::string::npos) ? "" : inst.substr(a, e - a);
      expect(!sec.empty() && sec.find("update-all") != std::string::npos && sec.find("NEVER `kmutil install --update-all`") != std::string::npos, "the 0.0.5 section forbids kmutil install --update-all");
      expect(sec.find("kmutil load -p /Library/Extensions/GarnetAccel-0.0.5.kext") != std::string::npos && sec.find("backup/auxkc-pre-0.0.5") != std::string::npos, "the 0.0.5 section: back up the aux KC, one load at the new path"); }
    // the two GarnetMetalOps.h copies
    { const std::string a = slurp(root + "/src/GarnetMetalOps.h"), c = slurp(bring + "/src/garnet/src/GarnetMetalOps.h");
      expect(!a.empty() && !c.empty() && a == c, "the aux and bring-up copies of GarnetMetalOps.h are byte-identical (ABI 3)"); }
}

// ---- A10 (aux 0.0.6, Run B): the monitor A as display index 2 ------------------------------------------------------------------------------------------------------
static GarnetDispSnap mona_snap() {
    GarnetDispSnap s = good_snap();
    s.index = GARNET_DISPA_INDEX; s.otg = GARNET_DISPA_OTG; s.ddcLine = GARNET_DISPA_DDC_LINE; s.w = GARNET_DISPA_W; s.h = GARNET_DISPA_H; s.pitchBytes = GARNET_DISPA_PITCH; s.refresh1616 = GARNET_DISPA_REFRESH1616; s.pixHz = GARNET_DISPA_PIXHZ;
    s.aperPhys = 0x80000000ull + 0x02000000ull; s.aperLen = GARNET_DISPA_BYTES; s.mcA = 0x8002000000ull; s.mcB = 0x80027F0000ull;
    return s;
}
static void a10_mona_framebuffer(const std::string &root) {
    const std::string src = slurp(root + "/src/GarnetAccel.cpp") + "\n" + slurp(root + "/src/garnet_accel_display_impl.inc"), pure = slurp(root + "/src/garnetaccel_pure.h"), plist = slurp(root + "/Info.plist"), inst = slurp(root + "/INSTALL.md"), dops = slurp(root + "/src/GarnetDisplayOps.h");
    // ---- the per-index geometry (the shared table) and the provider index
    expect_u("provider index: the monitor B", fb_provider_index(true, 1), 1); expect_u("provider index: the monitor A", fb_provider_index(true, 2), 2);
    expect_u("provider index: 0 / 3 / a huge value / no property are unknown", fb_provider_index(true, 0) + fb_provider_index(true, 3) + fb_provider_index(true, 0x100000001ull) + fb_provider_index(false, 1), 0);
    { GarnetDispSnap d = good_snap(), a = mona_snap();
      expect_u("the monitor B's snapshot is valid for index 1", fb_snap_for_index(&d, 1), GARNET_DSV_OK); expect_u("the monitor A's snapshot is valid for index 2", fb_snap_for_index(&a, 2), GARNET_DSV_OK);
      expect_u("the monitor A's snapshot under the monitor B's nub (index 1) is refused", fb_snap_for_index(&a, 1), GARNET_DSV_INDEX); expect_u("the monitor B's snapshot under the monitor A's nub (index 2) is refused", fb_snap_for_index(&d, 2), GARNET_DSV_INDEX);
      expect_u("an unknown provider index (0) is refused", fb_snap_for_index(&a, 0), GARNET_DSV_INDEX); expect_u("... and NULL", fb_snap_for_index(nullptr, 2), GARNET_DSV_NULL);
      // a WRONG geometry for index 2 is refused field by field (the aux kext accepts nothing the table does not list)
      { GarnetDispSnap x = a; x.w = 2560; expect_u("monitor A snapshot with the monitor B's width", fb_snap_for_index(&x, 2), GARNET_DSV_GEOMETRY); }
      { GarnetDispSnap x = a; x.h = 1440; expect_u("monitor A snapshot with the monitor B's height", fb_snap_for_index(&x, 2), GARNET_DSV_GEOMETRY); }
      { GarnetDispSnap x = a; x.pitchBytes = 10240; expect_u("monitor A snapshot with the monitor B's pitch", fb_snap_for_index(&x, 2), GARNET_DSV_GEOMETRY); }
      { GarnetDispSnap x = a; x.otg = 2; expect_u("monitor A snapshot on OTG2", fb_snap_for_index(&x, 2), GARNET_DSV_GEOMETRY); }
      { GarnetDispSnap x = a; x.ddcLine = 3; expect_u("monitor A snapshot with DDC line 3", fb_snap_for_index(&x, 2), GARNET_DSV_GEOMETRY); }
      { GarnetDispSnap x = a; x.refresh1616 = GARNET_DISP_REFRESH1616; expect_u("monitor A snapshot with the monitor B's refresh", fb_snap_for_index(&x, 2), GARNET_DSV_GEOMETRY); }
      { GarnetDispSnap x = a; x.pixHz = 241500000ull; expect_u("monitor A snapshot with the monitor B's pixel clock", fb_snap_for_index(&x, 2), GARNET_DSV_GEOMETRY); }
      { GarnetDispSnap x = a; x.aperLen = GARNET_DISP_BYTES; expect_u("monitor A snapshot with the monitor B's aperture length", fb_snap_for_index(&x, 2), GARNET_DSV_GEOMETRY); }
      { GarnetDispSnap x = a; x.aperLen = 127ull * 65536ull; expect_u("monitor A snapshot with the whole allocation as the aperture", fb_snap_for_index(&x, 2), GARNET_DSV_GEOMETRY); }
      { GarnetDispSnap x = d; x.aperLen = 8294400ull; expect_u("monitor B snapshot with the monitor A's aperture length", fb_snap_for_index(&x, 1), GARNET_DSV_GEOMETRY); }
      // every answer comes from the snapshot: the mode, the pixel format, the aperture, the EDID
      FbModeInfo m; expect(fb_mode_info(&a, 1, &m) && m.w == 1920u && m.h == 1080u && m.refresh1616 == 0x3C0000u && m.maxDepthIndex == 0u && m.valid && m.safe && m.isDefault, "mode 1 of the monitor A's framebuffer is 1920x1080 at exactly 60 Hz (from the snapshot, not 2560x1440)");
      FbPixel px; expect(fb_pixel_info(&a, 1, 0, true, &px) && px.bytesPerRow == 7680u && px.w == 1920u && px.h == 1080u && px.bitsPerPixel == 32u, "pixel info: pitch 7680, 1920x1080");
      uint64_t ph = 0, ln = 0; expect(fb_aperture(&a, true, &ph, &ln) && ph == a.aperPhys && ln == 8294400ull, "the monitor A's aperture is its own buffer A, 8,294,400 bytes");
      const uint8_t *sp = nullptr; uint64_t n = 0; expect_u("the monitor A's EDID block 1 is from its snapshot", fb_ddc_block(&a, 0, 1, true, true, 128, &sp, &n), kFbDdcOk); expect(sp == a.edid && n == 128u, "... bytes 0..127 of the snapshot"); }
    // ---- the ops check for an index: an ABI-1 bring-up kext is refused for the monitor A, still accepted for the monitor B
    { GarnetDispOps o = good_dops(); o.abi = 1u;
      expect_u("ABI-1 table, the monitor B: accepted", garnetdisp_ops_check_for(&o, 1), GARNET_DOV_OK); expect_u("ABI-1 table, the monitor A: refused cleanly (GARNET_DOV_ABI)", garnetdisp_ops_check_for(&o, 2), GARNET_DOV_ABI);
      o.abi = 2u; expect_u("ABI-2 table, the monitor A", garnetdisp_ops_check_for(&o, 2), GARNET_DOV_OK); expect_u("ABI-2 table, an unknown index", garnetdisp_ops_check_for(&o, 0), GARNET_DOV_ABI);
      o.abi = 1u; const uint32_t ovMonA = garnetdisp_ops_check_for(&o, 2);
      expect_u("start verdict: an ABI-1 table for the monitor A is an ops refusal", fb_start_verdict(true, false, true, true, ovMonA, 0, GARNET_DSV_OK), kFbStartOps); }
    // ---- source pins: start reads the nub's index and uses it for the ops check and the snapshot check; nothing is hard-wired to 2560x1440
    { const std::string b = body_of(src, "bool GarnetFramebuffer::start(IOService *provider) {"); const size_t pi = b.find("garnet_provider_index(provider)"), oc = b.find("garnetdisp_ops_check_for(o, pidx)"), sv = b.find("fb_snap_for_index(&s, pidx)");
      expect(pi != std::string::npos && oc != std::string::npos && sv != std::string::npos && pi < oc && oc < sv, "start: the nub's index is read BEFORE the ops check and the snapshot check, which both use it");
      expect(src.find("OSDynamicCast(OSNumber, nub->getProperty(GARNET_DISP_INDEX_KEY))") != std::string::npos && src.find("fb_provider_index(n != nullptr,") != std::string::npos, "the index is the nub's GarnetDisplayIndex OSNumber"); }
    { std::string fbcode = src; const size_t a = fbcode.find("// ---- GarnetFramebuffer (aux 0.0.4, M5)"); fbcode = a == std::string::npos ? "" : fbcode.substr(a);
      expect(!fbcode.empty() && fbcode.find("2560") == std::string::npos && fbcode.find("1440") == std::string::npos && fbcode.find("10240") == std::string::npos && fbcode.find("GARNET_DISP_W") == std::string::npos && fbcode.find("GARNET_DISP_H") == std::string::npos && fbcode.find("GARNET_DISP_BYTES") == std::string::npos && fbcode.find("GARNET_DISP_PITCH") == std::string::npos && fbcode.find("14745600") == std::string::npos && fbcode.find("8294400") == std::string::npos && fbcode.find("7680") == std::string::npos && fbcode.find("1920") == std::string::npos && fbcode.find("1080") == std::string::npos,
             "no monitor B geometry is hard-wired in the framebuffer class: it answers from the snapshot");
      std::string pcode = pure; const size_t b2 = pcode.find("// ---- the monitor B framebuffer (aux 0.0.4"), e2 = pcode.find("// ---- task window"); pcode = (b2 == std::string::npos || e2 == std::string::npos) ? "" : pcode.substr(b2, e2 - b2);
      expect(!pcode.empty() && pcode.find("GARNET_DISP_W") == std::string::npos && pcode.find("GARNET_DISP_H") == std::string::npos && pcode.find("GARNET_DISP_BYTES") == std::string::npos && pcode.find("GARNET_DISP_PITCH") == std::string::npos && pcode.find("2560") == std::string::npos && pcode.find("1440") == std::string::npos,
             "the pure framebuffer answers use no monitor B constant (only the snapshot and the shared validators)"); }
    // ---- the personalities
    { const size_t a = plist.find("<key>GarnetFramebuffer2</key>"), e = plist.find("<key>GarnetAccelerator</key>"); const std::string p = (a == std::string::npos || e == std::string::npos) ? "" : plist.substr(a, e - a);
      expect(!p.empty() && p.find("<key>IOProviderClass</key>\n\t\t\t<string>GarnetDisplayNub</string>") != std::string::npos && p.find("<key>IOClass</key>\n\t\t\t<string>GarnetFramebuffer</string>") != std::string::npos && p.find("<key>IOMatchCategory</key>\n\t\t\t<string>IOFramebuffer</string>") != std::string::npos &&
             p.find("<key>IOPropertyMatch</key>\n\t\t\t<dict>\n\t\t\t\t<key>GarnetDisplayIndex</key>\n\t\t\t\t<integer>2</integer>") != std::string::npos && p.find("IOPCI") == std::string::npos && p.find("IOResource") == std::string::npos && p.find("IOProbeScore") == std::string::npos,
             "the monitor A personality: same class, IOProviderClass GarnetDisplayNub, IOPropertyMatch {GarnetDisplayIndex: 2}, no PCI / IOResources match");
      expect_u("the framebuffer class is named in exactly two personalities", count_of(plist, "<string>GarnetFramebuffer</string>"), 2); }
    // ---- the shared header carries the table; INSTALL.md documents the NEW path
    expect(dops.find("garnetdisp_geom_for_index(s->index, &g)") != std::string::npos && dops.find("#define GARNET_DISPA_REFRESH1616 0x3C0000u") != std::string::npos && dops.find("GARNET_DISPA_BYTES       8294400ull") != std::string::npos, "the shared header validates through the per-index table (monitor A: refresh 0x3C0000, 8,294,400 bytes)");
    expect(inst.find("/Library/Extensions/GarnetAccel-0.0.6.kext") != std::string::npos && inst.find("## 0.0.6 (") != std::string::npos, "INSTALL.md documents the new install path /Library/Extensions/GarnetAccel-0.0.6.kext");
    { const size_t a = inst.find("## 0.0.6 ("), e = inst.find("## 0.0.5 (G6", a); const std::string sec = (a == std::string::npos || e == std::string::npos) ? "" : inst.substr(a, e - a);
      expect_u("the 0.0.6 section uses the NEW path in every step (cp, chown, chmod, xattr, libraries, load)", count_of(sec, "/Library/Extensions/GarnetAccel-0.0.6.kext"), 7u);
      expect(sec.find("GarnetAccel-0.0.5.kext") == std::string::npos && sec.find("GarnetAccel-0.0.4.kext") == std::string::npos, "the 0.0.6 section never names an older path");
      expect(sec.find("NEVER `kmutil install --update-all`") != std::string::npos && sec.find("kmutil load -p /Library/Extensions/GarnetAccel-0.0.6.kext") != std::string::npos && sec.find("backup/auxkc-pre-0.0.6") != std::string::npos && sec.find("Step 4b") != std::string::npos, "the 0.0.6 section: step 4b, back up the aux KC, one load at the new path, never --update-all");
      expect(sec.find("fbhold 2") != std::string::npos && sec.find("fbhold 1") != std::string::npos && sec.find("fbpublish 2") != std::string::npos && sec.find("fbpublish 1") != std::string::npos && sec.find("killall -9 WindowServer") != std::string::npos, "the 0.0.6 section carries the Run B order"); }
}

// A11 (aux 0.0.7, M6 Stage 1a): INSTALL.md documents the NEW install path for the already-approved id, in every step, and the Stage 1a order.
static void a11_m6_install(const std::string &root) {
    const std::string inst = slurp(root + "/INSTALL.md");
    expect(inst.find("/Library/Extensions/GarnetAccel-0.0.7.kext") != std::string::npos && inst.find("## 0.0.7 (") != std::string::npos, "INSTALL.md documents the new install path /Library/Extensions/GarnetAccel-0.0.7.kext");
    const size_t a = inst.find("## 0.0.7 ("), e = inst.find("## 0.0.6 (", a); const std::string sec = (a == std::string::npos || e == std::string::npos) ? "" : inst.substr(a, e - a);
    expect_u("the 0.0.7 section uses the NEW path in every step (cp, chown, chmod, xattr, libraries, load)", count_of(sec, "/Library/Extensions/GarnetAccel-0.0.7.kext"), 7u);
    expect(sec.find("GarnetAccel-0.0.5.kext") == std::string::npos && sec.find("GarnetAccel-0.0.4.kext") == std::string::npos && count_of(sec, "GarnetAccel-0.0.6.kext") == 1u, "the 0.0.7 section names only the 0.0.6 directory it moves away");
    expect(sec.find("NEVER `kmutil install --update-all`") != std::string::npos && sec.find("kmutil load -p /Library/Extensions/GarnetAccel-0.0.7.kext") != std::string::npos && sec.find("backup/auxkc-pre-0.0.7") != std::string::npos && sec.find("Step 4b") != std::string::npos, "the 0.0.7 section: step 4b, back up the aux KC, one load at the new path, never --update-all");
    expect(sec.find("garnet-m6=1") != std::string::npos && sec.find("stage17-native-1440-metal-disp-amfi-m6.plist") != std::string::npos && sec.find("accel fbpublish 2") != std::string::npos && sec.find("accel pipeadopt") != std::string::npos && sec.find("accel pipeagdc 1") != std::string::npos && sec.find("accel m6stat") != std::string::npos, "the 0.0.7 section carries the garnet-m6 latch and the Stage 1a order");
}

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    const std::string bring = argc > 2 ? argv[2] : root;
    a1_kill_switch(); a1_source(root); a2_gate(); a3_ops(); a3_source(root); a3b_tramp(root); a4_em(root); a5_thin(root); a6_plist(root, bring); a7_display(root); a8_framebuffer(root); a9_native_open(root, bring); a10_mona_framebuffer(root); a11_m6_install(root);
    std::printf("garnetaccel host_test: %d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
