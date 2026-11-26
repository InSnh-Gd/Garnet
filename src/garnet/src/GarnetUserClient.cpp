//
//  GarnetUserClient.cpp — see the header for what this is and is not.
//
#include "GarnetUserClient.hpp"
#include "GarnetDriver.hpp"
#include "GarnetPrimaryFramebuffer.hpp"
#include "amd/amdgpu_init.h"
#include "amd/native_s1b.h"   // 0.0.600: native_s1b_refuse
#include "amd/native_disp.h"    // 0.0.613: the display verbs 83..87 open only with boot-arg garnet-metal-disp=1
#include "amd/native_disp_pure.h"
#include "amd/native_s1c.h"   // 0.0.601: n1c_refuse_legacy
#include "amd/garnet_hang_recovery_pure.h"   // 0.0.623 (G1): the hang-recovery verbs 100..102 open only with boot-arg garnet-g1=1
#include "amd/garnet_multisession_pure.h"   // 0.0.627 (G2): the session stat verb 103 opens only with boot-arg garnet-multisession=1
#include "amd/garnet_app_policy_pure.h"   // 0.0.640 (G4): the allow-list verb 104 opens only with boot-args garnet-apps=1 + garnet-multisession=1
#include "amd/amdgpu_cp.h"
#include "amd/amdgpu_gmc.h"
#include "amd/amdgpu_pm4.h"
#include "amd/rdna4_cap.h"      // 0.0.282: the binary capture ring
#include "amd/cp_pm4_gfx12.h"
#include "amd/compute_test.h"
#include "amd/rdna4_log.h"
#include "amd/amdgpu_smu.h"
#include <IOKit/IOLib.h>

#define UCLOG(fmt, ...) IOLog("GarnetUC: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(GarnetUserClient, IOUserClient)

IOMemoryDescriptor *rdna4_scanout_descriptor(bool viaFramebuffer);   // GarnetPrimaryFramebuffer.cpp
uint32_t rdna4_scanfull_read(uint64_t off, void *dst, uint32_t max, uint64_t *total, uint64_t *seq);   // GarnetDriver.cpp (0.0.542)

IOReturn GarnetUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) {
	if (!memory) return kIOReturnBadArgument;
	*memory = nullptr;
	if (type != 0x46425343u && type != 0x46425652u) return super::clientMemoryForType(type, options, memory);
	// Both legacy types expose the live scanout of a GPU that the migrated
	// display stack also writes. A userspace mapping cannot hold a display
	// lease, so it is never writable. This is unconditional on purpose: gating
	// on "peer active" would leave a mapping created before READY writable
	// after READY (a time-of-check/time-of-use hole). The numeric memory type
	// is unchanged; only the protection bit is forced.
	if (!options) return kIOReturnNotPermitted;
	*options |= kIOMapReadOnly;
	RDNA4LOG("scanout-map: type %#x is read-only", (unsigned)type);
	IOMemoryDescriptor *md = rdna4_scanout_descriptor(type == 0x46425652u);
	RDNA4LOG("scanout-map: client memory type %#x (%s) -> descriptor %p, caller options %#x", (unsigned)type,
	       type == 0x46425343u ? "fresh device range" : "AMDGarnetPrimaryFramebuffer getVRAMRange", md, options ? (unsigned)*options : 0u);
	if (!md) return kIOReturnNotFound;
	*memory = md;   // the caller's reference; IOUserClient::mapClientMemory64 releases it after mapping
	return kIOReturnSuccess;
}

bool GarnetUserClient::initWithTask(task_t owningTask, void *securityID,
                                    UInt32 type, OSDictionary *properties) {
	if (!super::initWithTask(owningTask, securityID, type, properties)) return false;
	// Root only. Everything below writes GPU registers and hands the command
	// processor addresses; it is a debugging interface, not a public one.
	if (clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) != kIOReturnSuccess) {
		UCLOG("open refused: caller is not privileged");
		return false;
	}
	task = owningTask;
	return true;
}

bool GarnetUserClient::start(IOService *provider) {
	owner = OSDynamicCast(GarnetDriver, provider);
	if (!owner) return false;
	if (!super::start(provider)) return false;
	opened = true;
	UCLOG("client opened (ABI %u)", RDNA4_UC_ABI_VERSION);
	return true;
}

