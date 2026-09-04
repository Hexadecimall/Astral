/*
 * Runtime support for decompiled C emitted by Astral.
 *
 * The decompiler works in terms of sized machine types and a handful of
 * operations that have no C spelling: extract these bytes, glue these two
 * values together, did this addition carry. This header gives every one of them
 * a real definition.
 *
 * Include it when compiling many decompiled units together. Astral does not
 * inline this whole file into its output: it reads the markers below and emits
 * only the pieces a function actually refers to, which for most functions is
 * none of them.
 *
 * Each marker names what the piece after it provides, then the system headers
 * that piece needs, then the other pieces it depends on:
 *
 *     ASTRAL: <names> ; <headers> ; <dependencies>
 */
#ifndef ASTRAL_DECOMPILED_H
#define ASTRAL_DECOMPILED_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ASTRAL: ASTRAL_INLINE ASTRAL_NORETURN ; ; */
#if defined(__GNUC__) || defined(__clang__)
#  define ASTRAL_INLINE static inline __attribute__((always_inline, unused))
#  define ASTRAL_NORETURN __attribute__((noreturn))
#else
#  define ASTRAL_INLINE static inline
#  define ASTRAL_NORETURN
#endif

/* The decompiler names integer types by byte width. An `undefined` width is a
 * value whose signedness was never established; it behaves as unsigned. */

/* ASTRAL: bool true false ; stdbool.h ; */
/* bool is a keyword only from C23; everywhere earlier it is a header. */

/* ASTRAL: int1 int2 int4 int8 ; stdint.h ; */
typedef int8_t   int1;
typedef int16_t  int2;
typedef int32_t  int4;
typedef int64_t  int8;

/* ASTRAL: uint1 uint2 uint4 uint8 ; stdint.h ; */
typedef uint8_t  uint1;
typedef uint16_t uint2;
typedef uint32_t uint4;
typedef uint64_t uint8;

/* ASTRAL: byte word dword qword ; stdint.h ; */
typedef uint8_t  byte;
typedef uint16_t word;
typedef uint32_t dword;
typedef uint64_t qword;

/* ASTRAL: undefined undefined1 undefined2 undefined3 undefined4 undefined5 undefined6 undefined7 undefined8 ; stdint.h ; */
typedef uint8_t  undefined;
typedef uint8_t  undefined1;
typedef uint16_t undefined2;
typedef uint32_t undefined3;
typedef uint32_t undefined4;
typedef uint64_t undefined5;
typedef uint64_t undefined6;
typedef uint64_t undefined7;
typedef uint64_t undefined8;

/* ASTRAL: xunknown1 xunknown2 xunknown4 xunknown8 ; stdint.h ; */
typedef uint8_t  xunknown1;
typedef uint16_t xunknown2;
typedef uint32_t xunknown4;
typedef uint64_t xunknown8;

/* ASTRAL: float4 float8 float10 float16 ; ; */
typedef float    float4;
typedef double   float8;
typedef long double float10;
typedef long double float16;

/* ASTRAL: wchar2 wchar4 ; stdint.h ; */
typedef int16_t  wchar2;
typedef int32_t  wchar4;

/* An address holding code, of a shape the decompiler could not determine. A
 * `code *` is assigned from anything, so it stays an incomplete object type;
 * calls through one are cast at the call site instead. */
/* ASTRAL: code ; ; */
typedef void code;

/* Operations with no C spelling. Names carry their operand widths in bytes:
 * CONCAT44 glues two 4-byte values into an 8-byte one, SUB84 takes 4 bytes out
 * of an 8-byte value, ZEXT48 widens 4 bytes to 8. Astral generates those from
 * the widths a function uses; the fixed ones live here. */

/* Execution reached an instruction the disassembler could not decode. */
/* ASTRAL: halt_baddata ; ; ASTRAL_INLINE */
ASTRAL_NORETURN ASTRAL_INLINE void halt_baddata(void)
{
    for (;;) {
    }
}

/* A software interrupt, whose effect depends on the target. */
/* ASTRAL: swi ; ; ASTRAL_INLINE */
ASTRAL_INLINE void swi(int number) { (void)number; }

/* A trap/breakpoint instruction the disassembler could not attach meaning to.
 * The recovered code sometimes calls through its result, so it returns a null
 * address rather than nothing. */
/* ASTRAL: SoftwareBreakpoint ; ; ASTRAL_INLINE */
ASTRAL_INLINE long long SoftwareBreakpoint() { return 0; }

