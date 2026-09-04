/* Stress 02: arithmetic on parameters. */
int mix(int a, int b, int c) { return a * b + c - (a ^ b); }
int main(void) { return mix(3, 5, 7) != 0 ? 0 : 1; }
