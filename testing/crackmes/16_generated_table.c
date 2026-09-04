/* Crackme 16: a table built at run time, then indexed by the key. */
#include <stdio.h>
#include <string.h>
static unsigned char table[128];
static void build(void) {
    unsigned char v = 1;
    for (int i = 0; i < 128; ++i) { table[i] = v; v = (unsigned char)((v * 3) ^ (i & 0x1f)); }
}
int check(const char *key) {
    static const unsigned char want[] = {0x4b,0x1c,0x0d,0x23,0x21,0x23,0x0d};
    if (strlen(key) != 7) return 0;
    build();
    for (int i = 0; i < 7; ++i) if (table[key[i] & 0x7f] != want[i]) return 0;
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
