/* Stress 08: arrays, indexing and bounds. */
#include <string.h>
int longest_run(const char *text) {
    int best = 0, run = 0;
    char last = 0;
    for (size_t i = 0; text[i]; ++i) {
        run = text[i] == last ? run + 1 : 1;
        last = text[i];
        if (run > best) best = run;
    }
    return best;
}
int main(void) { return longest_run("aabbbcc") == 3 ? 0 : 1; }
