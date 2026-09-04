/* Stress 19: large struct copies and vectorizable loops. */
#include <string.h>
#include <stdint.h>
struct Record { uint64_t id; double weight; char label[32]; };
void copy_all(struct Record *to, const struct Record *from, int count) {
    for (int i = 0; i < count; ++i) to[i] = from[i];
}
double weigh(const struct Record *r, int count) {
    double t = 0;
    for (int i = 0; i < count; ++i) t += r[i].weight * (double)(r[i].id & 0xff);
    return t;
}
int main(void) {
    struct Record a[8], b[8];
    memset(a, 0, sizeof a);
    for (int i = 0; i < 8; ++i) { a[i].id = i; a[i].weight = i * 1.5; }
    copy_all(b, a, 8);
    return weigh(b, 8) > 0 ? 0 : 1;
}
