// garnet_cienv.h (bundle 9, app crash study item 1): the Core Image switch that keeps CIKL string kernels off the nil -[MTLDevice compiler] path.
// Pure C so it is host-testable (test-cienv.c).  Metal app compatibility.
//
// Why: -[CIKernel _initWithString:...] reflects the kernel-language source through [device newLibraryWithSource:] and asks the function for its arguments;
// for a stitchable function Metal asks [device compiler], which is nil on this device, so the argument list stays empty and CIKernelReflection's consolidate()
// writes into a NULL argument array (EXC_BAD_ACCESS in Preview, Tools > Adjust Color).  CI_USE_MTL_DAG_FOR_CIKL_SRC=0 makes Core Image take its own
// kernel-language parser instead.  The variable is set ONLY when it is unset, so an operator's explicit value (the ciprobe cikl control case sets 1) always wins.
#ifndef RDNA4_CIENV_H
#define RDNA4_CIENV_H
#include <stddef.h>
#define RDNA4CE_VAR   "CI_USE_MTL_DAG_FOR_CIKL_SRC"
#define RDNA4CE_VALUE "0"
enum { RDNA4CE_ALREADY_SET = 0, RDNA4CE_SET = 1, RDNA4CE_SET_FAILED = -1 };
typedef const char *(*rdna4ce_getenv_fn)(const char *name);
typedef int (*rdna4ce_setenv_fn)(const char *name, const char *value, int overwrite);
// Returns RDNA4CE_SET when it set the variable to "0", RDNA4CE_ALREADY_SET when a value (any value, even the empty string) was present and was left alone.
static inline int rdna4ce_apply(rdna4ce_getenv_fn get, rdna4ce_setenv_fn set) {
    if (get(RDNA4CE_VAR) != NULL) return RDNA4CE_ALREADY_SET;
    return set(RDNA4CE_VAR, RDNA4CE_VALUE, 0) == 0 ? RDNA4CE_SET : RDNA4CE_SET_FAILED;
}
#endif
