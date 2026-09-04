/* Crackme 11: two independent checks that must both hold. */
#include <stdio.h>
#include <string.h>
static int stage_one(const char *key) { return strlen(key) == 8 && key[7] == '7'; }
static int stage_two(const char *key) {
    int sum = 0, product = 1;
    for (int i = 0; i < 7; ++i) { sum += key[i]; product = (product * (key[i] & 0xf)) & 0xffff; }
    return sum == 708 && product == 540;
}
int check(const char *key) { return stage_one(key) && stage_two(key); }

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
