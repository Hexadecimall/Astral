/* Crackme 12: a state machine walked by the key. */
#include <stdio.h>
#include <string.h>
int check(const char *key) {
    if (strlen(key) != 8) return 0;
    int state = 0;
    for (int i = 0; i < 8; ++i) {
        switch (state) {
        case 0: state = key[i] == 'a' ? 1 : 9; break;
        case 1: state = key[i] == 'b' ? 2 : 9; break;
        case 2: state = key[i] == 'c' ? 0 : 9; break;
        default: return 0;
        }
    }
    return state == 2;
}

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
