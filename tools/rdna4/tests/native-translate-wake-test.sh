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

static double wake_delay(int kq, BOOL more, BOOL *rescan_io, double request_time) {
    struct { int kq; } scan = {kq};
    BOOL rescan = *rescan_io;
    current = 0;
    event_time = request_time;
C
    awk '/BOOL pace = more \|\| rescan;/ { emit = 1 } emit { print } emit && /pauseUntil\(/ { exit }' "$source_file"
    cat <<'C'
    *rescan_io = rescan;
    return current;
}

int main(void) {
    BOOL r_idle = NO, r_early = NO, r_backlog = NO, r_backlog_ev = NO, r_rescan = YES, r_quiet = NO;
    BOOL r_nokq_idle = NO, r_nokq_busy = NO;
    double idle = wake_delay(1, NO, &r_idle, 1.0);
    double early = wake_delay(1, NO, &r_early, 0.125);
    double backlog = wake_delay(1, YES, &r_backlog, 0.125);
    double backlog_ev = wake_delay(1, YES, &r_backlog_ev, 0.0);
    double rescan = wake_delay(1, NO, &r_rescan, 0.125);
    double quiet = wake_delay(1, NO, &r_quiet, 10.0);
    double nokq_idle = wake_delay(-1, NO, &r_nokq_idle, 10.0);
    double nokq_busy = wake_delay(-1, YES, &r_nokq_busy, 10.0);
    printf("next scan in seconds: idle wake=%.3f early wake=%.3f backlog=%.3f backlog_ev=%.3f rescan=%.3f no event=%.3f nokq_idle=%.3f nokq_busy=%.3f\n",
           idle, early, backlog, backlog_ev, rescan, quiet, nokq_idle, nokq_busy);
    if (idle != 1.0 || !r_idle || early != 0.125 || !r_early ||
        backlog != 0.0 || r_backlog || backlog_ev != 0.0 || !r_backlog_ev ||
        rescan != 0.0 || !r_rescan || quiet != 5.0 || r_quiet ||
        nokq_idle != 0.25 || nokq_busy != 0.0) {
        fputs("FAIL daemon wake/backlog pacing did not drain immediately or preserve 5s idle fallback\n", stderr);
        return 1;
    }
    puts("PASS vnode wakes and backlog slices run immediately; idle fallback waits 5s");
    return 0;
}
C
} | "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -x c - -o "$out/test"
"$out/test"