void GarnetUserClient::stop(IOService *provider) {
	// Release anything the client left behind — a crashed test tool must not
	// leak VRAM for the life of the boot.
	auto *ctx = owner ? owner->bringupContext() : nullptr;
	uint32_t leaked = 0;
	for (uint32_t i = 0; i < kMaxAllocs; i++) {
		if (!slots[i].inUse) continue;
		if (!ctx) { slots[i].inUse = false; leaked++; continue; }
		auto &pool = slots[i].hi ? ctx->gmc.vram_alloc_hi : ctx->gmc.vram_alloc;
		if (pool.is_inited()) pool.free(slots[i].a);
		slots[i].inUse = false;
		leaked++;
	}
	if (leaked) UCLOG("client closed with %u allocation(s) still held — freed", leaked);
	opened = false;
	super::stop(provider);
}

IOReturn GarnetUserClient::clientClose() {
	if (opened) terminate();
	return kIOReturnSuccess;
}

bool GarnetUserClient::rangeIsOurs(uint64_t gpu_va, uint64_t len) const {
	if (len == 0) return false;
	for (uint32_t i = 0; i < kMaxAllocs; i++) {
		if (!slots[i].inUse) continue;
		const uint64_t base = slots[i].a.gpu_va;
		const uint64_t end  = base + slots[i].a.size;
		if (gpu_va >= base && gpu_va + len <= end && gpu_va + len > gpu_va) return true;
	}
	return false;
}

// ---------------------------------------------------------------------------

IOReturn GarnetUserClient::doGetInfo(IOExternalMethodArguments *args) {
	if (!args->structureOutput || args->structureOutputSize < sizeof(RDNA4Info))
		return kIOReturnBadArgument;
	auto *ctx = owner->bringupContext();
	auto *dev = owner->deviceContext();
	if (!ctx || !dev) return kIOReturnNotReady;

	RDNA4Info info {};
	info.abi_version   = RDNA4_UC_ABI_VERSION;
	info.stage_reached = (uint32_t)ctx->reached;
	info.stage_result  = (uint32_t)ctx->lastResult;
	const amdgpu::IPVersion v = dev->ip.getVersion(amdgpu::IPBlock::GC);
	info.gc_version    = ((uint32_t)v.major << 16) | ((uint32_t)v.minor << 8) | v.rev;
	info.vram_total_bytes = dev->vramSizeBytes;
	info.vram_free_bytes  = ctx->gmc.vram_alloc.is_inited() ? ctx->gmc.vram_alloc.bytes_free() : 0;
	info.vram_hi_total_bytes = ctx->gmc.vram_hi_size;
	info.vram_hi_free_bytes  = ctx->gmc.vram_alloc_hi.is_inited()
	                              ? ctx->gmc.vram_alloc_hi.bytes_free() : 0;
	info.vram_start_mc    = ctx->gmc.vram_start;
	info.doorbell_gfx_ring0   = ctx->cp.doorbell_index;
	info.cp_ring_size_dwords  = ctx->cp.ring_size_dwords;
	if (ctx->cp.inited && ctx->cp.fetch_proven)     info.flags |= kRDNA4FlagCPReady;
	if (ctx->mes.pipe[0].enabled)                   info.flags |= kRDNA4FlagMESReady;
	if (ctx->sdma.instance[0].inited)               info.flags |= kRDNA4FlagSDMAReady;
	if (owner->interruptsArmed())                   info.flags |= kRDNA4FlagIRQArmed;
	if (ctx->computePassed)                         info.flags |= kRDNA4FlagComputeOK;

	memcpy(args->structureOutput, &info, sizeof(info));
	args->structureOutputSize = sizeof(info);
	return kIOReturnSuccess;
}

