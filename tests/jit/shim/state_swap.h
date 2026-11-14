// Included ahead of every core file by game.sh, so the Mac can load save states from the big-endian Wii U: while
// stateSwap is set, each value fread fills is byte-reversed by its type, which the pointer passed to fread gives
#pragma once
#ifdef __cplusplus
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <type_traits>
#include <typeinfo>

extern bool stateSwap;

template <typename T> inline void swapBytes(T &value) {
    uint8_t *b = (uint8_t*)&value;
    for (size_t i = 0; i < sizeof(T) / 2; i++) {
        uint8_t t = b[i];
        b[i] = b[sizeof(T) - 1 - i];
        b[sizeof(T) - 1 - i] = t;
    }
}

// Structures are swapped field by field, from a list of field sizes by type name in game.cpp; each field sits at the
// next multiple of its size, as both compilers lay them out. A structure with no list stops the run, naming it
const char *stateLayout(const char *type);

inline void swapLayout(uint8_t *data, const char *type, size_t size) {
    const char *layout = stateLayout(type);
    if (!layout) {
        fprintf(stderr, "state_swap.h: no layout for %s (%zu bytes)\n", type, size);
        exit(1);
    }
    size_t at = 0;
    for (const char *c = layout; *c;) {
        // Fields as size or size*count, separated by spaces
        size_t field = strtoul(c, (char**)&c, 10), count = 1;
        if (*c == '*') count = strtoul(c + 1, (char**)&c, 10);
        while (*c == ' ') c++;
        for (size_t i = 0; i < count; i++) {
            at = (at + field - 1) / field * field;
            for (size_t j = 0; j < field / 2; j++) {
                uint8_t t = data[at + j];
                data[at + j] = data[at + field - 1 - j];
                data[at + field - 1 - j] = t;
            }
            at += field;
        }
    }
    if ((at + 3) / 4 * 4 != (size + 3) / 4 * 4 && (at + 7) / 8 * 8 != size) {
        fprintf(stderr, "state_swap.h: layout for %s covers %zu of %zu bytes\n", type, at, size);
        exit(1);
    }
}

template <typename T> inline void swapValue(T &value) {
    if constexpr (std::is_array_v<T>) {
        for (auto &v: value) swapValue(v);
    }
    else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) {
        swapBytes(value);
    }
    else {
        swapLayout((uint8_t*)&value, typeid(T).name(), sizeof(T));
    }
}

template <typename P> inline size_t stateRead(P *ptr, size_t size, size_t count, FILE *file) {
    size_t got = fread((void*)ptr, size, count, file);
    if constexpr (!std::is_void_v<P>) {
        using T = std::remove_cv_t<P>;
        if (stateSwap && sizeof(T) > 1) {
            // Elements of the pointer's type, however the read counted them
            size_t bytes = got * size;
            if (bytes % sizeof(T)) {
                fprintf(stderr, "state_swap.h: read of %zu bytes into %s (%zu bytes)\n", bytes, typeid(T).name(),
                    sizeof(T));
                exit(1);
            }
            for (size_t i = 0; i < bytes / sizeof(T); i++) swapValue(((T*)ptr)[i]);
        }
    }
    else if (stateSwap && size > 1) {
        fprintf(stderr, "state_swap.h: untyped read of %zu x %zu\n", size, count);
        exit(1);
    }
    return got;
}

#define fread(ptr, size, count, file) stateRead(ptr, size, count, file)
#endif
