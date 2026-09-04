/* Crackme 07: each character checked by its own arithmetic. */
#include <stdio.h>
#include <string.h>
int check(const char *key) {
    if (strlen(key) != 6) return 0;
    if (key[0] * 2 != 224) return 0;
    if (key[1] + 5 != 109) return 0;
    if ((key[2] ^ 0x20) != 0x4f) return 0;
    if (key[3] - key[0] != 4) return 0;
    if (key[4] % 7 != 6) return 0;
    return key[5] - key[4] == -1;
}
int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
