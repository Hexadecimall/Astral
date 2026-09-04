/* Stress 11: dispatch through a table of function pointers. */
static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int mul(int a, int b) { return a * b; }
typedef int (*op)(int, int);
static op table[3] = { add, sub, mul };
int apply(int which, int a, int b) { return which < 3 ? table[which](a, b) : 0; }
int main(void) { return apply(2, 6, 7) == 42 ? 0 : 1; }
