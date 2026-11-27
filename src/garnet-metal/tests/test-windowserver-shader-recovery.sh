#!/bin/bash
set -euo pipefail
source_dir=$(cd "$(dirname "$0")/.." && pwd)
runtime="$source_dir/garnet_shaders.m"
out=$(mktemp -d "${TMPDIR:-/tmp}/windowserver-shader-recovery.XXXXXXXX")
trap 'rm -rf "$out"' EXIT
{
    cat <<'C'
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "garnet_gate.h"
#include "garnet_hotswap.h"

typedef bool BOOL;
#define GARNET_INTERNAL static
static BOOL isWindowServer = true;
static inline BOOL rdna4_is_ws(void) { return isWindowServer; }
C
    awk '/^static struct \{/ { buffered = ""; inside = 1 } inside { buffered = buffered $0 "\n" }
         /^} RDNA4SYNC = / { printf "%s", buffered; emit = 1; next }
         emit && /^static void rdna4x_translation_error\(/ { exit } emit { print }' "$runtime"
    cat <<'C'
#define RDNA4LOG(...) ((void)0)
static int dropped;
static void rdna4hs_drop(void *entry) { (void)entry; dropped++; }
static int64_t rdna4hs_wall_ns(void) { return 1; }

static BOOL rebuild(BOOL ok) {
    struct { rdna4hs_entry st; } entry = { { .st = RDNA4HS_BUILDING, .attempts = 1 } }, *e = &entry;
C
    awk '/if \(rdna4hs_build_done\(&e->st, ok,/ { emit = 1 }
         emit && /else if \(e->st.st == RDNA4HS_GAVEUP\)/ { exit }
         emit { print }' "$runtime"
    cat <<'C'
    return e->st.st == RDNA4HS_SWAPPED;
}

static void require(BOOL condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

static void timeout(void) {
    require(rdna4_sync_begin(false), "unexpected refusal before the third consecutive timeout");
    require(!rdna4_sync_begin(false), "WindowServer lost exclusive wait admission");
    rdna4_sync_end(false, true);
}

int main(void) {
    timeout(); timeout(); timeout();
    require(!rdna4_sync_begin(false), "three consecutive timeouts did not disable waits");
    require(!rebuild(false) && !rdna4_sync_begin(false), "failed rebuild rearmed waits");
    isWindowServer = false;
    require(rebuild(true) && !rdna4_sync_begin(true), "application wait policy changed");
    isWindowServer = true;
    require(rebuild(true) && rdna4_sync_begin(false), "successful WindowServer hot swap did not rearm the next cold request");
    require(rebuild(true) && !rdna4_sync_begin(false), "hot swap released another request's exclusive slot");
    rdna4_sync_end(false, true);
    for (unsigned misses = 0; misses < 4; misses++) {
        require(rebuild(true), "real pipeline was not published");
        timeout();
    }
    require(dropped == 7, "watcher did not retire successful entries");
    puts("PASS: failed rebuilds stay throttled; successful WindowServer rebuilds rearm bounded exclusive waits");
}
C
} | "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pthread -I "$source_dir" -x c - -o "$out/test"
"$out/test"
