#pragma once
#include <pthread.h>
#include <new>

struct IOLock { pthread_mutex_t mutex; };
static inline IOLock *IOLockAlloc() {
    auto *lock = new (std::nothrow) IOLock;
    if (lock && pthread_mutex_init(&lock->mutex, nullptr) != 0) {
        delete lock;
        return nullptr;
    }
    return lock;
}
static inline void IOLockFree(IOLock *lock) {
    pthread_mutex_destroy(&lock->mutex);
    delete lock;
}
static inline void IOLockLock(IOLock *lock) { pthread_mutex_lock(&lock->mutex); }
static inline void IOLockUnlock(IOLock *lock) { pthread_mutex_unlock(&lock->mutex); }
