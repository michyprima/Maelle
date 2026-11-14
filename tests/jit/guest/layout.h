// Where things are in the guest program run by host.cpp, and the calls it makes to the host, shared by both sides
#pragma once

// The codegen area sits below the program, like the console's, close enough for generated code to call it directly
#define GUEST_CODEGEN 0x00100000
#define GUEST_CODEGEN_SIZE 0x2000000
#define GUEST_HOST_CALLS 0x00010000 // A blr per call, watched by the host
#define GUEST_TEXT 0x02200000 // Where the program is linked
#define GUEST_HEAP_END 0x40000000
#define GUEST_STACK 0x50000000 // 8 MB, with the arguments above it
#define GUEST_STACK_TOP 0x507FFF00
#define GUEST_ARGS 0x50800000

#define GUEST_STR_(x) #x
#define GUEST_STR(x) GUEST_STR_(x)

// Calls, with their arguments in r3 onwards and the result in r3
enum {
    GUEST_EXIT, // code
    GUEST_WRITE, // fd, buffer, size -> bytes written
    GUEST_OPEN, // path, flags -> fd or -1
    GUEST_CLOSE, // fd
    GUEST_READ, // fd, buffer, size -> bytes read
    GUEST_SEEK, // fd, offset, whence -> new offset
    GUEST_SIZE, // fd -> size
    GUEST_STAT, // path -> size, -1 if missing, -2 for a folder
    GUEST_UNLINK, // path
    GUEST_RENAME, // from, to
    GUEST_MKDIR, // path
    GUEST_TRUNCATE, // fd, size
    GUEST_TIME, // two words to fill with nanoseconds, high first
    GUEST_ZONE, // buffer, size -> the host's time zone as a POSIX TZ string, empty if unknown
    GUEST_CALLS
};

// Open flags as the host call takes them
#define GUEST_O_WRONLY 1
#define GUEST_O_RDWR 2
#define GUEST_O_CREAT 4
#define GUEST_O_TRUNC 8
#define GUEST_O_APPEND 16
#define GUEST_O_EXCL 32
