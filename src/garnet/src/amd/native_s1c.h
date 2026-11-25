//
//  native_s1c.h - the kernel half of native-stack step S1c (kext 0.0.601): the engine behind the native user client
//  (GarnetNativeClient, type 'GRNT'). Contract: an internal design note. Default OFF: nothing here runs unless a client opens,
//  and a client opens only on a native boot (rdna4-native=1 accepted) whose S1b self-test reported POSITIVE PASS.
//
//  One client at a time, on VMID 8 (0.0.627: up to eight with boot-arg garnet-multisession=1, VMIDs 8..15), with a fresh empty 4-level tree per client. All GPU work goes through the kernel GFX ring (the
//  KGQ) with the ring-safety rules of the contract section 6: a free-space check against the write-back rptr, one client lock, a
//  kext-global ring lock, and a HUNG latch after which memory is leaked instead of freed.
//
#pragma once
#include <stdint.h>
#include <IOKit/IOReturn.h>
#include <IOKit/IOTypes.h>
#include <mach/mach_types.h>   // task_t (0.0.612: n1c_bo_import_host)

#include "../GarnetNativeABI.h"
#include "../garnet_version.h"
#include "amdgpu_init.h"
#include "native_s1b.h"
#include "garnet_submit_pure.h"
#include "garnet_irq_wait_pure.h"

class IOMemoryDescriptor;
class IOUserClient;       // 0.0.641: n1c_unmap_for_close

