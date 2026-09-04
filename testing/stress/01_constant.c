/* Stress 01: return a constant. */
int answer(void) { return 42; }
int main(void) { return answer() == 42 ? 0 : 1; }
