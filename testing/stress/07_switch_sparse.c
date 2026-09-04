/* Stress 07: a sparse switch, which becomes a comparison chain. */
int lookup(int code) {
    switch (code) {
    case 1: return 100;
    case 97: return 200;
    case 1013: return 300;
    case 65537: return 400;
    default: return 0;
    }
}
int main(void) { return lookup(1013) == 300 ? 0 : 1; }
