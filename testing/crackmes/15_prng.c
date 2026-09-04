/* Crackme 15: compared against a generated sequence. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
int check(const char *key) {
    if (strlen(key) != 6) return 0;
    uint32_t state = 0x2545f491u;
    for (int i = 0; i < 6; ++i) {
        state = state * 1103515245u + 12345u;
        if ((unsigned char)key[i] != (unsigned char)(((state >> 16) & 0x1f) + 'a' + (i * 3 % 7)))
            return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
