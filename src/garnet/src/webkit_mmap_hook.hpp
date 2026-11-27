//
//  webkit_mmap_hook.hpp — Modular seam for Garnet's WebKit executable-mmap hook.
//
//  Currently backed by Garnet's standalone version-independent MACF hook
//  (webkit_mmap_hook.cpp). Designed so a future Lilu migration can swap only
//  the hook installation backend without touching GarnetDriver or policy logic.
//
#ifndef Garnet_webkit_mmap_hook_hpp
#define Garnet_webkit_mmap_hook_hpp

class IOService;

namespace garnet_webkit {

// Install the _mac_file_check_mmap hook (unless disabled by garnet-webkitgpu=0)
// and publish IORegistry telemetry on `owner`. Safe to call once in start();
// fails closed on any verification error without breaking driver startup.
void install(IOService *owner);

// Restore original _mac_file_check_mmap bytes and drain active hook calls.
// Safe to call unconditionally in stop().
void uninstall();

} // namespace garnet_webkit

#endif // Garnet_webkit_mmap_hook_hpp
