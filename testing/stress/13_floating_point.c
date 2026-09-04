/* Stress 13: floating point, including comparisons and conversion. */
double mean(const double *v, int n) { double t = 0; for (int i = 0; i < n; ++i) t += v[i]; return n ? t / n : 0; }
double variance(const double *v, int n) {
    double m = mean(v, n), t = 0;
    for (int i = 0; i < n; ++i) { double d = v[i] - m; t += d * d; }
    return n ? t / n : 0;
}
int main(void) { double v[4] = {2,4,4,4}; return variance(v, 4) == 0.75 ? 0 : 1; }
