#pragma once

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(condition) assert(condition)
#define BREAK_TO_DEBUGGER() abort()
#define dm_output_to_console(...) fprintf(stderr, __VA_ARGS__)
#define __counted_by(member)
#define noinline_for_stack __attribute__((noinline))
#define READ_ONCE(value) (*(const volatile __typeof__(value) *)&(value))
#define swap(lhs, rhs) do { __typeof__(lhs) saved = (lhs); (lhs) = (rhs); (rhs) = saved; } while (0)
