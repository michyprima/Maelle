// The system under game.cpp when it's built with devkitPPC to run whole in Unicorn, see host.cpp: the startup code,
// and newlib's system calls and devices, which ask the host for files, the time and exiting
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <poll.h>
#include "layout.h"

// Calls to the host, which watches the instruction at each one's address, does the work and returns through the blr there
template <typename... Args> static int32_t host(uint32_t call, Args... args) {
    return ((int32_t(*)(Args...))(uintptr_t)(GUEST_HOST_CALLS + call * 4))(args...);
}

extern "C" {
int main(int argc, char **argv);
void __init();
void __libc_init_array();
extern char _end[];

// Set the stack, the small data bases and the heap, then run the constructors and main; the host gives argc and argv
__asm__(
    ".section .text._start\n"
    ".global _start\n"
    "_start:\n"
    "    lis 1, " GUEST_STR(GUEST_STACK_TOP) "@h\n"
    "    ori 1, 1, " GUEST_STR(GUEST_STACK_TOP) "@l\n"
    "    lis 13, _SDA_BASE_@ha\n"
    "    addi 13, 13, _SDA_BASE_@l\n"
    "    lis 2, _SDA2_BASE_@ha\n"
    "    addi 2, 2, _SDA2_BASE_@l\n"
    "    b guestStart\n"
    ".previous\n");

// newlib runs the .init section as _init, which the eabi startup files call __init
__attribute__((used)) void _init() { __init(); }

__attribute__((used)) void guestStart(int argc, char **argv) {
    // Local times as the host has them, which the DS's clock starts from
    static char zone[128];
    host(GUEST_ZONE, zone, sizeof(zone));
    if (zone[0]) {
        setenv("TZ", zone, 1);
        tzset();
    }
    __libc_init_array();
    exit(main(argc, argv));
}

// The heap runs from the end of the program to a fixed limit, which the host maps
void *_sbrk_r(struct _reent *r, ptrdiff_t increment) {
    static uintptr_t top = 0;
    if (!top) top = ((uintptr_t)_end + 0xFFF) & ~0xFFF;
    if (top + increment > GUEST_HEAP_END) {
        r->_errno = ENOMEM;
        return (void*)-1;
    }
    uintptr_t old = top;
    top += increment;
    return (void*)old;
}

void __syscall_exit(int rc) { host(GUEST_EXIT, rc); for (;;); }
void __syscall_abort() { host(GUEST_WRITE, 2, "abort\n", 6); __syscall_exit(134); }
void __syscall_assert_func(const char *file, int line, const char *, const char *expr) {
    char text[256];
    int size = snprintf(text, sizeof(text), "assertion failed: %s at %s:%d\n", expr, file, line);
    host(GUEST_WRITE, 2, text, size);
    __syscall_exit(134);
}

int __syscall_clock_gettime(clockid_t, struct timespec *tp) {
    uint32_t ns[2];
    host(GUEST_TIME, ns);
    uint64_t t = (uint64_t(ns[0]) << 32) | ns[1];
    tp->tv_sec = t / 1000000000;
    tp->tv_nsec = t % 1000000000;
    return 0;
}

int __syscall_gettod_r(struct _reent*, struct timeval *tp, struct timezone*) {
    struct timespec ts;
    __syscall_clock_gettime(CLOCK_REALTIME, &ts);
    tp->tv_sec = ts.tv_sec;
    tp->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

int poll(struct pollfd*, unsigned int, int) { errno = ENOSYS; return -1; }
}

// Threads take turns on the one CPU, switching only when the running one waits, sleeps, joins or ends, always to the
// next one that can run, so a run goes the same way every time. Time doesn't pass while waiting, so a wait with a
// timeout ends right away if nothing else can run
struct __pthread_t {
    uint32_t context[58]; // r1, lr, cr, r14-r31, then f14-f31, as guestSwitch saves them
    void *(*func)(void*);
    void *arg, *result;
    const void *waitOn; // What it waits for: a condition, a thread to end, or nothing
    bool done;
    const void *tls[32];
    __pthread_t *next;
};

static __pthread_t mainThread, *current = &mainThread, *lastThread = &mainThread;
static const void *const SLEEPING = &mainThread.waitOn; // Waiting for nothing, just letting the others run

extern "C" void guestSwitch(uint32_t *save, const uint32_t *load);
__asm__(
    ".section .text.guestSwitch\n"
    ".global guestSwitch\n"
    "guestSwitch:\n"
    "    stw 1, 0(3)\n"
    "    mflr 0\n"
    "    stw 0, 4(3)\n"
    "    mfcr 0\n"
    "    stw 0, 8(3)\n"
    "    stmw 14, 12(3)\n"
    "    stfd 14, 88(3)\n    stfd 15, 96(3)\n    stfd 16, 104(3)\n    stfd 17, 112(3)\n    stfd 18, 120(3)\n"
    "    stfd 19, 128(3)\n    stfd 20, 136(3)\n    stfd 21, 144(3)\n    stfd 22, 152(3)\n    stfd 23, 160(3)\n"
    "    stfd 24, 168(3)\n    stfd 25, 176(3)\n    stfd 26, 184(3)\n    stfd 27, 192(3)\n    stfd 28, 200(3)\n"
    "    stfd 29, 208(3)\n    stfd 30, 216(3)\n    stfd 31, 224(3)\n"
    "    lfd 14, 88(4)\n    lfd 15, 96(4)\n    lfd 16, 104(4)\n    lfd 17, 112(4)\n    lfd 18, 120(4)\n"
    "    lfd 19, 128(4)\n    lfd 20, 136(4)\n    lfd 21, 144(4)\n    lfd 22, 152(4)\n    lfd 23, 160(4)\n"
    "    lfd 24, 168(4)\n    lfd 25, 176(4)\n    lfd 26, 184(4)\n    lfd 27, 192(4)\n    lfd 28, 200(4)\n"
    "    lfd 29, 208(4)\n    lfd 30, 216(4)\n    lfd 31, 224(4)\n"
    "    lmw 14, 12(4)\n"
    "    lwz 0, 8(4)\n"
    "    mtcr 0\n"
    "    lwz 0, 4(4)\n"
    "    mtlr 0\n"
    "    lwz 1, 0(4)\n"
    "    blr\n"
    ".previous\n");

static void fail(const char *text) {
    host(GUEST_WRITE, 2, text, strlen(text));
    __syscall_exit(3);
}

// Run the next thread that isn't waiting; returns false if there's none and this one waits with a timeout, which then
// ends, and stops the run if it waits without one
static bool schedule(bool timeout) {
    for (__pthread_t *t = current->next ? current->next : &mainThread; ; t = t->next ? t->next : &mainThread) {
        if (t == current) break;
        if (t->done || (t->waitOn && t->waitOn != SLEEPING)) continue;
        if (t->waitOn == SLEEPING) t->waitOn = nullptr;
        __pthread_t *from = current;
        current = t;
        guestSwitch(from->context, t->context);
        return true;
    }
    if (current->waitOn == SLEEPING || timeout) {
        current->waitOn = nullptr;
        return false;
    }
    fail("every thread is waiting\n");
    return false;
}

static void wake(const void *what) {
    for (__pthread_t *t = &mainThread; t; t = t->next)
        if (t->waitOn == what) t->waitOn = nullptr;
}

extern "C" {
static void threadStart() {
    current->result = current->func(current->arg);
    current->done = true;
    wake(current);
    schedule(false);
}

int __syscall_thread_create(struct __pthread_t **thread, void *(*func)(void*), void *arg, void *stack, size_t stackSize) {
    __pthread_t *t = new __pthread_t();
    if (!stackSize) stackSize = 0x40000;
    if (!stack) stack = malloc(stackSize);
    t->func = func;
    t->arg = arg;
    uint32_t top = ((uintptr_t)stack + stackSize - 64) & ~15;
    *(uint32_t*)top = 0; // The end of the back chain
    t->context[0] = top;
    t->context[1] = (uintptr_t)threadStart;
    lastThread->next = t;
    lastThread = t;
    *thread = t;
    return 0;
}

void *__syscall_thread_join(struct __pthread_t *thread) {
    while (!thread->done) {
        current->waitOn = thread;
        schedule(false);
    }
    return thread->result;
}

int __syscall_thread_detach(struct __pthread_t*) { return 0; }
void __syscall_thread_exit(void *value) {
    current->result = value;
    current->done = true;
    wake(current);
    schedule(false);
}
struct __pthread_t *__syscall_thread_self() { return current; }

int __syscall_nanosleep(const struct timespec*, struct timespec*) {
    current->waitOn = SLEEPING;
    schedule(false);
    return 0;
}
int sched_yield() { return __syscall_nanosleep(nullptr, nullptr); }

// Thread-local values, by key
static uint32_t tlsKeys;
int __syscall_tls_create(uint32_t *key, void (*)(void*)) {
    if (tlsKeys >= 32) return EAGAIN;
    *key = tlsKeys++;
    return 0;
}
int __syscall_tls_set(uint32_t key, const void *value) { current->tls[key] = value; return 0; }
void *__syscall_tls_get(uint32_t key) { return (void*)current->tls[key]; }
int __syscall_tls_delete(uint32_t) { return 0; }

// A lock holds its owner's address; threads only switch when waiting, so another thread holding it is rare
static void acquire(_LOCK_T *lock) {
    while (*lock && *lock != (uintptr_t)current) {
        current->waitOn = SLEEPING;
        if (!schedule(false)) fail("waited on a lock that no other thread can free\n");
    }
    *lock = (uintptr_t)current;
}

void __syscall_lock_init(_LOCK_T *lock) { *lock = 0; }
void __syscall_lock_acquire(_LOCK_T *lock) { acquire(lock); }
int __syscall_lock_try_acquire(_LOCK_T *lock) {
    if (*lock) return 1;
    *lock = (uintptr_t)current;
    return 0;
}
void __syscall_lock_release(_LOCK_T *lock) { *lock = 0; }
void __syscall_lock_close(_LOCK_T*) {}
void __syscall_lock_init_recursive(_LOCK_RECURSIVE_T *lock) { lock->lock = lock->counter = 0; }
void __syscall_lock_acquire_recursive(_LOCK_RECURSIVE_T *lock) {
    if (lock->lock != (uintptr_t)current) acquire(&lock->lock);
    lock->counter++;
}
int __syscall_lock_try_acquire_recursive(_LOCK_RECURSIVE_T *lock) {
    if (lock->lock && lock->lock != (uintptr_t)current) return 1;
    lock->lock = (uintptr_t)current;
    lock->counter++;
    return 0;
}
void __syscall_lock_release_recursive(_LOCK_RECURSIVE_T *lock) { if (!--lock->counter) lock->lock = 0; }
void __syscall_lock_close_recursive(_LOCK_RECURSIVE_T*) {}

int __syscall_cond_signal(_COND_T *cond) {
    for (__pthread_t *t = &mainThread; t; t = t->next) {
        if (t->waitOn == cond) {
            t->waitOn = nullptr;
            break;
        }
    }
    return 0;
}
int __syscall_cond_broadcast(_COND_T *cond) { wake(cond); return 0; }

static int condWait(_COND_T *cond, uint64_t timeout) {
    current->waitOn = cond;
    return schedule(timeout != UINT64_MAX) ? 0 : ETIMEDOUT;
}
int __syscall_cond_wait(_COND_T *cond, _LOCK_T *lock, uint64_t timeout) {
    __syscall_lock_release(lock);
    int result = condWait(cond, timeout);
    acquire(lock);
    return result;
}
int __syscall_cond_wait_recursive(_COND_T *cond, _LOCK_RECURSIVE_T *lock, uint64_t timeout) {
    uint32_t counter = lock->counter;
    lock->counter = 0;
    lock->lock = 0;
    int result = condWait(cond, timeout);
    acquire(&lock->lock);
    lock->counter = counter;
    return result;
}
}

// Files are the host's, by its own descriptors; the console's output goes to the host's too
static int fileOpen(struct _reent *r, void *file, const char *path, int flags, int) {
    int mode = flags & O_ACCMODE, host_flags = (mode == O_WRONLY) ? GUEST_O_WRONLY : (mode == O_RDWR) ? GUEST_O_RDWR : 0;
    if (flags & O_CREAT) host_flags |= GUEST_O_CREAT;
    if (flags & O_TRUNC) host_flags |= GUEST_O_TRUNC;
    if (flags & O_APPEND) host_flags |= GUEST_O_APPEND;
    if (flags & O_EXCL) host_flags |= GUEST_O_EXCL;
    int fd = host(GUEST_OPEN, path, host_flags);
    if (fd < 0) {
        r->_errno = ENOENT;
        return -1;
    }
    *(int*)file = fd;
    return 0;
}

static int fileClose(struct _reent*, void *file) { return host(GUEST_CLOSE, *(int*)file); }

static ssize_t fileWrite(struct _reent *r, void *file, const char *ptr, size_t len) {
    int32_t done = host(GUEST_WRITE, *(int*)file, ptr, len);
    if (done < 0) r->_errno = EIO;
    return done;
}

static ssize_t fileRead(struct _reent *r, void *file, char *ptr, size_t len) {
    int32_t done = host(GUEST_READ, *(int*)file, ptr, len);
    if (done < 0) r->_errno = EIO;
    return done;
}

static off_t fileSeek(struct _reent *r, void *file, off_t pos, int dir) {
    int32_t at = host(GUEST_SEEK, *(int*)file, int32_t(pos), dir);
    if (at < 0) r->_errno = EINVAL;
    return at;
}

static int fileStat(struct _reent*, void *file, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_size = host(GUEST_SIZE, *(int*)file);
    st->st_mode = S_IFREG | 0666;
    return 0;
}

static int pathStat(struct _reent *r, const char *path, struct stat *st) {
    memset(st, 0, sizeof(*st));
    int32_t size = host(GUEST_STAT, path);
    if (size == -1) {
        r->_errno = ENOENT;
        return -1;
    }
    st->st_mode = (size == -2) ? (S_IFDIR | 0777) : (S_IFREG | 0666);
    st->st_size = (size == -2) ? 0 : size;
    return 0;
}

static int pathUnlink(struct _reent *r, const char *path) {
    if (host(GUEST_UNLINK, path) == 0) return 0;
    r->_errno = ENOENT;
    return -1;
}

static int pathRename(struct _reent *r, const char *from, const char *to) {
    if (host(GUEST_RENAME, from, to) == 0) return 0;
    r->_errno = ENOENT;
    return -1;
}

static int pathMkdir(struct _reent *r, const char *path, int) {
    if (host(GUEST_MKDIR, path) == 0) return 0;
    r->_errno = EEXIST;
    return -1;
}

static int fileTruncate(struct _reent *r, void *file, off_t len) {
    if (host(GUEST_TRUNCATE, *(int*)file, int32_t(len)) == 0) return 0;
    r->_errno = EIO;
    return -1;
}

static int fileSync(struct _reent*, void*) { return 0; }

static devoptab_t hostFiles = []() {
    devoptab_t tab = {};
    tab.name = "host";
    tab.structSize = sizeof(int);
    tab.open_r = fileOpen;
    tab.close_r = fileClose;
    tab.write_r = fileWrite;
    tab.read_r = fileRead;
    tab.seek_r = fileSeek;
    tab.fstat_r = fileStat;
    tab.stat_r = pathStat;
    tab.unlink_r = pathUnlink;
    tab.rename_r = pathRename;
    tab.mkdir_r = pathMkdir;
    tab.ftruncate_r = fileTruncate;
    tab.fsync_r = fileSync;
    return tab;
}();

static ssize_t outWrite(struct _reent*, void *, const char *ptr, size_t len) { return host(GUEST_WRITE, 1, ptr, len); }
static ssize_t errWrite(struct _reent*, void *, const char *ptr, size_t len) { return host(GUEST_WRITE, 2, ptr, len); }
static devoptab_t hostOut = { "stdout", 0, nullptr, nullptr, outWrite };
static devoptab_t hostErr = { "stderr", 0, nullptr, nullptr, errWrite };

// Before the other constructors, which may open files
__attribute__((constructor(101))) static void setupDevices() {
    devoptab_list[STD_OUT] = &hostOut;
    devoptab_list[STD_ERR] = &hostErr;
    int device = AddDevice(&hostFiles);
    if (device >= 0) setDefaultDevice(device);
}
