#include "i8080.h"

void i8080_reset(i8080* cpu) {
    if (!cpu) return;
    cpu->pc = 0;
    cpu->sp = 0;
    for(int i = 0; i < 65536; i++) cpu->memory[i] = 0;
}

void i8080_step(i8080* cpu) {
    if (!cpu) return;
    uint8_t opcode = cpu->memory[cpu->pc];
    cpu->pc++;
    switch(opcode) {
        case 0x00: break; 
        case 0xC3: {
            uint16_t addr = cpu->memory[cpu->pc] | (cpu->memory[cpu->pc+1] << 8);
            cpu->pc = addr;
            break;
        }
        default: break;
    }
}
