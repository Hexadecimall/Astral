/* Crackme 17: a system of equations over the characters. */
#include <stdio.h>
#include <string.h>
int check(const char *key) {
    if (strlen(key) != 6) return 0;
    int a = key[0], b = key[1], c = key[2], d = key[3], e = key[4], f = key[5];
    if (a + b + c != 351) return 0;
    if (d + e + f != 326) return 0;
    if (a * 2 - b != 109) return 0;
    if (c - b != -6) return 0;
    if ((d ^ e) != 0x11) return 0;
    return f - a == -6;
}
int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
