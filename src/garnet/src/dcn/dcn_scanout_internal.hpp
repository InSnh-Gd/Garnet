// Internal declarations shared across Garnet's SDMA scanout, fastcopy,
// scanout control, and double-buffered flipmode translation units.
#pragma once

#include "../GarnetDriver.hpp"
#include "../GarnetDriverInternal.hpp"
#include "../GarnetDisplayOps.h"
#include "scanout_sdma.hpp"
#include "dcn.hpp"
#include "scanout_copy.h"
#include "gfx_flipmode.h"
#include "scanout_full.h"
#include "scanout_verify_pure.h"
#include "../regs.hpp"
#include "../amd/amdgpu_init.h"
#include "../amd/amdgpu_regs.h"
#include "../amd/amdgpu_sdma.h"
#include "../amd/amdgpu_sysmem.h"
#include "../amd/native_disp.h"
#include "../amd/native_display_identity.h"
#include "../amd/sdma_gcr.h"
#include "../amd/sdma_dcc.h"
#include "../amd/fastcopy.h"
#include "../amd/fastcopy89.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>

#define gBringup (garnet_bringup_ctx())

static constexpr uint32_t kScanPcX = 64, kScanPcY = 96, kScanPcW = 256, kScanPcH = 64;
static constexpr uint32_t kScanMaxRows = 256;
static constexpr uint32_t kScanCopyMaxRows = 2160, kScanChunkRows = 200, kScanCopyLogBudget = 12, kScanCopyFailLogBudget = 24;

// Refusal / status codes shared across scanout entry points (out[0]). 0 = done and verified.
enum : uint32_t {
	kScanStOk = 0, kScanStNoContext = 1, kScanStNoFramebuffer = 2, kScanStGeom = 3, kScanStMcBase = 4,
	kScanStQueue = 5, kScanStQueueBusy = 6, kScanStAlloc = 7, kScanStSourceCtl = 8, kScanStPreflight = 9,
	kScanStFence = 10, kScanStReadback = 11, kScanStPlan = 12, kScanStInterlock = 13, kScanStRing = 14,
	kScanStNoSave = 15,
	kScanStGcrAlign = 18, kScanStGcrRange = 19, kScanStGcrOverlap = 20,
	kScanStFlipMode = 21,
	kScanStFull = 22,
};

extern IOLock *gScanoutLock;
extern bool gScanoutPcPassed;
extern uint32_t gScanoutPcRuns, gScanoutCopies, gScanoutRefusals;
extern uint32_t gScanoutLastReason;
extern uint32_t *gScanoutSave;
extern uint32_t gScanoutSaveX, gScanoutSaveY, gScanoutSaveW, gScanoutSaveH;
extern uint32_t gScanoutCopyCalls, gScanoutCopyFailLogs;
extern volatile uint32_t gScanoutWrites;
extern rdna4_fm gFm;
extern IOLock *gFmLock;

uint64_t fm_displayed_off(uint64_t consoleOff, uint64_t consoleLen);
uint32_t scanout_geometry(amdgpu::DeviceContext &dev, RDNA4ScanoutGeom *g);
uint32_t scanout_queue_check(const amdgpu::DeviceContext &dev, const amdgpu::SDMAInstance &inst,
                             uint32_t *rptrOut);
uint32_t scanout_sdma_copies(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                             const uint64_t *srcMc, const uint64_t *dstMc, const uint32_t *bytes,
                             uint32_t count, uint32_t fenceVal, uint32_t timeoutMs,
                             uint64_t *elapsedUs, uint32_t *lastFence);
uint32_t scanout_sdma_tiled_copy(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                                 const RDNA4ScanoutTiledPlan *p, uint64_t tiledMc, uint64_t linearMc,
                                 bool detile, uint32_t fenceValue, uint32_t timeoutMs,
                                 uint64_t *usOut, uint32_t *lastFenceOut, bool gcr = false);
uint32_t scanout_sdma_copy_linear_gcr(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                                      uint64_t srcMc, uint64_t dstMc, uint32_t bytes, bool gcr,
                                      uint32_t fenceValue, uint32_t timeoutMs,
                                      uint64_t *usOut, uint32_t *lastFenceOut);
uint32_t scanout_sdma_copies_chunked(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                                     const uint64_t *srcMc, const uint64_t *dstMc,
                                     const uint32_t *bytes, uint32_t n, uint32_t fenceBase,
                                     uint32_t timeoutMs, uint64_t *usOut, uint32_t *lastFenceOut,
                                     uint32_t *chunksOut);
void scanout_free_vram(amdgpu::VRAMAllocation &a);
struct RDNA4FullCase {
	uint32_t w, h;
	uint32_t tileWrong, tileSampled;
	uint32_t detWrong, detSampled, poison, barMm, barSampled;
	uint64_t map[4];
	uint32_t nfirst;
	uint32_t fdx[8], fdy[8], fgot[8], fpx[8], fpy[8];
	uint64_t probeOff, tiledOff, destOff, probeMc, tiledMc, destMc, linBytes, tilBytes;
	uint64_t tileUs, detUs;
	uint32_t tileStatus, detStatus, tileFence, detFence;
	uint32_t st;
};
uint32_t scanout_full_case(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                           uint32_t w, uint32_t h, uint32_t fenceBase, RDNA4FullCase *r, bool uniform);
struct RDNA4VerifySource {
	uint64_t off; uint32_t stride; uint32_t surfW; uint32_t srcX, srcY; int tiled;
};
void rdna4_scanout_verify_cells(amdgpu::DeviceContext &dev, uint64_t fbOff, uint32_t dstX, uint32_t dstY,
                                uint32_t w, uint32_t h, uint32_t linPitch, const RDNA4VerifySource &s);
uint32_t rdna4_scanout_full(uint32_t mode, uint64_t *v);