/* NAN is a macro in math.h; the decompiler means the predicate. */
/* ASTRAL: NAN ; ; ASTRAL_INLINE */
#ifdef NAN
#  undef NAN
#endif
ASTRAL_INLINE int NAN(double value) { return value != value; }

/* ASTRAL: ABS ; ; ASTRAL_INLINE */
ASTRAL_INLINE double ABS(double value) { return value < 0 ? -value : value; }

/* ASTRAL: SQRT ; ; ASTRAL_INLINE */
ASTRAL_INLINE double SQRT(double value)
{
    /* Newton's method, so this stays free of a libm dependency. */
    if (value <= 0)
        return 0;
    double guess = value;
    for (int i = 0; i < 64; ++i)
        guess = 0.5 * (guess + value / guess);
    return guess;
}

/* ASTRAL: CEIL ; stdint.h ; ASTRAL_INLINE */
ASTRAL_INLINE double CEIL(double value)
{
    double truncated = (double)(int64_t)value;
    return truncated < value ? truncated + 1 : truncated;
}

/* ASTRAL: FLOOR ; stdint.h ; ASTRAL_INLINE */
ASTRAL_INLINE double FLOOR(double value)
{
    double truncated = (double)(int64_t)value;
    return truncated > value ? truncated - 1 : truncated;
}

/* ASTRAL: ROUND ; ; ASTRAL_INLINE CEIL FLOOR */
ASTRAL_INLINE double ROUND(double value)
{
    return value < 0 ? CEIL(value - 0.5) : FLOOR(value + 0.5);
}

/* ASTRAL: TRUNC ; stdint.h ; ASTRAL_INLINE */
ASTRAL_INLINE double TRUNC(double value) { return (double)(int64_t)value; }

/* ASTRAL: INT2FLOAT ; stdint.h ; ASTRAL_INLINE */
ASTRAL_INLINE double INT2FLOAT(int64_t value) { return (double)value; }

/* ASTRAL: FLOAT2FLOAT ; ; ASTRAL_INLINE */
ASTRAL_INLINE double FLOAT2FLOAT(double value) { return value; }

/* ASTRAL: POPCOUNT ; stdint.h ; ASTRAL_INLINE */
ASTRAL_INLINE int POPCOUNT(uint64_t value)
{
    int count = 0;
    while (value != 0) {
        count += (int)(value & 1u);
        value >>= 1;
    }
    return count;
}

/* ASTRAL: LZCOUNT ; stdint.h ; ASTRAL_INLINE */
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
 * The decompiler writes `value._0_8_ = x`, meaning "put the bytes of x at
 * offset zero". Assigning through an integer lvalue would convert the number
 * instead of copying its bytes, quietly turning a float into the nearest
 * integer, so the bytes are copied directly. A value narrower than the slot is
 * zero-filled and a wider one truncated, matching how the decompiler models the
 * little-endian machines this comes up on. */
/* ASTRAL: ASTRAL_STORE ; string.h ; */
#define ASTRAL_STORE(variable, offset, width, value)                                          \
    do {                                                                                      \
        __typeof__(value) astral_value_ = (value);                                            \
        unsigned char astral_bytes_[(width) > sizeof(astral_value_) ? (width)                 \
                                                                    : sizeof(astral_value_)]; \
        memset(astral_bytes_, 0, sizeof astral_bytes_);                                       \
        memcpy(astral_bytes_, &astral_value_, sizeof astral_value_);                          \
        memcpy((unsigned char *)&(variable) + (offset), astral_bytes_, (width));              \
    } while (0)

/* ASTRAL: INSERT ; stdint.h ; ASTRAL_INLINE */
ASTRAL_INLINE uint64_t INSERT(uint64_t into, uint64_t value, int position, int width)
{
    uint64_t mask = width >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << width) - 1);
    return (into & ~(mask << position)) | ((value & mask) << position);
}

/* ASTRAL: ZPULL ; stdint.h ; ASTRAL_INLINE */
ASTRAL_INLINE uint64_t ZPULL(uint64_t value, int shift)
{
    return shift >= 64 ? 0 : value >> shift;
}

/* ASTRAL: SPULL ; stdint.h ; ASTRAL_INLINE */
ASTRAL_INLINE int64_t SPULL(int64_t value, int shift)
{
    return shift >= 64 ? (value < 0 ? -1 : 0) : value >> shift;
}

#endif /* ASTRAL_DECOMPILED_H */