IOReturn GarnetUserClient::doAllocVRAM(IOExternalMethodArguments *args) {
	if (amdgpu::n1c_refuse_legacy(amdgpu::kN1cSiteAllocVRAM)) return kIOReturnNotPermitted;   // NATIVE S1c (0.0.601): refused once the native VM self-test ran
	if (args->scalarInputCount < 2 || args->scalarOutputCount < 2) return kIOReturnBadArgument;
	auto *ctx = owner->bringupContext();
	if (!ctx || !ctx->gmc.vram_alloc.is_inited()) return kIOReturnNotReady;

	const uint64_t size  = args->scalarInput[0];
	uint64_t       align = args->scalarInput[1];
	const uint32_t flags = args->scalarInputCount >= 3 ? (uint32_t)args->scalarInput[2] : 0u;
	const bool     hi    = (flags & kRDNA4AllocDeviceOnly) != 0;
	// The low pool is the BAR0 window (~168 MiB); the device-only pool is the
	// rest of the card, so it gets a much larger ceiling.
	const uint64_t cap = hi ? (8ull << 30) : (256ull << 20);
	if (size == 0 || size > cap) return kIOReturnBadArgument;
	if (align == 0) align = 4096;
	if (align & (align - 1)) return kIOReturnBadArgument;

	auto &pool = hi ? ctx->gmc.vram_alloc_hi : ctx->gmc.vram_alloc;
	if (!pool.is_inited()) return kIOReturnNotReady;

	uint32_t slot = 0;
	for (uint32_t i = 1; i < kMaxAllocs; i++) if (!slots[i].inUse) { slot = i; break; }
	if (slot == 0) return kIOReturnNoResources;

	amdgpu::VRAMAllocation a {};
	if (!pool.alloc(size, align, &a)) return kIOReturnNoMemory;
	slots[slot].inUse = true;
	slots[slot].hi    = hi;
	slots[slot].a     = a;
	args->scalarOutput[0] = a.gpu_va;
	args->scalarOutput[1] = slot;
	return kIOReturnSuccess;
}

IOReturn GarnetUserClient::doFreeVRAM(IOExternalMethodArguments *args) {
	if (amdgpu::n1c_refuse_legacy(amdgpu::kN1cSiteFreeVRAM)) return kIOReturnNotPermitted;   // NATIVE S1c (0.0.601): refused once the native VM self-test ran
	if (args->scalarInputCount < 1) return kIOReturnBadArgument;
	const uint64_t h = args->scalarInput[0];
	if (h == 0 || h >= kMaxAllocs || !slots[h].inUse) return kIOReturnBadArgument;
	auto *ctx = owner->bringupContext();
	if (ctx) {
		auto &pool = slots[h].hi ? ctx->gmc.vram_alloc_hi : ctx->gmc.vram_alloc;
		if (pool.is_inited()) pool.free(slots[h].a);
	}
	slots[h].inUse = false;
	return kIOReturnSuccess;
}

IOReturn GarnetUserClient::doWriteVRAM(IOExternalMethodArguments *args) {
	if (amdgpu::n1c_refuse_legacy(amdgpu::kN1cSiteWriteVRAM)) return kIOReturnNotPermitted;   // NATIVE S1c (0.0.601): refused once the native VM self-test ran
	if (args->scalarInputCount < 1 || !args->structureInput) return kIOReturnBadArgument;
	const uint64_t gpu_va = args->scalarInput[0];
	const uint32_t len    = args->structureInputSize;
	if (len == 0 || len > RDNA4_UC_MAX_XFER) return kIOReturnBadArgument;
	if (!rangeIsOurs(gpu_va, len)) return kIOReturnNotPermitted;
	auto *ctx = owner->bringupContext();
	auto *dev = owner->deviceContext();
	if (!ctx || !dev) return kIOReturnNotReady;

	amdgpu::vram_memcpy(*dev, gpu_va - ctx->gmc.vram_start, args->structureInput, len);
	amdgpu::gmc_hdp_flush(*dev);
	return kIOReturnSuccess;
}

IOReturn GarnetUserClient::doReadVRAM(IOExternalMethodArguments *args) {
	if (args->scalarInputCount < 2 || !args->structureOutput) return kIOReturnBadArgument;
	const uint64_t gpu_va = args->scalarInput[0];
	const uint64_t len    = args->scalarInput[1];
	if (len == 0 || len > RDNA4_UC_MAX_XFER || len > args->structureOutputSize)
		return kIOReturnBadArgument;
	if ((len & 3) != 0) return kIOReturnBadArgument;
	if (!rangeIsOurs(gpu_va, len)) return kIOReturnNotPermitted;
	auto *ctx = owner->bringupContext();
	auto *dev = owner->deviceContext();
	if (!ctx || !dev) return kIOReturnNotReady;

	// Read through the MM_INDEX window: that is the GPU's own view of VRAM,
	// so what comes back is what a shader would see, not a stale CPU copy.
	const uint64_t off = gpu_va - ctx->gmc.vram_start;
	auto *out = static_cast<uint32_t *>(args->structureOutput);
	for (uint64_t i = 0; i < len / 4; i++)
		out[i] = amdgpu::RVRAM32_via_mm(*dev, off + i * 4);
	args->structureOutputSize = (uint32_t)len;
	return kIOReturnSuccess;
}

