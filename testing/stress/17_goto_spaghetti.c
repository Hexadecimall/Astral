/* Stress 17: control flow no structuring pass will enjoy. */
int tangle(int n) {
    int acc = 0;
    int i = 0;
start:
    if (i >= n) goto done;
    if (i % 3 == 0) goto three;
    if (i % 5 == 0) goto five;
    acc += 1;
    goto next;
three:
    acc += 3;
    if (i % 15 == 0) goto both;
    goto next;
five:
    acc += 5;
    goto next;
both:
    acc += 15;
next:
    ++i;
    goto start;
done:
    return acc;
}
int main(void) { return tangle(30) > 0 ? 0 : 1; }
