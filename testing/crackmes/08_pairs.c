/* Crackme 08: constraints that tie characters together. */
#include <stdio.h>
#include <string.h>
int check(const char *key) {
    if (strlen(key) != 6) return 0;
    if (key[0] + key[5] != 'm' + 'r') return 0;
    if (key[1] * key[4] != 'i' * 'o') return 0;
    if (key[2] != key[3]) return 0;
    if (key[2] != 'r') return 0;
    if (key[0] != 'm' || key[1] != 'i') return 0;
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
