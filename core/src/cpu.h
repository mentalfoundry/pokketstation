#ifndef PSEMU_CPU_H
#define PSEMU_CPU_H

#include <stdint.h>

#include "memory.h"

#define CPSR_N (1u << 31)
#define CPSR_Z (1u << 30)
#define CPSR_C (1u << 29)
#define CPSR_V (1u << 28)
#define CPSR_I (1u << 7)
#define CPSR_F (1u << 6)
#define CPSR_T (1u << 5)
#define CPSR_MODE_MASK 0x1Fu

#define ARM_MODE_USR 0x10u
#define ARM_MODE_FIQ 0x11u
#define ARM_MODE_IRQ 0x12u
#define ARM_MODE_SVC 0x13u
#define ARM_MODE_ABT 0x17u
#define ARM_MODE_UND 0x1Bu
#define ARM_MODE_SYS 0x1Fu

#define ARM_IRQ_VECTOR 0x18u
#define ARM_FIQ_VECTOR 0x1Cu

/* Bank slots: 0=fiq, 1=irq, 2=svc, 3=abt, 4=und, 5=usr/sys (shared, per spec). */
#define ARM_BANK_COUNT 6

/* Ring buffer of recently executed (pc, cpsr) pairs.
   arm7tdmi_step records one entry every step, regardless of caller.
   psemu_write_crash_report (psemu.c/psemu.h) uses this buffer to show
   what led up to a fault. A frontend needs no tracing of its own for
   this. See docs/hardware-notes.md for the Chocobo World crash
   investigation that motivated this buffer.
   The buffer originally held 256 entries. A real crash report showed a
   long straight-line drift through non-code memory. This drift filled
   the entire 256-entry window and erased the history of the bad jump
   that caused it. This project raised the size to 8192 entries (64KB
   per instance) to fix that. */
#define PSEMU_TRACE_SIZE 8192

typedef struct {
    uint32_t pc;
    uint32_t cpsr;
} psemu_trace_entry_t;

typedef struct {
    uint32_t r[16]; /* r0-r14 general purpose (banked view for current mode), r15 = pc */
    uint32_t cpsr;

    uint32_t r13_bank[ARM_BANK_COUNT];
    uint32_t r14_bank[ARM_BANK_COUNT];
    uint32_t spsr_bank[ARM_BANK_COUNT];

    psemu_bus_t *bus;
    int halted;
    int unimplemented; /* set when an unrecognized opcode is hit; sticky until cleared by caller */

    psemu_trace_entry_t trace[PSEMU_TRACE_SIZE];
    uint32_t trace_pos;    /* next slot to write; wraps, so this counts total steps mod PSEMU_TRACE_SIZE */
    uint64_t total_steps;  /* monotonically increasing count of arm7tdmi_step calls, never wraps in practice */
} arm7tdmi_t;

typedef struct {
    uint32_t value;
    int carry;
} arm_shift_result_t;

void arm7tdmi_init(arm7tdmi_t *cpu, psemu_bus_t *bus);
void arm7tdmi_reset(arm7tdmi_t *cpu, uint32_t reset_vector);

/* Executes one instruction, or delivers a pending FIQ/IRQ instead.
   Returns the real wait-state cycle cost of that step: the region-costed
   opcode fetch, plus any data-access, internal, and pipeline-refill
   cycles the instruction needed. See docs/hardware-notes.md, "Memory
   access timing".
   A halted or unimplemented-fault step returns a flat 1, since nothing
   real executes in that case. */
uint32_t arm7tdmi_step(arm7tdmi_t *cpu);

/* Diagnostic hook: the PC of the instruction currently executing.
   arm7tdmi_step updates this value at the top of every step.
   Register read/write traces use this value to log the real PC and CPU
   mode for each access. Examples: intc.c's psemu_intc_trace_enabled and
   similar opt-in trace flags, memory.c's CLK_MODE/DAC_CTRL logging, and
   tools/inspect.c.
   This tracking matters because the real BIOS mixes ARM and Thumb code.
   Static disassembly without mode-tracking cannot follow this mix.
   This project added the hook for one specific investigation (see
   docs/hardware-notes.md). It now serves as permanent, general-purpose
   diagnostic infrastructure. */
extern uint32_t psemu_debug_current_pc;

/* Shared helpers used by arm_exec.c / thumb_exec.c, implemented in cpu.c. */
int arm_condition_passed(arm7tdmi_t *cpu, uint32_t cond);
uint32_t arm_read_reg(arm7tdmi_t *cpu, int n, uint32_t pc, int thumb);
void arm_write_reg(arm7tdmi_t *cpu, int n, uint32_t value);
void arm_set_mode(arm7tdmi_t *cpu, uint32_t new_mode);
int arm_current_bank(arm7tdmi_t *cpu);
void arm_set_nz(arm7tdmi_t *cpu, uint32_t result);
uint32_t arm_adc_raw(uint32_t a, uint32_t b, uint32_t carry_in, int *carry_out, int *overflow);
arm_shift_result_t arm_apply_shift(uint32_t value, int shift_type, uint32_t amount, int carry_in, int is_immediate_encoding);
void arm_enter_exception(arm7tdmi_t *cpu, uint32_t mode, uint32_t vector, uint32_t return_addr);

/* Adds `n` cycles on top of the automatic opcode-fetch/data-access cost.
   Covers two cases:
   - Internal-only (I) cycles with no bus access of their own: a
     register-specified shift, a multiply's data-dependent extra cycles,
     or LDR/LDM's fixed +1I.
   - Pipeline-refill fetches when an instruction changes PC: branches,
     BX, a PC-writing data-processing/LDR/LDM instruction, or exception
     entry.
   See docs/hardware-notes.md, "Memory access timing", and the call
   sites in arm_exec.c and thumb_exec.c. */
void arm7tdmi_add_cycles(arm7tdmi_t *cpu, uint32_t n);
/* Real ARM7TDMI multiply early-termination cycle count (m = 1..4) for a
   given Rs value. See the definition in arm_exec.c for the rule.
   Shared by arm_exec.c's MUL/MLA/UMULL/UMLAL/SMULL/SMLAL and
   thumb_exec.c's MUL. */
uint32_t arm7tdmi_mul_m_cycles(uint32_t rs);

void arm_execute(arm7tdmi_t *cpu, uint32_t instr, uint32_t pc);
void thumb_execute(arm7tdmi_t *cpu, uint16_t instr, uint32_t pc);

#endif
