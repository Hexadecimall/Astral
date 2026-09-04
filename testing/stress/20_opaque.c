/* Stress 20: opaque predicates and a flattened state machine. */
#include <stdint.h>
static int always_true(int x) { return ((x * x) & 1) == (x & 1); }
int flattened(int input) {
    int state = 1, acc = 0, guard = 0;
    while (state != 0 && guard++ < 64) {
        switch (state) {
        case 1: acc = input; state = always_true(input) ? 2 : 7; break;
        case 2: acc ^= 0x5a5a; state = 3; break;
        case 3: acc = (acc << 3) | (acc >> 29); state = always_true(acc) ? 4 : 6; break;
        case 4: acc += 0x1234; state = 5; break;
        case 5: acc &= 0x7fffffff; state = 0; break;
        case 6: acc = ~acc; state = 4; break;
        case 7: acc = 0; state = 0; break;
        default: state = 0; break;
        }
    }
    return acc;
}
int main(void) { return flattened(7) != 0 ? 0 : 1; }
