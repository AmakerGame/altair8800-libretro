#ifndef I8080_H
#define I8080_H
#include <stdint.h>

typedef struct {
    uint8_t a, b, c, d, e, h, l;
    uint16_t pc, sp;
    uint8_t flags;
    uint8_t memory[65536];
} i8080;

void i8080_reset(i8080* cpu);
void i8080_step(i8080* cpu);
#endif