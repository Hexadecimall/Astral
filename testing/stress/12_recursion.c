/* Stress 12: recursion, and two functions calling each other. */
static int is_odd(int n);
static int is_even(int n) { return n == 0 ? 1 : is_odd(n - 1); }
static int is_odd(int n) { return n == 0 ? 0 : is_even(n - 1); }
int ackermann_small(int m, int n) {
    if (m == 0) return n + 1;
    if (n == 0) return ackermann_small(m - 1, 1);
    return ackermann_small(m - 1, ackermann_small(m, n - 1));
}
int main(void) { return (is_even(10) && ackermann_small(2, 3) == 9) ? 0 : 1; }
