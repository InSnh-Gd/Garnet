#!/bin/bash
set -euo pipefail
test_dir=$(cd "$(dirname "$0")" && pwd)
source_file=${1:-$test_dir/../native/native-translate.m}
out=$(mktemp -d "${TMPDIR:-/tmp}/native-translate-wake.XXXXXXXX")
trap 'rm -rf "$out"' EXIT
{
    cat <<'C'
#include <assert.h>
#include <stdio.h>
#include <time.h>

typedef int BOOL;
enum { NO = 0, YES = 1, EVFILT_VNODE = -4 };
struct kevent { int filter; };

static double current, event_time;

static double now(void) { return current; }

static int kevent(int queue, const void *changes, int change_count, struct kevent *events,
                  int event_count, const struct timespec *timeout) {
    assert(queue == 1 && !changes && change_count == 0 && event_count > 0);
    double deadline = current + timeout->tv_sec + timeout->tv_nsec / 1e9;
    if (event_time > deadline) {
        current = deadline;
        return 0;
    }
    if (event_time > current) current = event_time;
    events[0].filter = EVFILT_VNODE;
    return 1;
}

static void pauseUntil(double deadline) {
    if (deadline > current) current = deadline;
}

static double wake_delay(BOOL more, BOOL rescan, double request_time) {
    struct { int kq; } scan = {1};
    current = 0;
    event_time = request_time;
C
    awk '/BOOL pace = more \|\| rescan;/ { emit = 1 } emit { print } emit && /pauseUntil\(/ { exit }' "$source_file"
    cat <<'C'
    return current;
}

int main(void) {
    double idle = wake_delay(NO, NO, 1.0);
    double early = wake_delay(NO, NO, 0.125);
    double backlog = wake_delay(1, NO, 0.125);
    double rescan = wake_delay(NO, 1, 0.125);
    double quiet = wake_delay(NO, NO, 10.0);
    printf("next scan in seconds: idle wake=%.3f early wake=%.3f backlog=%.3f rescan=%.3f no event=%.3f\n",
           idle, early, backlog, rescan, quiet);
    if (idle != 1.0 || early != 0.25 || backlog != 0.25 || rescan != 0.25 || quiet != 5.25) {
        fputs("FAIL idle request spent part of its own deadline on daemon pacing, or active pacing changed\n", stderr);
        return 1;
    }
    puts("PASS idle vnode wake is immediate after pacing; active scans remain bounded");
    return 0;
}
C
} | "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -x c - -o "$out/test"
"$out/test"