IOReturn GarnetUserClient::doSubmitIB(IOExternalMethodArguments *args) {
	if (amdgpu::native_s1b_refuse(1)) return kIOReturnNotPermitted;   // NATIVE S1b (0.0.600): refused once the VM self-test ran
	if (args->scalarInputCount < 3 || args->scalarOutputCount < 1) return kIOReturnBadArgument;
	const uint64_t gpu_va  = args->scalarInput[0];
	const uint32_t len_dw  = (uint32_t)args->scalarInput[1];
	const uint32_t vmid    = (uint32_t)args->scalarInput[2];
	if (len_dw == 0 || len_dw > 0x10000u) return kIOReturnBadArgument;
	if (!rangeIsOurs(gpu_va, (uint64_t)len_dw * 4)) return kIOReturnNotPermitted;
	auto *ctx = owner->bringupContext();
	auto *dev = owner->deviceContext();
	if (!ctx || !dev || !ctx->cp.inited) return kIOReturnNotReady;

	uint32_t fence = 0;
	kern_return_t r = amdgpu::cp_submit_ib(*dev, ctx->cp, gpu_va, len_dw, vmid, &fence);
	if (r == kIOReturnSuccess) owner->noteSubmit();
	args->scalarOutput[0] = fence;
	return r;
}

IOReturn GarnetUserClient::doWaitFence(IOExternalMethodArguments *args) {
	if (args->scalarInputCount < 2 || args->scalarOutputCount < 2) return kIOReturnBadArgument;
	const uint32_t fence      = (uint32_t)args->scalarInput[0];
	uint64_t       timeout_us = args->scalarInput[1];
	if (timeout_us == 0 || timeout_us > 10000000ull) timeout_us = 2000000ull;
	auto *ctx = owner->bringupContext();
	if (!ctx || !ctx->cp.inited) return kIOReturnNotReady;

	uint64_t observed = 0, elapsed = 0;
	kern_return_t r = amdgpu::cp_wait_fence(ctx->cp, fence, timeout_us, &observed, &elapsed);
	owner->noteFence(r == kIOReturnSuccess);
	args->scalarOutput[0] = observed;
	args->scalarOutput[1] = elapsed;
	return r;
}

IOReturn GarnetUserClient::doRegRead(IOExternalMethodArguments *args) {
	if (args->scalarInputCount < 1 || args->scalarOutputCount < 1) return kIOReturnBadArgument;
	auto *dev = owner->deviceContext();
	if (!dev) return kIOReturnNotReady;
	args->scalarOutput[0] = amdgpu::RREG32(*dev, (uint32_t)args->scalarInput[0]);
	return kIOReturnSuccess;
}

