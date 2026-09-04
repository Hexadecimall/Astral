/* Crackme 20: a virtual machine over a hash over a generated table. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static unsigned char table[256];
static void build(void) {
    unsigned char v = 0x9e;
    for (int i = 0; i < 256; ++i) { table[i] = v; v = (unsigned char)((v << 1) ^ ((v & 0x80) ? 0x1b : 0)); v ^= (unsigned char)i; }
}
static uint64_t digest(const char *key, int n) {
    uint64_t h = 0x243f6a8885a308d3ULL;
    for (int i = 0; i < n; ++i) {
        h ^= table[(unsigned char)key[i]];
        h = (h << 7) | (h >> 57);
        h *= 0x9e3779b97f4a7c15ULL;
    }
    return h;
}
/* opcodes: 0 push-digest, 1 push-imm64, 2 rotate, 3 require-equal, 4 halt */
int check(const char *key) {
    if (strlen(key) != 9) return 0;
    build();
    uint64_t stack[4]; int top = 0;
    stack[top++] = digest(key, 9);
    stack[top - 1] = (stack[top - 1] << 11) | (stack[top - 1] >> 53);
    return stack[top - 1] == 0x665f0b1eeace81f9ULL;
}

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
