#include "i8080.h"

/* CPU cycles for each opcode (Intel 8080 Systems User Manual).
   Conditional CALL/RET entries hold the "taken" timing; Emulate() lowers
   them when the branch is not taken. */
static const uint8_t cycles8080[256] = {
	4, 10, 7, 5, 5, 5, 7, 4, 4, 10, 7, 5, 5, 5, 7, 4, //0x00..0x0f
	4, 10, 7, 5, 5, 5, 7, 4, 4, 10, 7, 5, 5, 5, 7, 4, //0x10..0x1f
	4, 10, 16, 5, 5, 5, 7, 4, 4, 10, 16, 5, 5, 5, 7, 4,
	4, 10, 13, 5, 10, 10, 10, 4, 4, 10, 13, 5, 5, 5, 7, 4,

	5, 5, 5, 5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 7, 5, //0x40..0x4f
	5, 5, 5, 5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 7, 5,
	5, 5, 5, 5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 7, 5,
	7, 7, 7, 7, 7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 7, 5,

	4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4, //0x80..0x8f
	4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
	4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
	4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,

	11, 10, 10, 10, 17, 11, 7, 11, 11, 10, 10, 10, 17, 17, 7, 11, //0xc0..0xcf
	11, 10, 10, 10, 17, 11, 7, 11, 11, 10, 10, 10, 17, 17, 7, 11,
	11, 10, 10, 18, 17, 11, 7, 11, 11, 5, 10, 5, 17, 17, 7, 11,
	11, 10, 10, 4, 17, 11, 7, 11, 11, 5, 10, 4, 17, 17, 7, 11,
};

/* 16-bit wrapped memory access */
#define RD(a)    (state->memory[(uint16_t)(a)])
#define WR(a, v) (state->memory[(uint16_t)(a)] = (uint8_t)(v))

static int parity(int x)
{
	x &= 0xff;
	x ^= x >> 4;
	x ^= x >> 2;
	x ^= x >> 1;
	return (~x) & 1;   /* 1 = even parity */
}

static void SetZSP(State8080 *state, uint8_t v)
{
	state->cc.z = (v == 0);
	state->cc.s = (v >> 7) & 1;
	state->cc.p = parity(v);
}

/* ---------- register / register-pair access ---------- */

static uint8_t GetReg(State8080 *state, int r)
{
	switch (r & 7)
	{
	case 0: return state->b;
	case 1: return state->c;
	case 2: return state->d;
	case 3: return state->e;
	case 4: return state->h;
	case 5: return state->l;
	case 6: return RD((state->h << 8) | state->l);
	default: return state->a;
	}
}

static void SetReg(State8080 *state, int r, uint8_t v)
{
	switch (r & 7)
	{
	case 0: state->b = v; break;
	case 1: state->c = v; break;
	case 2: state->d = v; break;
	case 3: state->e = v; break;
	case 4: state->h = v; break;
	case 5: state->l = v; break;
	case 6: WR((state->h << 8) | state->l, v); break;
	default: state->a = v; break;
	}
}

/* rp: 0=BC 1=DE 2=HL 3=SP */
static uint16_t GetPair(State8080 *state, int rp)
{
	switch (rp & 3)
	{
	case 0: return (uint16_t)((state->b << 8) | state->c);
	case 1: return (uint16_t)((state->d << 8) | state->e);
	case 2: return (uint16_t)((state->h << 8) | state->l);
	default: return state->sp;
	}
}

static void SetPair(State8080 *state, int rp, uint16_t v)
{
	switch (rp & 3)
	{
	case 0: state->b = v >> 8; state->c = v & 0xff; break;
	case 1: state->d = v >> 8; state->e = v & 0xff; break;
	case 2: state->h = v >> 8; state->l = v & 0xff; break;
	default: state->sp = v; break;
	}
}

/* ---------- stack ---------- */

static void Push16(State8080 *state, uint16_t v)
{
	WR(state->sp - 1, v >> 8);
	WR(state->sp - 2, v & 0xff);
	state->sp -= 2;
}

static uint16_t Pop16(State8080 *state)
{
	uint16_t v = (uint16_t)(RD(state->sp) | (RD(state->sp + 1) << 8));
	state->sp += 2;
	return v;
}

static uint8_t PackFlags(State8080 *state)
{
	/* S Z 0 AC 0 P 1 CY */
	return (uint8_t)((state->cc.s << 7) | (state->cc.z << 6) |
	                 (state->cc.ac << 4) | (state->cc.p << 2) |
	                 0x02 | state->cc.cy);
}

