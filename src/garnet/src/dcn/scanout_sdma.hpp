//
//  scanout_sdma.hpp — SDMA console scanout copies, bounded fastcopy staging,
//  double-buffered flip mode, and full scanout capture.
//
#ifndef RDNA4ScanoutSdma_hpp
#define RDNA4ScanoutSdma_hpp

#include <stdint.h>

class IOMemoryDescriptor;

void     rdna4_scanout_sdma_init_locks(void);
uint32_t rdna4_scanout_live_dims(uint32_t *w, uint32_t *h);
uint32_t rdna4_scanout_control(uint64_t arg, uint64_t *out, unsigned count);
uint32_t rdna4_scanout_copy_vram(uint64_t srcPhys, uint64_t srcLen, uint32_t srcW, uint32_t srcH, uint32_t srcStride,
                                 uint32_t srcX, uint32_t srcY, uint32_t dstX, uint32_t dstY, uint32_t w, uint32_t h,
                                 uint64_t *out, unsigned count, bool verify = true);
uint32_t rdna4_scanout_copy_tiled(uint64_t srcPhys, uint64_t srcLen, uint32_t surfW, uint32_t surfH, uint32_t swizzleMode,
                                  uint32_t dstX, uint32_t dstY, uint32_t w, uint32_t h,
                                  uint64_t *out, unsigned count, bool verify = true, bool gcr = false);
uint32_t rdna4_scanout_copy_staged(IOMemoryDescriptor *md, uint32_t srcStride, uint32_t srcW, uint32_t srcH,
                                   uint32_t dstX, uint32_t dstY, uint32_t w, uint32_t h, uint64_t *out, unsigned count);
bool     rdna4_scanout_pc_passed(void);

uint32_t rdna4_fc_mode_now(void);
void     rdna4_fc_set(uint32_t mode, uint64_t *extra, unsigned n);
void     rdna4_fc89_set(uint32_t mode, uint64_t *extra, unsigned n);
void     rdna4_fc89_report(uint64_t *v, unsigned n);
void     rdna4_fc_copy_report(uint64_t *v, unsigned n);

uint32_t rdna4_fm_on(void);
uint32_t rdna4_fm_present(uint64_t phys, uint64_t len, uint32_t surfW, uint32_t surfH, uint32_t swz,
                          uint32_t pw, uint32_t ph, bool linearCopy, bool verify, bool gcr, uint64_t presentId,
                          uint64_t *out, unsigned count, uint32_t *cstOut);
void     rdna4_fm_note_withdrawal(void);
uint32_t rdna4_fm_copy_buf(void);
uint32_t rdna4_fm_replay_blocked(void);
void     rdna4_fm_disarm(void);
uint32_t rdna4_fm_control(uint32_t m, uint32_t contRefused);
uint32_t rdna4_fm_test(uint32_t n, uint64_t *out, unsigned count);

uint32_t rdna4_scanfull_read(uint64_t off, void *dst, uint32_t max, uint64_t *total, uint64_t *seq);
uint32_t rdna4_sdma_copy_linear_sync(uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fenceVal, uint32_t timeoutMs);

#endif /* RDNA4ScanoutSdma_hpp */
