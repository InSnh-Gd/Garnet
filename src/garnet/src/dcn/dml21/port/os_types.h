#ifndef GARNET_DML21_OS_TYPES_H
#define GARNET_DML21_OS_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define noinline_for_stack __attribute__((__noinline__))

#ifndef __cplusplus
#define swap(left, right) do { __typeof__(left) value = (left); (left) = (right); (right) = value; } while (0)
#endif

#if defined(KERNEL)
#ifndef GARNET_DML21_COMPILE_ONLY
#error "DML2.1 has no supported kernel FPU owner; these objects cannot run in the kext"
#endif
#ifndef INT_MIN
#define INT_MIN (-__INT_MAX__ - 1)
#endif
#define ASSERT(condition) ((void)(condition))
#define dm_output_to_console(...) ((void)0)
#else
#include <assert.h>
#include <limits.h>
#define ASSERT(condition) assert(condition)
#define dm_output_to_console(...) ((void)0)
#endif

#endif