// The stress tests run entirely in the kernel: a hundred thousand round trips
// through IOConnectCallMethod would measure IOKit, not the GPU.
IOReturn GarnetUserClient::doSelfTest(IOExternalMethodArguments *args) {
	if (amdgpu::n1c_refuse_legacy(amdgpu::kN1cSiteSelfTest)) return kIOReturnNotPermitted;   // NATIVE S1c (0.0.601): refused once the native VM self-test ran
	if (!args->structureInput || args->structureInputSize < sizeof(RDNA4SelfTestIn))
		return kIOReturnBadArgument;
	if (!args->structureOutput || args->structureOutputSize < sizeof(RDNA4SelfTestOut))
		return kIOReturnBadArgument;
	RDNA4SelfTestIn in {};
	memcpy(&in, args->structureInput, sizeof(in));
	auto *ctx = owner->bringupContext();
	auto *dev = owner->deviceContext();
	if (!ctx || !dev || !ctx->cp.inited) return kIOReturnNotReady;
	if (in.iterations == 0 || in.iterations > 1000000u) return kIOReturnBadArgument;
	uint64_t timeout = in.timeout_us ? in.timeout_us : 1000000ull;

	RDNA4SelfTestOut out {};
	out.test_id = in.test_id;

	// The compute test owns its own buffers and verification; run it as many
	// times as asked. This is how a shader change gets exercised without a new
	// kext: rebuild the blob, reload, run test 3.
	if (in.test_id == kRDNA4TestCompute) {
		for (uint32_t i = 0; i < in.iterations; i++) {
			amdgpu::ComputeTestResult cr {};
			kern_return_t r = amdgpu::compute_dispatch_test(*dev, ctx->gmc, ctx->cp, &cr, ctx->hsaAbi);
			out.last_observed = cr.observed[0];
			out.last_fence    = cr.fence_expected;
			out.elapsed_us   += cr.elapsed_us;
			owner->noteSubmit();
			owner->noteFence(cr.fence_landed);
			if (r != kIOReturnSuccess || cr.lanes_mismatched != 0) {
				out.failures++;
				if (out.kr == 0) { out.kr = (uint32_t)(r ? r : kIOReturnIOError); out.first_failure_iter = i; }
				break;
			}
			out.iterations_run++;
		}
		UCLOG("selftest compute: %u/%u ok, %u failures, last lane0 %#010x",
		      out.iterations_run, in.iterations, out.failures, out.last_observed);
		memcpy(args->structureOutput, &out, sizeof(out));
		args->structureOutputSize = sizeof(out);
		return kIOReturnSuccess;
	}

	// One scratch page + one IB page for the whole run.
	amdgpu::VRAMAllocation ib {}, target {};
	if (!ctx->gmc.vram_alloc.alloc(4096, 4096, &ib)) return kIOReturnNoMemory;
	if (!ctx->gmc.vram_alloc.alloc(4096, 4096, &target)) {
		ctx->gmc.vram_alloc.free(ib);
		return kIOReturnNoMemory;
	}
	const uint64_t ib_off  = ib.gpu_va - ctx->gmc.vram_start;
	const uint64_t tgt_off = target.gpu_va - ctx->gmc.vram_start;

	uint64_t elapsed_total = 0;
	for (uint32_t i = 0; i < in.iterations; i++) {
		uint32_t dw[8];
		uint32_t n = 0;
		uint32_t magic = 0;
		switch (in.test_id) {
		case kRDNA4TestNopSubmit:
			dw[n++] = amdgpu::cp_p3_nop1();
			dw[n++] = amdgpu::cp_p3_nop1();
			break;
		case kRDNA4TestWriteData:
		case kRDNA4TestFenceBurst:
			magic = 0xA5A50000u + i;
			dw[n++] = amdgpu::cp_p3(amdgpu::kP3_WRITE_DATA, 3);
			dw[n++] = amdgpu::pm4_write_data_control(amdgpu::kPM4WriteDataEngineME,
			                                         amdgpu::kPM4WriteDataDstSelMemory, true);
			dw[n++] = (uint32_t)(target.gpu_va & 0xFFFFFFFFu);
			dw[n++] = (uint32_t)(target.gpu_va >> 32);
			dw[n++] = magic;
			break;
		default:
			ctx->gmc.vram_alloc.free(target); ctx->gmc.vram_alloc.free(ib);
			return kIOReturnUnsupported;
		}

		amdgpu::bar0_memcpy_to_vram(*dev, ib_off, dw, n * 4);
		amdgpu::gmc_hdp_flush(*dev);

		uint32_t fence = 0;
		kern_return_t r = amdgpu::cp_submit_ib(*dev, ctx->cp, ib.gpu_va, n, 0, &fence);
		if (r != kIOReturnSuccess) {
			out.failures++;
			if (out.kr == 0) { out.kr = (uint32_t)r; out.first_failure_iter = i; }
			break;
		}
		owner->noteSubmit();
		out.last_fence = fence;

		// FenceBurst does not wait every iteration — it lets fences pile up and
		// checks only at the end, which is what exercises the ring's wrap and
		// the CP's ability to stay ahead of the host.
		if (in.test_id != kRDNA4TestFenceBurst || i + 1 == in.iterations) {
			uint64_t observed = 0, el = 0;
			r = amdgpu::cp_wait_fence(ctx->cp, fence, timeout, &observed, &el);
			elapsed_total += el;
			owner->noteFence(r == kIOReturnSuccess);
			if (r != kIOReturnSuccess) {
				out.failures++;
				if (out.kr == 0) { out.kr = (uint32_t)r; out.first_failure_iter = i; }
				break;
			}
			if (in.test_id != kRDNA4TestNopSubmit) {
				const uint32_t got = amdgpu::RVRAM32_via_mm(*dev, tgt_off);
				out.last_observed = got;
				if (got != magic) {
					out.failures++;
					if (out.kr == 0) { out.kr = (uint32_t)kIOReturnIOError; out.first_failure_iter = i; }
					break;
				}
			}
		}
		out.iterations_run++;
	}
	out.elapsed_us = elapsed_total;

	ctx->gmc.vram_alloc.free(target);
	ctx->gmc.vram_alloc.free(ib);

	UCLOG("selftest %u: %u/%u iterations, %u failures (first at %u), kr=%#x, %llu us",
	      out.test_id, out.iterations_run, in.iterations, out.failures,
	      out.first_failure_iter, out.kr, out.elapsed_us);
	memcpy(args->structureOutput, &out, sizeof(out));
	args->structureOutputSize = sizeof(out);
	return kIOReturnSuccess;
}

