/* Stress 09: structs passed by pointer and by value. */
struct Point { int x, y; };
struct Box { struct Point lo, hi; };
int area(struct Box b) { return (b.hi.x - b.lo.x) * (b.hi.y - b.lo.y); }
void grow(struct Box *b, int by) { b->lo.x -= by; b->lo.y -= by; b->hi.x += by; b->hi.y += by; }
int main(void) {
    struct Box b = {{0,0},{3,4}};
    grow(&b, 1);
    return area(b) == 30 ? 0 : 1;
}
