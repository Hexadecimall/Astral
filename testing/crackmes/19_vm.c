/* Crackme 19: a small virtual machine that validates the key. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
/* opcodes: 0 push-key[n], 1 push-imm, 2 xor, 3 add, 4 require-equal, 5 halt */
static const uint8_t program[] = {
    0,0, 1,0x2a, 2, 1,0x47, 4,
    0,1, 1,0x11, 3, 1,0x72, 4,
    0,6, 1,0x05, 2, 1,0x60, 4,
    5
};
int check(const char *key) {
    if (strlen(key) != 7) return 0;
    uint32_t stack[8]; int top = 0; int pc = 0; int ok = 1;
    while (pc < (int)sizeof program) {
        switch (program[pc++]) {
        case 0: stack[top++] = (unsigned char)key[program[pc++]]; break;
        case 1: stack[top++] = program[pc++]; break;
        case 2: { uint32_t b = stack[--top], a = stack[--top]; stack[top++] = a ^ b; break; }
        case 3: { uint32_t b = stack[--top], a = stack[--top]; stack[top++] = a + b; break; }
        case 4: { uint32_t b = stack[--top], a = stack[--top]; ok = ok && a == b; break; }
        case 5: return ok;
        default: return 0;
        }
    }
    return ok;
}

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
