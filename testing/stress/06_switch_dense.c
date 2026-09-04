/* Stress 06: a dense switch, which becomes a jump table. */
int step(int state) {
    switch (state) {
    case 0: return 10;
    case 1: return 20;
    case 2: return 30;
    case 3: return 40;
    case 4: return 50;
    case 5: return 60;
    case 6: return 70;
    case 7: return 80;
    default: return -1;
    }
}
int main(void) { return step(4) == 50 ? 0 : 1; }
