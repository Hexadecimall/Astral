/* Stress 16: variadic arguments. */
#include <stdarg.h>
int sum_all(int count, ...) {
    va_list args;
    va_start(args, count);
    int total = 0;
    for (int i = 0; i < count; ++i) total += va_arg(args, int);
    va_end(args);
    return total;
}
int main(void) { return sum_all(4, 1, 2, 3, 4) == 10 ? 0 : 1; }
