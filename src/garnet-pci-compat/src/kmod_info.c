//
//  kmod_info.c — kmod entry glue for GarnetPCIBarCompat.kext.
//
#include <mach/mach_types.h>
#include <libkern/OSKextLib.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

extern kern_return_t garnet_pci_compat_kmod_start(kmod_info_t *ki, void *data);
extern kern_return_t garnet_pci_compat_kmod_stop(kmod_info_t *ki, void *data);

KMOD_EXPLICIT_DECL(org.garnet.pcibarcompat, "1.0.0", _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = garnet_pci_compat_kmod_start;
__private_extern__ kmod_stop_func_t  *_antimain = garnet_pci_compat_kmod_stop;
