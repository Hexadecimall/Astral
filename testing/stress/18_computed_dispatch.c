/* Stress 18: a hand-rolled dispatch loop over an opcode stream. */
#include <stdint.h>
int execute(const uint8_t *code, int length) {
    int stack[32]; int top = 0; int pc = 0;
    while (pc < length) {
        switch (code[pc++]) {
        case 0: stack[top++] = code[pc++]; break;
        case 1: { int b = stack[--top], a = stack[--top]; stack[top++] = a + b; break; }
        case 2: { int b = stack[--top], a = stack[--top]; stack[top++] = a * b; break; }
        case 3: { int b = stack[--top], a = stack[--top]; stack[top++] = a - b; break; }
        case 4: return top ? stack[top - 1] : 0;
        default: return -1;
        }
    }
    return top ? stack[top - 1] : 0;
}
int main(void) {
    const uint8_t program[] = {0, 6, 0, 7, 2, 4};
    return execute(program, sizeof program) == 42 ? 0 : 1;
}
