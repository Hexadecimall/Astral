/* Stress 10: pointer arithmetic and indirection. */
int sum_stride(const int *base, int count, int stride) {
    const int *p = base;
    int total = 0;
    for (int i = 0; i < count; ++i) { total += *p; p += stride; }
    return total;
}
int main(void) { int v[9] = {1,2,3,4,5,6,7,8,9}; return sum_stride(v, 3, 3) == 12 ? 0 : 1; }