static void UnpackFlags(State8080 *state, uint8_t f)
{
	state->cc.cy = (f & 0x01) != 0;
	state->cc.p  = (f & 0x04) != 0;
	state->cc.ac = (f & 0x10) != 0;
	state->cc.z  = (f & 0x40) != 0;
	state->cc.s  = (f & 0x80) != 0;
}

/* ---------- ALU ---------- */

static void Add8(State8080 *state, uint8_t v, int carry)
{
	uint16_t res = (uint16_t)state->a + v + carry;
	state->cc.cy = res > 0xff;
	state->cc.ac = ((state->a ^ v ^ res) & 0x10) != 0;
	state->a = res & 0xff;
	SetZSP(state, state->a);
}

/* Sub8 with store=0 is CMP */
static void Sub8(State8080 *state, uint8_t v, int borrow, int store)
{
	int res = (int)state->a - v - borrow;
	state->cc.cy = res < 0;
	state->cc.ac = (((state->a ^ v ^ res) & 0x10) == 0);   /* AC = !borrow from bit 4 */
	SetZSP(state, (uint8_t)res);
	if (store)
		state->a = (uint8_t)res;
}

static void Ana8(State8080 *state, uint8_t v)
{
	state->cc.ac = ((state->a | v) & 0x08) != 0;   /* 8080 quirk */
	state->a &= v;
	state->cc.cy = 0;
	SetZSP(state, state->a);
}

static void Xra8(State8080 *state, uint8_t v)
{
	state->a ^= v;
	state->cc.cy = state->cc.ac = 0;
	SetZSP(state, state->a);
}

static void Ora8(State8080 *state, uint8_t v)
{
	state->a |= v;
	state->cc.cy = state->cc.ac = 0;
	SetZSP(state, state->a);
}

/* op: 0 ADD 1 ADC 2 SUB 3 SBB 4 ANA 5 XRA 6 ORA 7 CMP */
static void Alu(State8080 *state, int op, uint8_t v)
{
	switch (op & 7)
	{
	case 0: Add8(state, v, 0); break;
	case 1: Add8(state, v, state->cc.cy); break;
	case 2: Sub8(state, v, 0, 1); break;
	case 3: Sub8(state, v, state->cc.cy, 1); break;
	case 4: Ana8(state, v); break;
	case 5: Xra8(state, v); break;
	case 6: Ora8(state, v); break;
	default: Sub8(state, v, 0, 0); break;
	}
}

static void DAA(State8080 *state)
{
	uint16_t a = state->a;
	uint8_t cy = state->cc.cy;
	uint8_t ac = 0;

	if ((a & 0x0f) > 9 || state->cc.ac)
	{
		ac = ((a & 0x0f) + 6) > 0x0f;
		a += 6;
	}
	if ((a >> 4) > 9 || cy)
	{
		a += 0x60;
		cy = 1;
	}
	state->a = a & 0xff;
	state->cc.cy = cy;
	state->cc.ac = ac;
	SetZSP(state, state->a);
}

static uint8_t Inr(State8080 *state, uint8_t v)
{
	uint8_t res = v + 1;
	state->cc.ac = ((res & 0x0f) == 0);
	SetZSP(state, res);
	return res;
}

static uint8_t Dcr(State8080 *state, uint8_t v)
{
	uint8_t res = v - 1;
	state->cc.ac = ((v & 0x0f) != 0);
	SetZSP(state, res);
	return res;
}

/* cond: 0 NZ 1 Z 2 NC 3 C 4 PO 5 PE 6 P 7 M */
static int Cond(State8080 *state, int cond)
{
	switch (cond & 7)
	{
	case 0: return !state->cc.z;
	case 1: return state->cc.z;
	case 2: return !state->cc.cy;
	case 3: return state->cc.cy;
	case 4: return !state->cc.p;
	case 5: return state->cc.p;
	case 6: return !state->cc.s;
	default: return state->cc.s;
	}
}

/*
Emulate Intel 8080 CPU Operation
Returns the number of cycles consumed.
HLT no longer terminates the host process: it sets state->halted and the
CPU idles (4 cycles per call) until GenInterrupt() or the host clears it.
*/
int Emulate(State8080 *state, bool printTrace)
{
	if (state->halted)
		return 4;

	if (printTrace)
		Disassemble8080(state->memory, state->pc);

	uint8_t op = RD(state->pc);
	state->pc++;
	int cycles = cycles8080[op];

	/* operand helpers (read lazily, PC advanced only when used) */
#define IMM8()  (state->pc++, RD(state->pc - 1))
#define IMM16() (state->pc += 2, (uint16_t)(RD(state->pc - 2) | (RD(state->pc - 1) << 8)))

	switch (op >> 6)
	{
	case 0: /* 0x00 - 0x3F */
		switch (op & 7)
		{
		case 0: /* NOP (and undocumented NOP aliases, incl. 0x20 / 0x30) */
			break;
		case 1:
			if (op & 8) /* DAD rp */
			{
				uint32_t r = (uint32_t)GetPair(state, op >> 4) + GetPair(state, 2);
				state->cc.cy = (r > 0xffff);
				SetPair(state, 2, (uint16_t)r);
			}
			else /* LXI rp,d16 */
				SetPair(state, op >> 4, IMM16());
			break;
		case 2:
			switch (op >> 3)
			{
			case 0: WR(GetPair(state, 0), state->a); break;               /* STAX B */
			case 1: state->a = RD(GetPair(state, 0)); break;              /* LDAX B */
			case 2: WR(GetPair(state, 1), state->a); break;               /* STAX D */
			case 3: state->a = RD(GetPair(state, 1)); break;              /* LDAX D */
			case 4: { uint16_t ad = IMM16();                              /* SHLD   */
			          WR(ad, state->l); WR(ad + 1, state->h); } break;
			case 5: { uint16_t ad = IMM16();                              /* LHLD   */
			          state->l = RD(ad); state->h = RD(ad + 1); } break;
			case 6: { uint16_t ad = IMM16(); WR(ad, state->a); } break;   /* STA    */
			default: { uint16_t ad = IMM16(); state->a = RD(ad); } break; /* LDA    */
			}
			break;
		case 3: /* INX / DCX */
			SetPair(state, op >> 4, (uint16_t)(GetPair(state, op >> 4) + ((op & 8) ? -1 : 1)));
			break;
		case 4: /* INR r */
			SetReg(state, op >> 3, Inr(state, GetReg(state, op >> 3)));
			break;
		case 5: /* DCR r */
			SetReg(state, op >> 3, Dcr(state, GetReg(state, op >> 3)));
			break;
		case 6: /* MVI r,d8 */
			SetReg(state, op >> 3, IMM8());
			break;
		default: /* 7: RLC RRC RAL RAR DAA CMA STC CMC */
			switch (op >> 3)
			{
			case 0: state->cc.cy = state->a >> 7;                         /* RLC */
			        state->a = (uint8_t)((state->a << 1) | state->cc.cy); break;
			case 1: state->cc.cy = state->a & 1;                          /* RRC */
			        state->a = (uint8_t)((state->a >> 1) | (state->cc.cy << 7)); break;
			case 2: { uint8_t c = state->cc.cy;                           /* RAL */
			          state->cc.cy = state->a >> 7;
			          state->a = (uint8_t)((state->a << 1) | c); } break;
			case 3: { uint8_t c = state->cc.cy;                           /* RAR */
			          state->cc.cy = state->a & 1;
			          state->a = (uint8_t)((state->a >> 1) | (c << 7)); } break;
			case 4: DAA(state); break;
			case 5: state->a = ~state->a; break;                          /* CMA */
			case 6: state->cc.cy = 1; break;                              /* STC */
			default: state->cc.cy = !state->cc.cy; break;                 /* CMC */
			}
			break;
		}
		break;

	case 1: /* 0x40 - 0x7F : MOV / HLT */
		if (op == 0x76)
			state->halted = HALT_HLT;
		else
			SetReg(state, op >> 3, GetReg(state, op));
		break;

	case 2: /* 0x80 - 0xBF : ALU r */
		Alu(state, op >> 3, GetReg(state, op));
		break;

	default: /* 0xC0 - 0xFF */
		switch (op & 7)
		{
		case 0: /* Rcc */
			if (Cond(state, op >> 3))
				state->pc = Pop16(state);
			else
				cycles = 5;
			break;
		case 1:
			if (!(op & 8)) /* POP rp / POP PSW */
			{
				int rp = (op >> 4) & 3;
				if (rp == 3)
				{
					uint8_t f = RD(state->sp);
					state->a = RD(state->sp + 1);
					state->sp += 2;
					UnpackFlags(state, f);
				}
				else
					SetPair(state, rp, Pop16(state));
			}
			else switch (op >> 4)
			{
			case 0xC: case 0xD: state->pc = Pop16(state); break;           /* RET (0xC9, 0xD9 alias) */
			case 0xE: state->pc = GetPair(state, 2); break;                /* PCHL */
			default:  state->sp = GetPair(state, 2); break;                /* SPHL */
			}
			break;
		case 2: /* Jcc */
		{
			uint16_t ad = IMM16();
			if (Cond(state, op >> 3))
				state->pc = ad;
			break;
		}
		case 3:
			switch (op >> 3)
			{
			case 0x18: case 0x19: state->pc = IMM16(); break;              /* JMP (0xC3, 0xCB alias) */
			case 0x1A: { uint8_t port = IMM8();                            /* OUT */
			             if (state->io_out) state->io_out(port, state->a); } break;
			case 0x1B: { uint8_t port = IMM8();                            /* IN  */
			             state->a = state->io_in ? state->io_in(port) : 0xFF; } break;
			case 0x1C: { uint8_t l = RD(state->sp), h = RD(state->sp + 1); /* XTHL */
			             WR(state->sp, state->l); WR(state->sp + 1, state->h);
			             state->l = l; state->h = h; } break;
			case 0x1D: { uint8_t d = state->d, e = state->e;               /* XCHG */
			             state->d = state->h; state->e = state->l;
			             state->h = d; state->l = e; } break;
			case 0x1E: state->int_enable = 0; break;                       /* DI */
			default:   state->int_enable = 1; break;                       /* EI */
			}
			break;
		case 4: /* Ccc */
		{
			uint16_t ad = IMM16();
			if (Cond(state, op >> 3))
			{
				Push16(state, state->pc);
				state->pc = ad;
			}
			else
				cycles = 11;
			break;
		}
		case 5:
			if (!(op & 8)) /* PUSH rp / PUSH PSW */
			{
				int rp = (op >> 4) & 3;
				if (rp == 3)
				{
					WR(state->sp - 2, PackFlags(state));
					WR(state->sp - 1, state->a);
					state->sp -= 2;
				}
				else
					Push16(state, GetPair(state, rp));
			}
			else /* CALL (0xCD, 0xDD/0xED/0xFD aliases) */
			{
				uint16_t ad = IMM16();
				Push16(state, state->pc);
				state->pc = ad;
			}
			break;
		case 6: /* ADI ACI SUI SBI ANI XRI ORI CPI */
			Alu(state, op >> 3, IMM8());
			break;
		default: /* RST n : return address is the NEXT instruction (PC already advanced) */
			Push16(state, state->pc);
			state->pc = (uint16_t)(op & 0x38);
			break;
		}
		break;
	}
#undef IMM8
#undef IMM16

	return cycles;
}

