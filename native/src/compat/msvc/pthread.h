/*
 * xbox-cast: MSVC / UWP portability shim for the vendored UxPlay library (used only when building with MSVC).
 * Copied from src/compat/pthread.h of github.com/vproHacks/screen-mirror-xbox (GPL-3.0), unchanged except this header.
 */
/*
 * Minimal pthread subset for MSVC / UWP, covering what the UxPlay library uses.
 * Mutexes and condition variables are SRW locks / CONDITION_VARIABLEs, whose
 * all-zero state is valid, so zero-initialized pthread_mutex_t objects (as in
 * calloc'ed structs that never call pthread_mutex_init) work.
 */
#ifndef COMPAT_PTHREAD_H
#define COMPAT_PTHREAD_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef HANDLE pthread_t;
typedef struct { SRWLOCK lock; } pthread_mutex_t;
typedef struct { CONDITION_VARIABLE cv; } pthread_cond_t;
typedef void pthread_attr_t;
typedef void pthread_mutexattr_t;
typedef void pthread_condattr_t;

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
typedef int clockid_t;

static __inline int clock_gettime(clockid_t clk, struct timespec *ts) {
    if (clk == CLOCK_MONOTONIC) {
        static LARGE_INTEGER freq;
        LARGE_INTEGER now;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        ts->tv_sec = (time_t) (now.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long) (((now.QuadPart % freq.QuadPart) * 1000000000LL) / freq.QuadPart);
        return 0;
    }
    /* UCRT timespec_get uses GetSystemTimePreciseAsFileTime */
    return timespec_get(ts, TIME_UTC) == TIME_UTC ? 0 : -1;
}

typedef struct { void *(*func)(void *); void *arg; } compat_thread_start_t;

static unsigned __stdcall compat_thread_trampoline(void *p) {
    compat_thread_start_t start = *(compat_thread_start_t *) p;
    free(p);
    start.func(start.arg);
    return 0;
}

static __inline int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                   void *(*func)(void *), void *arg) {
    (void) attr;
    compat_thread_start_t *start = (compat_thread_start_t *) malloc(sizeof(*start));
    if (!start) return ENOMEM;
    start->func = func;
    start->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, compat_thread_trampoline, start, 0, NULL);
    if (!h) {
        free(start);
        return EAGAIN;
    }
    *thread = (HANDLE) h;
    return 0;
}

static __inline int pthread_join(pthread_t thread, void **retval) {
    if (retval) *retval = NULL;
    if (!thread) return EINVAL;
    WaitForSingleObjectEx(thread, INFINITE, FALSE);
    CloseHandle(thread);
    return 0;
}

static __inline int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    (void) a;
    InitializeSRWLock(&m->lock);
    return 0;
}
static __inline int pthread_mutex_lock(pthread_mutex_t *m) { AcquireSRWLockExclusive(&m->lock); return 0; }
static __inline int pthread_mutex_unlock(pthread_mutex_t *m) { ReleaseSRWLockExclusive(&m->lock); return 0; }
static __inline int pthread_mutex_destroy(pthread_mutex_t *m) { (void) m; return 0; }

static __inline int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
    (void) a;
    InitializeConditionVariable(&c->cv);
    return 0;
}
static __inline int pthread_cond_signal(pthread_cond_t *c) { WakeConditionVariable(&c->cv); return 0; }
static __inline int pthread_cond_broadcast(pthread_cond_t *c) { WakeAllConditionVariable(&c->cv); return 0; }
static __inline int pthread_cond_destroy(pthread_cond_t *c) { (void) c; return 0; }
static __inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    SleepConditionVariableSRW(&c->cv, &m->lock, INFINITE, 0);
    return 0;
}
/* abstime is CLOCK_REALTIME, as with the POSIX default */
static __inline int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                                           const struct timespec *abstime) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t ms = ((int64_t) abstime->tv_sec - (int64_t) now.tv_sec) * 1000 +
                 ((int64_t) abstime->tv_nsec - (int64_t) now.tv_nsec) / 1000000;
    if (ms < 0) ms = 0;
    if (!SleepConditionVariableSRW(&c->cv, &m->lock, (DWORD) ms, 0)) {
        return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif
