/* Crackme 05: exclusive-or with a key that advances. */
#include <stdio.h>
#include <string.h>
int check(const char *key) {
    static const unsigned char want[] = {0x63,0x6d,0x77,0x70,0x70,0x60,0x74};
    if (strlen(key) != 7) return 0;
    unsigned char k = 0;
    for (int i = 0; i < 7; ++i) { if ((unsigned char)(key[i] ^ k) != want[i]) return 0; k += 1; }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
