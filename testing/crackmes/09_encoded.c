/* Crackme 09: decoded from a packed form before comparison. */
#include <stdio.h>
#include <string.h>
static void decode(const unsigned char *in, char *out, int n) {
    for (int i = 0; i < n; ++i) out[i] = (char)(((in[i] >> 3) | (in[i] << 5)) & 0xff);
    out[n] = 0;
}
int check(const char *key) {
    static const unsigned char packed[] = {0x63,0x0b,0x73,0xa3,0x2b,0x93,0x73};
    char wanted[16];
    decode(packed, wanted, 7);
    return strcmp(key, wanted) == 0;
}

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
