#include <mach/mach_types.h>
#include <libkern/OSKextLib.h>
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
static kern_return_t fixture_start(kmod_info_t *info, void *data) { (void)info; (void)data; return KERN_SUCCESS; }
static kern_return_t fixture_stop(kmod_info_t *info, void *data) { (void)info; (void)data; return KERN_SUCCESS; }
KMOD_EXPLICIT_DECL(org.garnet.vmfixture, "0.0.1", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = fixture_start;
__private_extern__ kmod_stop_func_t *_antimain = fixture_stop;