IOReturn GarnetUserClient::doGetCounters(IOExternalMethodArguments *args) {
	if (!args->structureOutput || args->structureOutputSize < sizeof(RDNA4Counters))
		return kIOReturnBadArgument;
	RDNA4Counters c {};
	owner->fillCounters(&c);
	memcpy(args->structureOutput, &c, sizeof(c));
	args->structureOutputSize = sizeof(c);
	return kIOReturnSuccess;
}

// The driver's own log ring (rdna4_log.h) — independent of macOS logging, which
// stopped carrying these lines once the kext moved into the boot collection.
IOReturn GarnetUserClient::doReadLog(IOExternalMethodArguments *args) {
	if (args->scalarInputCount < 1 || !args->structureOutput) return kIOReturnBadArgument;
	if (args->scalarOutputCount < 1) return kIOReturnBadArgument;
	const uint32_t offset = (uint32_t)args->scalarInput[0];
	uint32_t max = args->structureOutputSize;
	if (max > RDNA4_UC_MAX_XFER) max = RDNA4_UC_MAX_XFER;
	uint32_t total = 0;
	const uint32_t got = amdgpu::rdna4_log_read(offset, args->structureOutput, max, &total);
	args->structureOutputSize = got;
	args->scalarOutput[0] = total;
	return kIOReturnSuccess;
}

// Live telemetry from PMFW. Refuses rather than guessing if the firmware's
// driver-interface version is not the one our table layout was written for.
//: drop the log. The buffer stops appending when full (by design -- the
// FIRST failure matters most), so without this every later measurement is
// invisible while `log` still returns a complete-looking 512 KiB.
IOReturn GarnetUserClient::doLogReset(IOExternalMethodArguments *args) {
	(void)args;
	amdgpu::rdna4_log_reset();
	return kIOReturnSuccess;
}

// 0.0.276: the streaming reader's consume. Removes only bytes the reader says it has copied.
IOReturn GarnetUserClient::doLogConsume(IOExternalMethodArguments *args) {
	if (args->scalarInputCount < 1 || args->scalarOutputCount < 4) return kIOReturnBadArgument;
	uint64_t consumed = 0, dropped = 0;
	const uint32_t held = amdgpu::rdna4_log_consume((uint32_t)args->scalarInput[0], &consumed, &dropped);
	args->scalarOutput[0] = held;
	args->scalarOutput[1] = consumed;
	args->scalarOutput[2] = dropped;
	args->scalarOutput[3] = amdgpu::rdna4_log_overflowed() ? 1u : 0u;
	return kIOReturnSuccess;
}

// 0.0.282: the binary capture ring, read and consumed exactly as the log is (0.0.276).
IOReturn GarnetUserClient::doReadCap(IOExternalMethodArguments *args) {
	if (args->scalarInputCount < 1 || !args->structureOutput) return kIOReturnBadArgument;
	if (args->scalarOutputCount < 1) return kIOReturnBadArgument;
	const uint32_t offset = (uint32_t)args->scalarInput[0];
	uint32_t max = args->structureOutputSize;
	if (max > RDNA4_UC_MAX_XFER) max = RDNA4_UC_MAX_XFER;
	uint32_t total = 0;
	const uint32_t got = amdgpu::rdna4_cap_read(offset, args->structureOutput, max, &total);
	args->structureOutputSize = got;
	args->scalarOutput[0] = total;
	return kIOReturnSuccess;
}

IOReturn GarnetUserClient::doCapConsume(IOExternalMethodArguments *args) {
	if (args->scalarInputCount < 1 || args->scalarOutputCount < 4) return kIOReturnBadArgument;
	uint64_t consumed = 0, dropped = 0, records = 0;
	const uint32_t held = amdgpu::rdna4_cap_consume((uint32_t)args->scalarInput[0], &consumed, &dropped, &records);
	args->scalarOutput[0] = held;
	args->scalarOutput[1] = consumed;
	args->scalarOutput[2] = dropped;
	args->scalarOutput[3] = records;
	return kIOReturnSuccess;
}

