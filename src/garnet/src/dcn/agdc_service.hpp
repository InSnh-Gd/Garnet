#pragma once

#include <stdint.h>

uint32_t rdna4_agdc_native_control(uint64_t arg, uint64_t *out, unsigned count);
uint32_t rdna4_agdc_hold_control(uint64_t arg, uint64_t *out, unsigned count);
void     rdna4_agdc_m6_stat(uint64_t o[8]);
void     rdna4_agdc_m6_stat2(uint64_t o[12]);
