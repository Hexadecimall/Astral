/* Stress 04: a counted loop. */
int total(int n) { int t = 0; for (int i = 1; i <= n; ++i) t += i; return t; }
int main(void) { return total(10) == 55 ? 0 : 1; }