IOReturn GarnetUserClient::doMetrics(IOExternalMethodArguments *args) {
	if (!args->structureOutput || args->structureOutputSize < sizeof(RDNA4Metrics))
		return kIOReturnBadArgument;
	auto *dev = owner->deviceContext();
	if (!dev) return kIOReturnNotReady;

	amdgpu::SmuMetricsResult res {};
	amdgpu::SmuSeq smuSeq;   // 0.0.604: the whole table-transfer sequence under the shared mailbox lock (PPSMC and DAL)
	kern_return_t r = amdgpu::smu_read_metrics(*dev, &res);
	if (r != kIOReturnSuccess) return r;
	amdgpu::smu_log_metrics(res);
	const amdgpu::SmuMetrics &m = res.metrics;

	RDNA4Metrics out {};
	out.if_version      = res.if_version;
	out.layout_verified = res.layout_verified ? 1u : 0u;
	out.values_plausible = res.values_plausible ? 1u : 0u;
	out.gfxclk_mhz       = amdgpu::smu_effective_gfxclk_mhz(m);
	out.socclk_mhz       = m.CurrClock[amdgpu::SMUClk::SOCCLK];
	out.uclk_mhz         = m.CurrClock[amdgpu::SMUClk::UCLK];
	out.fclk_mhz         = m.CurrClock[amdgpu::SMUClk::FCLK];
	out.temp_edge_c      = m.AvgTemperature[amdgpu::SMUTemp::EDGE];
	out.temp_hotspot_c   = m.AvgTemperature[amdgpu::SMUTemp::HOTSPOT];
	out.temp_mem_c       = m.AvgTemperature[amdgpu::SMUTemp::MEM];
	out.socket_power_w   = m.AverageSocketPower;
	out.board_power_w    = m.AverageTotalBoardPower;
	out.gfx_activity_pct = m.AverageGfxActivity;
	out.mem_activity_pct = m.AverageUclkActivity;
	out.fan_rpm          = m.AvgFanRpm;
	out.fan_pwm_pct      = m.AvgFanPwm;
	out.pcie_gen         = m.PcieRate;
	out.pcie_width       = m.PcieWidth;
	out.metrics_counter  = m.MetricsCounter;
	memcpy(args->structureOutput, &out, sizeof(out));
	args->structureOutputSize = sizeof(out);
	return kIOReturnSuccess;
}

// Clamp the GFX clock. Only PPCLK_GFXCLK is touched, so display scanout
// (DISPCLK/DCFCLK) is unaffected.
IOReturn GarnetUserClient::doPowerState(IOExternalMethodArguments *args) {
	if (args->scalarInputCount < 1) return kIOReturnBadArgument;
	const uint32_t state = (uint32_t)args->scalarInput[0];
	if (state > 4) return kIOReturnBadArgument;
	auto *dev = owner->deviceContext();
	if (!dev) return kIOReturnNotReady;
	amdgpu::SmuSeq smuSeq;   // 0.0.604: both clock messages of the power state under the shared mailbox lock
	return amdgpu::smu_set_power_state(*dev, state);
}


// build 0.0.542 (dcn/scanout_full.h): a slice of the published full-res scanout capture. Read-only; like ReadCap, at most
// RDNA4_UC_MAX_XFER bytes per call. kIOReturnNotFound when nothing is published.
IOReturn GarnetUserClient::doReadScanFull(IOExternalMethodArguments *args) {
	if (args->scalarInputCount < 1 || !args->structureOutput || args->scalarOutputCount < 2) return kIOReturnBadArgument;
	uint32_t max = args->structureOutputSize;
	if (max > RDNA4_UC_MAX_XFER) max = RDNA4_UC_MAX_XFER;
	uint64_t total = 0, seq = 0;
	const uint32_t got = rdna4_scanfull_read(args->scalarInput[0], args->structureOutput, max, &total, &seq);
	args->structureOutputSize = got;
	args->scalarOutput[0] = total;
	args->scalarOutput[1] = seq;
	return total ? kIOReturnSuccess : kIOReturnNotFound;
}

