/* Stress 15: bitfields and packed layout. */
#include <stdint.h>
struct Header {
    unsigned version : 4;
    unsigned kind : 4;
    unsigned length : 16;
    unsigned flags : 8;
};
uint32_t pack(struct Header h) {
    return (uint32_t)h.version | ((uint32_t)h.kind << 4) | ((uint32_t)h.length << 8) |
           ((uint32_t)h.flags << 24);
}
struct Header unpack(uint32_t raw) {
    struct Header h;
    h.version = raw & 0xf; h.kind = (raw >> 4) & 0xf;
    h.length = (raw >> 8) & 0xffff; h.flags = (raw >> 24) & 0xff;
    return h;
}
int main(void) {
    struct Header h = {2, 3, 1024, 0x80};
    return unpack(pack(h)).length == 1024 ? 0 : 1;
}
