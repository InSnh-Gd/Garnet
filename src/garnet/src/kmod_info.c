//
//  kmod_info.c — hand-written kmod glue for Garnet.kext.
//  Link order: <objects> -lkmodc++ kmod_info.o -lkmod.
//
#include <mach/mach_types.h>
#include <libkern/OSKextLib.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

static kern_return_t garnet_kmod_start(kmod_info_t *ki, void *data) { (void)ki; (void)data; return KERN_SUCCESS; }
static kern_return_t garnet_kmod_stop (kmod_info_t *ki, void *data) { (void)ki; (void)data; return KERN_SUCCESS; }

KMOD_EXPLICIT_DECL(org.garnet.driver, "0.0.673", _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = garnet_kmod_start;
__private_extern__ kmod_stop_func_t  *_antimain = garnet_kmod_stop;
