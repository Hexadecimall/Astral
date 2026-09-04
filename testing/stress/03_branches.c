/* Stress 03: if, else if, else. */
int classify(int v) {
    if (v < 0) return -1;
    else if (v == 0) return 0;
    else if (v < 100) return 1;
    return 2;
}
int main(void) { return classify(50) == 1 ? 0 : 1; }
