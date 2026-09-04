/* Stress 14: 64-bit arithmetic, shifts and rotates. */
#include <stdint.h>
static uint64_t rotl(uint64_t v, int by) { return (v << by) | (v >> (64 - by)); }
uint64_t scramble(uint64_t v) {
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33;
    v = rotl(v, 17);
    v *= 0xc4ceb9fe1a85ec53ULL;
    return v ^ (v >> 33);
}
int main(void) { return scramble(1) != 0 ? 0 : 1; }