/* ---------- disassembler ---------- */

int Disassemble8080(unsigned char *codebuffer, int pc)
{
	static const char *regs[8]  = {"B", "C", "D", "E", "H", "L", "M", "A"};
	static const char *rps[4]   = {"B", "D", "H", "SP"};
	static const char *rpp[4]   = {"B", "D", "H", "PSW"};
	static const char *alu[8]   = {"ADD", "ADC", "SUB", "SBB", "ANA", "XRA", "ORA", "CMP"};
	static const char *alui[8]  = {"ADI", "ACI", "SUI", "SBI", "ANI", "XRI", "ORI", "CPI"};
	static const char *cc[8]    = {"NZ", "Z", "NC", "C", "PO", "PE", "P", "M"};
	static const char *rot[8]   = {"RLC", "RRC", "RAL", "RAR", "DAA", "CMA", "STC", "CMC"};

	unsigned char *code = &codebuffer[pc & 0xffff];
	unsigned char b1 = codebuffer[(pc + 1) & 0xffff];
	unsigned char b2 = codebuffer[(pc + 2) & 0xffff];
	unsigned char op = *code;
	int opbytes = 1;

	printf("%04x ", pc & 0xffff);

	switch (op >> 6)
	{
	case 0:
		switch (op & 7)
		{
		case 0: printf("NOP"); break;
		case 1:
			if (op & 8) printf("DAD    %s", rps[(op >> 4) & 3]);
			else { printf("LXI    %s,#$%02x%02x", rps[(op >> 4) & 3], b2, b1); opbytes = 3; }
			break;
		case 2:
			switch (op >> 3)
			{
			case 0: printf("STAX   B"); break;
			case 1: printf("LDAX   B"); break;
			case 2: printf("STAX   D"); break;
			case 3: printf("LDAX   D"); break;
			case 4: printf("SHLD   $%02x%02x", b2, b1); opbytes = 3; break;
			case 5: printf("LHLD   $%02x%02x", b2, b1); opbytes = 3; break;
			case 6: printf("STA    $%02x%02x", b2, b1); opbytes = 3; break;
			default: printf("LDA    $%02x%02x", b2, b1); opbytes = 3; break;
			}
			break;
		case 3: printf("%s    %s", (op & 8) ? "DCX" : "INX", rps[(op >> 4) & 3]); break;
		case 4: printf("INR    %s", regs[(op >> 3) & 7]); break;
		case 5: printf("DCR    %s", regs[(op >> 3) & 7]); break;
		case 6: printf("MVI    %s,#$%02x", regs[(op >> 3) & 7], b1); opbytes = 2; break;
		default: printf("%s", rot[(op >> 3) & 7]); break;
		}
		break;
	case 1:
		if (op == 0x76) printf("HLT");
		else printf("MOV    %s,%s", regs[(op >> 3) & 7], regs[op & 7]);
		break;
	case 2:
		printf("%s    %s", alu[(op >> 3) & 7], regs[op & 7]);
		break;
	default:
		switch (op & 7)
		{
		case 0: printf("R%s", cc[(op >> 3) & 7]); break;
		case 1:
			if (!(op & 8)) printf("POP    %s", rpp[(op >> 4) & 3]);
			else switch (op >> 4)
			{
			case 0xC: case 0xD: printf("RET"); break;
			case 0xE: printf("PCHL"); break;
			default:  printf("SPHL"); break;
			}
			break;
		case 2: printf("J%s     $%02x%02x", cc[(op >> 3) & 7], b2, b1); opbytes = 3; break;
		case 3:
			switch (op >> 3)
			{
			case 0x18: case 0x19: printf("JMP    $%02x%02x", b2, b1); opbytes = 3; break;
			case 0x1A: printf("OUT    #$%02x", b1); opbytes = 2; break;
			case 0x1B: printf("IN     #$%02x", b1); opbytes = 2; break;
			case 0x1C: printf("XTHL"); break;
			case 0x1D: printf("XCHG"); break;
			case 0x1E: printf("DI"); break;
			default:   printf("EI"); break;
			}
			break;
		case 4: printf("C%s     $%02x%02x", cc[(op >> 3) & 7], b2, b1); opbytes = 3; break;
		case 5:
			if (!(op & 8)) printf("PUSH   %s", rpp[(op >> 4) & 3]);
			else { printf("CALL   $%02x%02x", b2, b1); opbytes = 3; }
			break;
		case 6: printf("%s    #$%02x", alui[(op >> 3) & 7], b1); opbytes = 2; break;
		default: printf("RST    %d", (op >> 3) & 7); break;
		}
		break;
	}

	printf("\r\n");
	return opbytes;
}

void GenInterrupt(State8080 *state, int interrupt_num)
{
	state->halted = HALT_NONE;           /* an interrupt wakes the CPU from HLT */
	Push16(state, state->pc);
	state->pc = (uint16_t)(8 * (interrupt_num & 7));
	state->int_enable = 0;
}

void ReadFileIntoMemory(State8080 *state, char *filename, uint32_t offset)
{
	FILE *f = fopen(filename, "rb");
	if (f == NULL)
	{
		fprintf(stderr, "error opening file: %s\n", filename);
		return;
	}
	if (offset >= 0x10000)
	{
		fclose(f);
		return;
	}

	size_t room = 0x10000 - offset;
	size_t got = fread(&state->memory[offset], 1, room, f);
	(void)got;
	fclose(f);
}

State8080 *Init8080(void)
{
	State8080 *state = calloc(1, sizeof(State8080));
	if (!state)
		return NULL;
	state->memory = calloc(1, 0x10000);   /* 64 KB, zeroed */
	if (!state->memory)
	{
		free(state);
		return NULL;
	}
	return state;
}

void Free8080(State8080 *state)
{
	if (!state)
		return;
	free(state->memory);
	free(state);
}
