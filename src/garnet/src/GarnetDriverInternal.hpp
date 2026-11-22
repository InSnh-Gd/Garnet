//
//  GarnetDriverInternal.hpp — private internal bridge between GarnetDriver.cpp
//  and sibling core implementation modules (dcn/scanout_sdma.cpp, etc.).
//
#ifndef GarnetDriverInternal_hpp
#define GarnetDriverInternal_hpp

#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <stdint.h>

namespace amdgpu {
struct BringupContext;
}

extern IOLock *gVramMmLock;

amdgpu::BringupContext &garnet_bringup_ctx(void);
IOService *garnet_driver_pci(void);
IOService *garnet_driver_service(void);

uint32_t rdna4_gc_base(void);
uint32_t rdna4_reg_read32(uint32_t reg);
bool     rdna4_vram_read_mm(uint64_t vramOffset, uint32_t *dst, uint32_t dwords);
bool     rdna4_vram_write_mm(uint64_t vramOffset, const uint32_t *src, uint32_t dwords);
uint32_t rdna4_vram_apple_dest_check(uint64_t off, uint64_t len);
void     rdna4_sdmadcc_default(void);

#endif /* GarnetDriverInternal_hpp */

