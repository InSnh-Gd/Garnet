#pragma once

#include <stdint.h>

class IOService;
namespace rdna4peer { struct PrimaryView; }

// Returns a retained, unique primary on this GPU, with validated console and OTG0.
// Optional snapshot outputs are cleared on failure; callers release the service.
bool rdna4_display_primary(IOService **outRetainedFb,
                          rdna4peer::PrimaryView *out = nullptr,
                          uint64_t *outGeneration = nullptr);

// Revalidates identity only; each consumer must also validate its own interface.
bool rdna4_display_is_primary(IOService *fb);
