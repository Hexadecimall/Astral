/*
 * Runtime support for decompiled C emitted by Astral.
 *
 * The decompiler works in terms of sized machine types and a handful of
 * operations that have no C spelling: extract these bytes, glue these two
 * values together, did this addition carry. This header gives every one of them
 * a real definition, so the emitted source is ordinary C that a compiler
 * accepts and a linker links.
 *
 * The emitter can inline a copy of this file into its output instead, which is
 * what it does by default; include it directly when compiling many decompiled
 * units together.
 */
#ifndef ASTRAL_DECOMPILED_H
#define ASTRAL_DECOMPILED_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#  define ASTRAL_INLINE static inline __attribute__((always_inline, unused))
#  define ASTRAL_NORETURN __attribute__((noreturn))
#else
#  define ASTRAL_INLINE static inline
#  define ASTRAL_NORETURN
#endif

/* --------------------------------------------------------------- machine types
 *
 * The decompiler names integer types by byte width. `undefined` widths are
 * values whose signedness was never established; they behave as unsigned.
 */
typedef int8_t   int1;
typedef int16_t  int2;
typedef int32_t  int4;
typedef int64_t  int8;
typedef uint8_t  uint1;
typedef uint16_t uint2;
typedef uint32_t uint4;
typedef uint64_t uint8;

typedef uint8_t  byte;
typedef uint16_t word;
typedef uint32_t dword;
typedef uint64_t qword;

typedef uint8_t  undefined;
typedef uint8_t  undefined1;
typedef uint16_t undefined2;
typedef uint32_t undefined3;
typedef uint32_t undefined4;
typedef uint64_t undefined5;
typedef uint64_t undefined6;
typedef uint64_t undefined7;
typedef uint64_t undefined8;

typedef uint8_t  xunknown1;
typedef uint16_t xunknown2;
typedef uint32_t xunknown4;
typedef uint64_t xunknown8;

typedef float    float4;
typedef double   float8;
typedef long double float10;
typedef long double float16;

typedef int16_t  wchar2;
typedef int32_t  wchar4;

/* An address holding code. Calls through it are cast at the call site. */
typedef void code;

/* ------------------------------------------------------------------ p-code ops
 *
 * Names carry their operand widths in bytes: CONCAT44 glues two 4-byte values
 * into an 8-byte one, SUB84 takes 4 bytes out of an 8-byte value, ZEXT48 widens
 * 4 bytes to 8. The emitter writes out only the width combinations a given
 * function actually uses; the fixed-arity operations below are always present.
 */

/* Execution reached an instruction the disassembler could not decode. */
ASTRAL_NORETURN ASTRAL_INLINE void halt_baddata(void)
{
    for (;;) {
    }
}

/* A software interrupt, whose effect depends on the target. */
ASTRAL_INLINE void swi(int number) { (void)number; }

/* NAN is a macro in math.h; the decompiler means the predicate. */
#ifdef NAN
#  undef NAN
#endif
ASTRAL_INLINE int NAN(double value) { return value != value; }

ASTRAL_INLINE double ABS(double value) { return value < 0 ? -value : value; }

ASTRAL_INLINE double SQRT(double value)
{
    /* Newton's method, so this header stays free of a libm dependency. */
    if (value <= 0)
        return 0;
    double guess = value;
    for (int i = 0; i < 64; ++i)
        guess = 0.5 * (guess + value / guess);
    return guess;
}

ASTRAL_INLINE double CEIL(double value)
{
    double truncated = (double)(int64_t)value;
    return truncated < value ? truncated + 1 : truncated;
}

ASTRAL_INLINE double FLOOR(double value)
{
    double truncated = (double)(int64_t)value;
    return truncated > value ? truncated - 1 : truncated;
}

ASTRAL_INLINE double ROUND(double value)
{
    return value < 0 ? CEIL(value - 0.5) : FLOOR(value + 0.5);
}

ASTRAL_INLINE double TRUNC(double value) { return (double)(int64_t)value; }

ASTRAL_INLINE double INT2FLOAT(int64_t value) { return (double)value; }

ASTRAL_INLINE double FLOAT2FLOAT(double value) { return value; }

ASTRAL_INLINE int POPCOUNT(uint64_t value)
{
    int count = 0;
    while (value != 0) {
        count += (int)(value & 1u);
        value >>= 1;
    }
    return count;
}

ASTRAL_INLINE int LZCOUNT(uint64_t value)
{
    int count = 0;
    for (int bit = 63; bit >= 0; --bit) {
        if ((value >> bit) & 1u)
            break;
        ++count;
    }
    return count;
}

/* Writing part of a variable.
 *
 * The decompiler writes `value._0_8_ = x`, meaning "put the bytes of x at offset
 * zero". A plain assignment through an integer lvalue would convert the number
 * instead of copying its bytes, which quietly changes a float into the integer
 * nearest to it, so the bytes are copied directly. A value narrower than the
 * slot is zero-filled; a wider one is truncated, matching how the decompiler
 * models the little-endian machines this comes up on.
 */
#define ASTRAL_STORE(variable, offset, width, value)                                   \
    do {                                                                               \
        __typeof__(value) astral_value_ = (value);                                     \
        unsigned char astral_bytes_[(width) > sizeof(astral_value_) ? (width)          \
                                                                   : sizeof(astral_value_)]; \
        memset(astral_bytes_, 0, sizeof astral_bytes_);                                \
        memcpy(astral_bytes_, &astral_value_, sizeof astral_value_);                   \
        memcpy((unsigned char *)&(variable) + (offset), astral_bytes_, (width));       \
    } while (0)

/* Bit insertion and the shift-with-carry pair, as p-code defines them. */
ASTRAL_INLINE uint64_t INSERT(uint64_t into, uint64_t value, int position, int width)
{
    uint64_t mask = width >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << width) - 1);
    return (into & ~(mask << position)) | ((value & mask) << position);
}

ASTRAL_INLINE uint64_t ZPULL(uint64_t value, int shift)
{
    return shift >= 64 ? 0 : value >> shift;
}

ASTRAL_INLINE int64_t SPULL(int64_t value, int shift)
{
    return shift >= 64 ? (value < 0 ? -1 : 0) : value >> shift;
}

#endif /* ASTRAL_DECOMPILED_H */
