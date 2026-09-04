/* Crackme 06: a substitution table applied per character. */
#include <stdio.h>
#include <string.h>
static unsigned char table[256];
static void build(void) { for (int i = 0; i < 256; ++i) table[i] = (unsigned char)((i * 7 + 13) & 0xff); }
int check(const char *key) {
    static const unsigned char want[] = {0x24,0x40,0xb4,0x32,0xb4,0x2b};
    if (strlen(key) != 6) return 0;
    build();
    for (int i = 0; i < 6; ++i) if (table[(unsigned char)key[i]] != want[i]) return 0;
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
