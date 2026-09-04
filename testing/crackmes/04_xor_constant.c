/* Crackme 04: exclusive-or with one byte. */
#include <stdio.h>
#include <string.h>
int check(const char *key) {
    static const unsigned char want[] = {0x22,0x29,0x2e,0x39,0x20,0x2d};
    if (strlen(key) != 6) return 0;
    for (int i = 0; i < 6; ++i) if ((unsigned char)(key[i] ^ 0x4c) != want[i]) return 0;
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
