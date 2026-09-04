/* Stress 05: nested loops with an early exit. */
int find(const int *grid, int rows, int cols, int wanted) {
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            if (grid[r * cols + c] == wanted) return r * cols + c;
    return -1;
}
int main(void) { int g[6] = {1,2,3,4,5,6}; return find(g, 2, 3, 5) == 4 ? 0 : 1; }
