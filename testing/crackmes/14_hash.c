/* Crackme 14: a hash of the key compared with a constant. */
#include <stdio.h>
#include <stdint.h>
static uint64_t fnv(const char *text) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; text[i]; ++i) { h ^= (unsigned char)text[i]; h *= 0x100000001b3ULL; }
    return h;
}
int check(const char *key) { return fnv(key) == 0xd272abd690a81985ULL; }

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
