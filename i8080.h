#ifndef I8080_H
#define I8080_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

/*opcode flags
z   - zero        set to 1 when result equals 0
s   - sign        set to 1 when bit 7 (MSB) is set
p   - parity      set whenever answer has even parity
cy  - carry       set to 1 when instruction resulted in carry out
ac  - aux carry   carry out of the least significant four bits
*/
typedef struct ConditionCodes
{
    uint8_t z : 1;
    uint8_t s : 1;
    uint8_t p : 1;
    uint8_t cy : 1;
    uint8_t ac : 1;
    uint8_t pad : 3;
} ConditionCodes;

/* halted values */
#define HALT_NONE    0
#define HALT_HLT     1   /* HLT executed - resumes on interrupt            */
#define HALT_ILLEGAL 2   /* undecodable opcode - needs reset (never raised */
                         /* by the current decoder, kept for safety)       */

typedef struct State8080
{
    uint8_t a;
    uint8_t b;
    uint8_t c;
    uint8_t d;
    uint8_t e;
    uint8_t h;
    uint8_t l;
    uint16_t sp;
    uint16_t pc;
    uint8_t *memory;               /* 64 KB, always accessed with 16-bit wrap */
    struct ConditionCodes cc;
    uint8_t int_enable;
    uint8_t halted;                /* HALT_* - Emulate() never calls exit()   */

    /* Optional I/O hooks used by IN / OUT. NULL = open bus (IN returns 0xFF) */
    uint8_t (*io_in)(uint8_t port);
    void    (*io_out)(uint8_t port, uint8_t value);
} State8080;

//Emulate Intel 8080 CPU
//Returns number of cycles for each executed instruction
int Emulate(State8080 *state, bool printTrace);

//Prints out the instruction at pc, returns its length in bytes
int Disassemble8080(unsigned char *codebuffer, int pc);

//Facilitates Interrupts (RST n). Wakes the CPU from HLT.
void GenInterrupt(State8080 *state, int interrupt_num);

//Read desired ROM file into Memory at specified offset (bounds checked)
void ReadFileIntoMemory(State8080 *state, char *filename, uint32_t offset);

//Allocate CPU struct (zeroed, with 64 KB zeroed RAM). Returns NULL on failure.
State8080 *Init8080(void);

//Free CPU struct and its memory
void Free8080(State8080 *state);

#endif /* I8080_H */