namespace amdgpu {

constexpr uint32_t kN1cKextBuild = GARNET_KEXT_BUILD;   // Hello / QueryInfo must match the bundle and nub build

// Legacy-client sites refused once the native VM self-test has touched the GPU (contract 6.2). The legacy ring writers and the pool
// users that share vram_alloc without a lock.
enum N1cLegacySite : uint32_t { kN1cSiteSelfTest = 2, kN1cSiteAllocVRAM = 3, kN1cSiteFreeVRAM = 4, kN1cSiteWriteVRAM = 5 };
// True (refuse, log once per site) when native_s1b_latched(); false otherwise, which is EVERY non-native boot.
bool n1c_refuse_legacy(uint32_t site);

// 0.0.627 (GPU-apps G2): the caller's session. The client keeps the reference n1c_open filled in and passes it to every call; a stale reference (its
// session closed, or the slot re-opened by another client: a different id) is refused with NotReady. slot 0..7 = VMID 8..15; id 0 = none.
struct N1cRef { uint32_t slot; uint32_t id; };
// Open: gate checks, exclusivity, and the fresh tree. Returns kIOReturnSuccess, NotReady (gate / S1b / HUNG / device not ready),
// ExclusiveAccess (a client is open; 0.0.627 with boot-arg garnet-multisession=1: no free slot, a G1 test holds the GPU, or a WindowServer open while
// any session is open) or NoMemory. Logs one line per refusal. ws: the caller was admitted by the uid-88 (WindowServer) rule. *out: the session.
IOReturn n1c_open(BringupContext &ctx, bool ws = false, N1cRef *out = nullptr);
// Close (clientClose / clientDied / stop): idempotent. `how` names the caller in the log line.
void n1c_close(const N1cRef &ref, const char *how);
bool n1c_is_open();   // any session open
// 0.0.610 (ABI 1.8): read-only views for the Metal nub's gate. Hello done on the live session / the GPU declared HUNG this boot.
bool n1c_hello_done();
bool n1c_hung();

// Selector bodies. The caller (the client) has already checked scalar counts and struct sizes EXACTLY. Every one returns NotReady
// until Hello succeeded. Locking is described in the contract: all but QueryInfo / ReadRegs / WaitSeq take the client lock.
IOReturn n1c_hello(const N1cRef &ref, uint64_t clientAbi, uint64_t flags, uint64_t out[4]);
IOReturn n1c_query_info(const N1cRef &ref, rdna4n_info *out);
IOReturn n1c_read_regs(const N1cRef &ref, uint64_t dwordOff, uint64_t count, uint64_t instance, uint32_t *out);
IOReturn n1c_bo_create(const N1cRef &ref, const rdna4n_gem_create_in *in, uint64_t out[4]);
IOReturn n1c_bo_free(const N1cRef &ref, uint64_t handle);
// 0.0.612 (ABI 1.9), selector 21: import [hostVa, hostVa + size) of `task`'s address space (the caller's own: the client passes the task it was opened with) as a SYSTEM-memory BO. gpuVa == 0: handle
// only (map later with GemVa MAP); else map at import at gpuVa with the GemVa vm `flags`. out[0] handle [1] size [2] GPU VA mapped (0 if none) [3] RDNA4N_PLACED_HOST_IMPORT. Limits and order: native_hostimport_pure.h.
IOReturn n1c_bo_import_host(const N1cRef &ref, task_t task, uint64_t hostVa, uint64_t size, uint64_t flags, uint64_t gpuVa, uint64_t out[4]);
// 0.0.612 (review item A): latch this GPU's PCI BARs (n entries, base + FULL size each, 0 <= n <= 7) once at start; the first call wins, later calls are ignored. With nothing latched every host import is refused.
void n1c_latch_pci_bars(const uint64_t *base, const uint64_t *size, uint32_t n);
// 0.0.663 (ReBAR item 5): IOPCIFamily and config space disagree on BAR0 (address, or a BAR0 IOPCIFamily dropped, or the ReBAR size): true makes n1c_open refuse (NotReady). First call wins, like the BAR latch.
void n1c_latch_bar0_conflict(bool conflict);
IOReturn n1c_gem_va(const N1cRef &ref, const rdna4n_gem_va *in);
IOReturn n1c_ctx(const N1cRef &ref, const rdna4n_ctx *in, rdna4n_ctx *out);
IOReturn n1c_submit(const N1cRef &ref, const uint8_t *in, uint32_t size, uint64_t *seqOut);
IOReturn n1c_buffer_op(const N1cRef &ref, const rdna4n_buffer_op *in, bool fill, uint64_t *seqOut);
IOReturn n1c_wait(const N1cRef &ref, uint64_t target, uint64_t timeoutNs, uint64_t out[3]);
// Stage 2A: wake blocked native GFX fence/ring waiters from GarnetDriver::handleInterrupt()
// (CP EOP src_id 181, VM fault 0, CP errors 183..185). Leaf-locked and safe when no waiters exist.
void n1c_irq_notify();
bool n1c_irq_wait_enabled();
// ---- ABI 1.1 (kext 0.0.603, native S2a): the scanout selectors 9..14. Same shape rules as the selectors above (the client checked counts and
// sizes exactly); all need Hello. Lock order: gCliLock, then the scanout (DCN) lock inside rdna4dcn::scan*. Contract: NATIVE-S1C-ABI.md, "ABI 1.1 addendum".
IOReturn n1c_scan_query(const N1cRef &ref, rdna4n_scan_query *out);
IOReturn n1c_scan_acquire(const N1cRef &ref, uint64_t flags, uint64_t out[2]);
IOReturn n1c_scan_register(const N1cRef &ref, const rdna4n_scan_reg *in, uint64_t out[2]);
IOReturn n1c_scan_present(const N1cRef &ref, uint64_t slot, uint64_t flags, uint64_t out[3]);
IOReturn n1c_scan_status(const N1cRef &ref, rdna4n_scan_status *out);
IOReturn n1c_scan_release(const N1cRef &ref, uint64_t out[2]);
// ---- ABI 1.11 (0.0.661, M6 Stage 1b) + 1.12 (0.0.662, M6 Stage 2): selectors 22..26, instance 2 (the monitor B) or - with garnet-m6flip1 - 1 (the monitor A). inst must be one of them. Unsupported with garnet-m6 / garnet-m6flip OFF; Contract: NATIVE-S7-MULTIMON.md "Q2: Stage 1b build contract".
IOReturn n1c_scanx_acquire(const N1cRef &ref, uint64_t inst, uint64_t flags, uint64_t out[5]);   // 0.0.662: out[4] = the instance's geometry (w | h << 16 | pitch_px << 32); the client copies as many words as the caller asked for (4 or 5)
IOReturn n1c_scanx_register(const N1cRef &ref, uint64_t inst, const rdna4n_scan_reg *in, uint64_t out[2]);
IOReturn n1c_scanx_present(const N1cRef &ref, uint64_t inst, uint64_t slot, uint64_t flags, uint64_t out[3]);
IOReturn n1c_scanx_status(const N1cRef &ref, uint64_t inst, rdna4n_scan_status *out, uint64_t sout[4]);
IOReturn n1c_scanx_release(const N1cRef &ref, uint64_t inst, uint64_t out[2]);
// ---- ABI 1.2 (kext 0.0.604, native S2-DISPCLK): selector 15. Runs one named experiment step of an internal design note (1 E1b, 2 E2, 3 E3, 4 E4)
// against the DAL mailbox (smu_dal.cpp) and fills *out. BadArgument for a step outside 1..4 or non-zero flags, NotReady until Hello; otherwise
// Success with the verdict inside *out (DENIED when the gate, the rdna4-dalsmc level, the order, the latch or a concurrent step refuses it).
// The client lock is NOT held while the step runs (it sleeps for up to ~65 s): a busy flag refuses a second concurrent step.
IOReturn n1c_dal_step(const N1cRef &ref, uint64_t step, uint64_t flags, rdna4n_dal_result *out);
// ---- ABI 1.3 (kext 0.0.605, native S2d): selector 16. Runs one timed mode trial (row 50 or 120, dwell in ms) through rdna4dcn::modeTrial and fills *out (rdna4n_mode_result).
// BadArgument for non-zero flags or a null out, NotReady until Hello; otherwise Success with the verdict inside *out (DENIED when nothing was written). The client lock is NOT held
// while the trial runs (it sleeps for up to dwell + ~12 s); the trial's own busy flag refuses a second one.
IOReturn n1c_mode_hold(const N1cRef &ref, uint64_t maxMs, uint64_t flags, rdna4n_mode_result *out);   // 0.0.609 (ABI 1.7): the row-120 HELD mode; returns with the mode UP (or the trial's ordinary verdict)
IOReturn n1c_mode_release(const N1cRef &ref, uint64_t flags, rdna4n_mode_result *out);                // 0.0.609 (ABI 1.7): end the hold, wait, the final result
IOReturn n1c_mode_trial(const N1cRef &ref, uint64_t row, uint64_t dwellMs, uint64_t flags, rdna4n_mode_result *out);   // 0.0.606 (ABI 1.4): flags = RDNA4N_MODE_TF_*, the result is 512 B
// clientMemoryForType(type = BO handle): the +1 descriptor to map, or NotFound / NotPermitted.
IOReturn n1c_memory_for_handle(const N1cRef &ref, uint32_t handle, IOOptionBits *options, IOMemoryDescriptor **memory);

// ---- 0.0.635 (multi-monitor stages M4c / M4d, `accel disp2 plane 2`): the monitor B plane's two scanout buffers; 0.0.655: ONE PAIR PER INSTANCE (inst 1 = the monitor A's `disp2 plane 1`, inst 2 = the monitor B's). Allocated from the visible pool (vram_alloc) UNDER THE NATIVE
// CLIENT LOCK gCliLock, which is the lock every other user of the shared pools holds (lock order: gCliLock, then the display lock). `bytes` each, 64 KiB aligned (a multiple of 64 KiB: the monitor A's is 127 x 64 KiB = 8,323,072, the monitor B's 225 x 64 KiB),
// inside the BAR0 aperture (offset + size <= bar0Size). mc[k] = the MC address, off[k] = the BAR0 byte offset (0-based). At most ONE pair per instance is held: a second alloc for the same instance while held is refused (kIOReturnExclusiveAccess); the
// other instance's pair (held or pinned) never blocks it. Free gives BOTH back (the caller does it only after the instance's HUBP clock reads off); with none held it is a no-op. Neither touches a register or any memory.
IOReturn n1c_d2_alloc(BringupContext &ctx, uint32_t inst, uint64_t bytes, uint64_t mc[2], uint64_t off[2]);
void n1c_d2_free(uint32_t inst);
// Exclusive visible-VRAM allocator helpers serialized under gCliLock (for diagnostic direct-BAR tests).
bool n1c_vram_alloc_excl(BringupContext &ctx, uint64_t bytes, uint64_t align, VRAMAllocation *out);
bool n1c_vram_alloc_excl_at(BringupContext &ctx, uint64_t gpuVa, uint64_t bytes, VRAMAllocation *out);
void n1c_vram_free_excl(BringupContext &ctx, const VRAMAllocation &a);
// 0.0.652 (M5, `accel disp2 fbhold 2`): pin the pair for the rest of the boot (the framebuffer scans A, WindowServer maps it). pin: under gCliLock, true only when the allocator holds exactly mc[0] / mc[1] for that instance and nothing was pinned yet. While
// pinned n1c_d2_free is a LOGGED NO-OP (nothing returns to the allocator; the pair stays held, so a second alloc is refused as before). There is no unpin: a reboot is the release. 0.0.655: per instance.
bool n1c_d2_pin(uint32_t inst, const uint64_t mc[2]);
bool n1c_d2_pinned(uint32_t inst);

// ---- 0.0.623: GPU-apps stage G1, hang recovery (garnet_hang_recovery_pure.h). DEFAULT OFF: boot-arg rdna4-g1=1, latched once. -------------------------
bool n1c_g1_latched_on();
// 0.0.627: the uid-88 (WindowServer) note is now set by n1c_open itself (its `ws` argument) and cleared by that session's close (0.0.623-0.0.626: n1c_note_ws_session).
// accel actions 100 hangtest / 101 hangrecover / 102 hangstat. `o` receives rdna4native::g1::kOutN (13) scalars; o[0] is the verdict code.
IOReturn n1c_g1_verb(uint32_t action, uint64_t arg, BringupContext &ctx, uint64_t *o);

// ---- 0.0.627: GPU-apps stage G2, several sessions (garnet_multisession_pure.h). DEFAULT OFF: boot-arg garnet-multisession=1, latched once. -------------------
bool n1c_multi_latched_on();
// accel action 103 `sessstat <page>`: page 0..7 one session slot (VMID 8..15), 8 the global page. `o` receives rdna4native::g2::kOutN (13) scalars; read-only.
IOReturn n1c_g2_stat(uint64_t page, uint64_t *o);

// ---- 0.0.640: GPU-apps stage G4, ordinary user applications (garnet_app_policy_pure.h). DEFAULT OFF: boot-args garnet-apps=1 AND garnet-multisession=1, each latched once. -----------------
bool n1c_apps_enabled();                       // both latches ON
// Is the process called `comm` (proc_selfname of the opener) on the kernel allow-list? false whenever the role is disabled. The list is empty at every boot and changes only through accel 104.
bool n1c_app_allowed(const char *comm, bool *struckOut);   // 0.0.641: *struckOut = listed but out of strikes (3 recoveries blamed on that name this boot)
// Mark the session `ref` (just returned by n1c_open) as an APP session: it then refuses ReadRegs and the scanout / mode / DAL calls by itself. NotPermitted when the role is disabled.
// 0.0.641: `key` = the opener's executable-name key (the strike count's key).
IOReturn n1c_mark_app(const N1cRef &ref, uint64_t key);
bool n1c_ws_is_open();                          // 0.0.641: a WindowServer session is open (apps ON: the open policy's alreadyOpen input)
// 0.0.641: the client class calls this BEFORE n1c_close (apps ON only; a no-op otherwise): remove the CPU mappings IOUserClient still holds for the session's visible-VRAM BOs.
void n1c_unmap_for_close(const N1cRef &ref, IOUserClient *uc);
bool n1c_log_gate(uint32_t *suppressed);        // 0.0.641: rate limit for the "open refused" log line (true = log it; *suppressed = lines dropped in the last window)
// accel action 104 `appallow <add|remove|list|strikes>` and action 110 `appdeny <add|remove|list|clear>`: root-only (the legacy user client), exists only with the role enabled. `o` receives rdna4native::g4::kOutN (13) scalars; o[0] is the code.
IOReturn n1c_app_verb(uint64_t arg, uint64_t *o);
IOReturn n1c_app_deny_verb(uint64_t arg, uint64_t *o);

// ---- 0.0.650: GPU-apps stage G5 Stage 1, the application credit (garnet_app_credit_pure.h). DEFAULT OFF: boot-arg rdna4-g5=1 AND garnet-multisession=1, latched once. -----------------
bool n1c_g5_latched_on();                       // both latches ON
// accel action 103 `sessstat 9`: the G5 page (exists only with G5 ON). `o` receives rdna4native::g2::kOutN (13) scalars; read-only.
IOReturn n1c_g5_stat(uint64_t *o);

} // namespace amdgpu
