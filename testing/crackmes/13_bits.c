/* Crackme 13: a bit pattern the key must produce. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
int check(const char *key) {
    if (strlen(key) != 6) return 0;
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits = (bits << 8) | (unsigned char)key[i];
    uint64_t folded = bits ^ (bits >> 13) ^ (bits >> 27);
    return (folded & 0xffffff) == 0x264c5f;
}

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
