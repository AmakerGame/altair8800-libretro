#include "i8080.h"

void i8080_reset(i8080* cpu) {
    if (!cpu) return;
    cpu->pc = 0;
    cpu->sp = 0;
    for(int i = 0; i < 65536; i++) cpu->memory[i] = 0;
}

void i8080_step(i8080* cpu) {
    if (!cpu) return;
    
    if (cpu->pc >= 65536) {
        cpu->pc = 0; 
        return;
    }

    uint8_t opcode = cpu->memory[cpu->pc];
    
    switch(opcode) {
        case 0x00: 
            cpu->pc++;
            break;
        case 0xC3: { // JMP
            uint16_t low = cpu->memory[(cpu->pc + 1) & 0xFFFF];
            uint16_t high = cpu->memory[(cpu->pc + 2) & 0xFFFF];
            cpu->pc = low | (high << 8);
            break;
        }
        default:
            cpu->pc++;
            break;
    }
}
