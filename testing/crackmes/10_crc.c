/* Crackme 10: a cyclic redundancy check over the whole key. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static uint32_t crc32(const char *text) {
    uint32_t c = 0xffffffffu;
    for (int i = 0; text[i]; ++i) {
        c ^= (unsigned char)text[i];
        for (int b = 0; b < 8; ++b) c = (c >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}
int check(const char *key) { return strlen(key) == 6 && crc32(key) == 0x1b6e485bu; }

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