// Native bringup / display verb dispatch (`accelExperiment`).
IOReturn GarnetUserClient::doAccelExperiment(IOExternalMethodArguments *args) {
	if (args->scalarInputCount < 1) return kIOReturnBadArgument;
	const uint32_t action = (uint32_t)args->scalarInput[0];
	// Native verbs:
	//   60 = scanout, 74..77 = dcnstate / dcnvbl / dcnflip / dcnmode, 78 = fbname,
	//   79 = agdchold, 82 = sdmadcc, 83..90 = native display pipe, 106..107 = m6, 108 = vramstat,
	//   91..99 = ddcread / dmubring / dispcensus / region4read / dmub* / disp2 / scdcread,
	//   100..102 = hangtest / hangrecover / hangstat (G1), 103 = sessstat (G2/G5),
	//   104 = appallow (G4), 105 = fbpublish, 109 = bartest (direct CPU BAR0 <-> GPU test), 110 = appdeny (G4).
	if (rdna4rebar::bartest_is_action(action)) {
		// 109 is admitted by the admin-only legacy client (its own argument/mode gate runs in accelExperiment)
	} else
	if (rdna4native::g4::action_admitted(amdgpu::n1c_apps_enabled(), action)) {
		// 104 is admitted: it skips the old bound below (the verb's own argument check is rdna4native::g4::native_exempt in accelExperiment)
	} else
	if (!rdna4native::g1::action_admitted(amdgpu::n1c_g1_latched_on(), action) && !rdna4native::g2::action_admitted(amdgpu::n1c_multi_latched_on(), action)) {
	if (action > 82 && !rdna4disp::action_admitted(rdna4disp_latched_on(), action)) return kIOReturnBadArgument;   // 0 status, 1 fire, 2 memenable, 3 synctables, 4 enablerings, 5 startengines
	}
	uint64_t installed = 0, calls = 0, firstUnsup = 0;
	uint64_t extra[GarnetDriver::kAccelExtraScalars] = { 0 };
	// 0.0.194: scalarInput[1] is an optional verb argument (vmib's VA). An older
	// rdna4test sends only the action, which lands here as 0 — every verb that
	// reads it treats 0 as "use the default".
	const uint64_t argScalar = (args->scalarInputCount >= 2) ? args->scalarInput[1] : 0;
	IOReturn kr = owner->accelExperiment(action, &installed, &calls, &firstUnsup, extra,
	                                     argScalar);
	if (args->scalarOutputCount >= 3) {
		args->scalarOutput[0] = installed;
		args->scalarOutput[1] = calls;
		args->scalarOutput[2] = firstUnsup;
	}
	// Verb-specific results ride back as scalars so no log flood can erase them
	// (rule 14). An older rdna4test asking for only 3 outputs simply misses them.
	for (unsigned i = 0; i < GarnetDriver::kAccelExtraScalars; i++)
		if (args->scalarOutputCount >= 4 + i) args->scalarOutput[3 + i] = extra[i];
	return kr;
}

IOReturn GarnetUserClient::externalMethod(uint32_t selector,
                                          IOExternalMethodArguments *args,
                                          IOExternalMethodDispatch *dispatch,
                                          OSObject *target, void *reference) {
	if (!owner || !args) return kIOReturnNotReady;
	switch (selector) {
	case kRDNA4SelGetInfo:     return doGetInfo(args);
	case kRDNA4SelAllocVRAM:   return doAllocVRAM(args);
	case kRDNA4SelFreeVRAM:    return doFreeVRAM(args);
	case kRDNA4SelWriteVRAM:   return doWriteVRAM(args);
	case kRDNA4SelReadVRAM:    return doReadVRAM(args);
	case kRDNA4SelSubmitIB:    return doSubmitIB(args);
	case kRDNA4SelWaitFence:   return doWaitFence(args);
	case kRDNA4SelRegRead:     return doRegRead(args);
	case kRDNA4SelSelfTest:    return doSelfTest(args);
	case kRDNA4SelGetCounters: return doGetCounters(args);
	case kRDNA4SelReadLog:     return doReadLog(args);
	case kRDNA4SelLogReset:    return doLogReset(args);
	case kRDNA4SelLogConsume:  return doLogConsume(args);
	case kRDNA4SelReadCap:     return doReadCap(args);      // 0.0.282
	case kRDNA4SelCapConsume:  return doCapConsume(args);
	case kRDNA4SelReadScanFull: return doReadScanFull(args);   // build 0.0.542 (`scanout full`)
	case kRDNA4SelMetrics:     return doMetrics(args);
	case kRDNA4SelPowerState:  return doPowerState(args);
	case kRDNA4SelAccelExperiment: return doAccelExperiment(args);
	default: break;
	}
	return super::externalMethod(selector, args, dispatch, target, reference);
}
