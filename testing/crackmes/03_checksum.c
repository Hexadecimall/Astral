/* Crackme 03: length and a sum of characters. */
#include <stdio.h>
#include <string.h>
int check(const char *key) {
    if (strlen(key) != 6) return 0;
    int sum = 0;
    for (int i = 0; i < 6; ++i) sum += key[i];
    return sum == 597;
}

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
