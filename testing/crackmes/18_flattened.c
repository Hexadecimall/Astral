/* Crackme 18: the check itself is a flattened state machine. */
#include <stdio.h>
#include <string.h>
int check(const char *key) {
    int state = 1, index = 0, ok = 1, guard = 0;
    static const char wanted[] = "flatten";
    while (state && guard++ < 128) {
        switch (state) {
        case 1: state = strlen(key) == 7 ? 2 : 6; break;
        case 2: state = index < 7 ? 3 : 5; break;
        case 3: ok = ok && key[index] == wanted[index]; state = 4; break;
        case 4: ++index; state = ok ? 2 : 6; break;
        case 5: state = 0; break;
        case 6: ok = 0; state = 0; break;
        default: state = 0; break;
        }
    }
    return ok;
}

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage: %s <key>\n", argv[0]); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
