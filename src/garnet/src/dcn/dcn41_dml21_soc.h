#pragma once

#include "dcn41_atom_pure.h"
#include "dcn41_dml21_bridge.h"
#include "amd/amdgpu_asic.h"
#include "amd/smu_dal_pure.h"

namespace rdna4dml {

enum class PowerSource : uint8_t { AC, DC };

bool prepareSoc(amdgpu::AsicId asic, const rdna4atom::Board &board,
                const rdna4dal::ClockSnapshot &clocks, PowerSource source, Soc &output);

} // namespace rdna4dml
