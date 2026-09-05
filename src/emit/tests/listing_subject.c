/* The subject the listing tests read.
 *
 * Each function is here because it makes the printer do one thing: a frame slot
 * that no symbol names, a request number that only means something in an ioctl
 * call, the same number where it means nothing, and a value first given inside
 * a branch. Built at -O0 so none of it folds away before the decompiler sees
 * it. */
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

/* A local whose address escapes into a call, which is what keeps it in the
 * frame instead of in a register. */
int measure_terminal(int fd)
{
    struct winsize size;
    if (ioctl(fd, TIOCGWINSZ, &size) == -1)
        return -1;
    return size.ws_col;
}

/* The same number as the request above, in a slot where it means nothing. */
unsigned long same_number_elsewhere(unsigned long n)
{
    return n + 0x40087468UL;
}

/* Two values, each first given inside a branch. */
int first_use_inside_a_branch(int n)
{
    int total;
    if (n > 0) {
        int scaled = n * 3;
        total = scaled + 1;
    } else {
        total = 0;
    }
    return total;
}

/* Widths the listing has to spell: a byte, a half, a word and a long. */
long widths(unsigned char a, unsigned short b, unsigned int c, long d)
{
    return (long)a + (long)b + (long)c + d;
}

int main(void)
{
    char text[64];
    snprintf(text, sizeof text, "%d %lu %d %ld", measure_terminal(1),
             same_number_elsewhere(1), first_use_inside_a_branch(4), widths(1, 2, 3, 4));
    return (int)strlen(text);
}
