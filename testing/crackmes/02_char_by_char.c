/* Crackme 02: compared one character at a time. */
#include <stdio.h>
int check(const char *key) {
    static const char wanted[] = {'o','r','b','i','t',0};
    for (int i = 0; wanted[i]; ++i) if (key[i] != wanted[i]) return 0;
    return key[5] == 0;
}

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
