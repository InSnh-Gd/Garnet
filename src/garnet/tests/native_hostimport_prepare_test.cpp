#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using IOReturn = uint32_t;
using mach_vm_address_t = uint64_t;
using mach_vm_size_t = uint64_t;
using vm_size_t = uint64_t;
enum IODirection { kIODirectionIn = 1, kIODirectionOut = 2, kIODirectionInOut = 3 };
constexpr IOReturn kIOReturnSuccess = 0, KERN_PROTECTION_FAILURE = 2;
constexpr IOReturn kIOReturnNoMemory = 0xe00002bd, kIOReturnBadArgument = 0xe00002c2;
template<class... Args> static void testLog(const char *, Args...) {}
#define N1C_LOG(...) testLog(__VA_ARGS__)

struct Task {
    bool readable = true, writable = true;
    bool failPageList = false;
    unsigned creates = 0, releases = 0, prepares = 0, pins = 0, completes = 0;
    unsigned allocations = 0, frees = 0, rollbacks = 0;
    unsigned nullAt = 0;
    unsigned errorAt = 0;
    IOReturn forcedError = 0;
    std::vector<char> events;
};
struct IOMemoryDescriptor {
    Task *task;
    IODirection direction;
    bool wired = false;
    static IOMemoryDescriptor *withAddressRange(uint64_t, uint64_t, IODirection direction, Task *task) {
        if (++task->creates == task->nullAt) return nullptr;
        return new IOMemoryDescriptor{task, direction, false};
    }
    IOReturn prepare() {
        task->prepares++;
        if (task->forcedError && (task->errorAt == 0 || task->prepares == task->errorAt)) return task->forcedError;
        // XNU IOMemoryDescriptor::wireVirtual uses UPL_COPYOUT_FROM only for Out.
        if (!task->readable || (!task->writable && direction != kIODirectionOut)) return KERN_PROTECTION_FAILURE;
        wired = true;
        task->pins++;
        return 0;
    }
    void complete() { task->events.push_back('C'); if (wired) task->completes++; wired = false; }
    void release() { task->events.push_back('L'); task->releases++; delete this; }
};

static Task *gCurrentTask;
static void *IOMalloc(vm_size_t size) {
    gCurrentTask->events.push_back('A');
    gCurrentTask->allocations++;
    return gCurrentTask->failPageList ? nullptr : std::malloc((size_t)size);
}
static void IOFree(void *p, vm_size_t) {
    gCurrentTask->events.push_back('F');
    gCurrentTask->frees++;
    std::free(p);
}

static IOReturn productionPrepare(Task *task, bool *readOnly) {
    gCurrentTask = task;
    const uint64_t hostVa = 0x100000, size = 0x4000;
    (void)hostVa; (void)size;
    const struct { uint64_t pages; } pre { size / 0x1000 };
    bool reserved = true;   // the system-budget token belongs to the enclosing import call
    const auto sys_pending_rollback = [&](bool &armed, uint64_t) {
        if (!armed) return;
        armed = false;
        task->events.push_back('P');
        task->rollbacks++;
    };
    #include "host_import_prepare.inc"
    #ifdef RDNA4_TEST_HAS_READONLY_IMPORT
    *readOnly = hro;
    #else
    *readOnly = false;
    #endif
    IOFree(pages, (vm_size_t)(pre.pages * sizeof(uint64_t)));
    md->complete();
    md->release();
    return 0;
}

int main() {
    unsigned failed = 0;
    auto rollback_is_ordered = [](const Task &task) {
        const auto p = std::find(task.events.begin(), task.events.end(), 'P');
        if (p == task.events.end()) return true;
        if (p + 1 != task.events.end()) return false;
        if (task.pins == 0) return std::find(task.events.begin(), p, 'C') == p;
        const auto l = std::find(task.events.begin(), p, 'L');
        if (l == p) return false;
        const auto c = std::find(task.events.begin(), l, 'C');
        return c != l;
    };
    auto run = [&](const char *name, Task task, IOReturn wantRc, bool wantReadOnly,
                   unsigned creates, unsigned prepares, unsigned completes, unsigned rollbacks,
                   unsigned allocations, unsigned frees) {
        bool readOnly = false;
        IOReturn rc = productionPrepare(&task, &readOnly);
        bool ok = rc == wantRc && readOnly == wantReadOnly && task.creates == creates &&
            task.prepares == prepares && task.releases == creates - (task.nullAt ? 1u : 0u) && task.completes == completes &&
            task.rollbacks == rollbacks && task.allocations == allocations && task.frees == frees && rollback_is_ordered(task);
        std::printf("%s %s rc=%#x readonly=%d descriptors=%u prepares=%u pins=%u completes=%u releases=%u rollbacks=%u allocs=%u frees=%u\n",
            ok ? "PASS" : "FAIL", name, rc, readOnly, task.creates, task.prepares, task.pins, task.completes,
            task.releases, task.rollbacks, task.allocations, task.frees);
        failed += !ok;
    };
    run("writable", Task{}, 0, false, 1, 1, 1, 0, 1, 1);
    Task ro; ro.writable = false;
    run("read-only CA backing", ro, 0, true, 2, 2, 1, 0, 1, 1);
    Task unmapped; unmapped.readable = false; unmapped.writable = false;
    run("unreadable", unmapped, kIOReturnBadArgument, false, 2, 2, 0, 1, 0, 0);
    Task allocation; allocation.nullAt = 1;
    run("descriptor allocation failure", allocation, kIOReturnNoMemory, false, 1, 0, 0, 1, 0, 0);
    ro.nullAt = 2;
    run("readonly descriptor allocation failure", ro, kIOReturnNoMemory, false, 2, 1, 0, 1, 0, 0);
    Task failure; failure.forcedError = kIOReturnNoMemory;
    run("non-protection prepare failure", failure, kIOReturnBadArgument, false, 1, 1, 0, 1, 0, 0);
    Task roFailure; roFailure.writable = false; roFailure.forcedError = kIOReturnNoMemory; roFailure.errorAt = 2;
    run("read-only prepare fallback failure releases before rollback", roFailure, kIOReturnBadArgument, false, 2, 2, 0, 1, 0, 0);
    Task pageList; pageList.failPageList = true;
    run("page-list allocation failure unpins before rollback", pageList, kIOReturnNoMemory, false, 1, 1, 1, 1, 1, 0);
    return failed ? 1 : 0;
}
