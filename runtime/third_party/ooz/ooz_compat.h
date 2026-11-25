// What kraken.cpp (ooz, GPL-3.0-or-later) took from "stdafx.h" and the Windows headers, for the compilers the
// runtime uses: llvm-mingw (Windows builds, <intrin.h> has the MSVC intrinsics) and clang/gcc on Linux, where the
// intrinsics kraken.cpp calls by their MSVC names are defined here with builtins. Written for this project; ooz's own
// stdafx.h is not used.
#pragma once

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char byte;
typedef unsigned char uint8;
typedef unsigned int uint32;
typedef uint64_t uint64;
typedef int64_t int64;
typedef signed int int32;
typedef unsigned short uint16;
typedef signed short int16;
typedef unsigned int uint;

#if defined(_WIN32)
#include <intrin.h>
#else
#include <emmintrin.h>

// MSVC's unsigned long is 32 bits, Linux's is 64: the mask stays 32 bits, the index is stored as unsigned long
static inline unsigned char _BitScanReverse(unsigned long *index, uint32_t mask) {
    if (!mask) return 0;
    *index = 31 - __builtin_clz(mask);
    return 1;
}
static inline unsigned char _BitScanForward(unsigned long *index, uint32_t mask) {
    if (!mask) return 0;
    *index = __builtin_ctz(mask);
    return 1;
}
static inline uint32_t _rotl(uint32_t v, int n) { return (v << (n & 31)) | (v >> ((32 - n) & 31)); }
static inline unsigned short _byteswap_ushort(unsigned short v) { return __builtin_bswap16(v); }
#define _byteswap_ulong(v) ((uint32_t)__builtin_bswap32((uint32_t)(v)))
#define _byteswap_uint64(v) ((uint64_t)__builtin_bswap64((uint64_t)(v)))
#define __forceinline inline __attribute__((always_inline))
#endif
