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

/* Bank slots: 0=fiq, 1=irq, 2=svc, 3=abt, 4=und, 5=usr/sys (shared, per spec). */
#define ARM_BANK_COUNT 6

typedef struct {
    uint32_t r[16]; /* r0-r14 general purpose (banked view for current mode), r15 = pc */
    uint32_t cpsr;

    uint32_t r13_bank[ARM_BANK_COUNT];
    uint32_t r14_bank[ARM_BANK_COUNT];
    uint32_t spsr_bank[ARM_BANK_COUNT];

    psemu_bus_t *bus;
    int halted;
    int unimplemented; /* set when an unrecognized opcode is hit; sticky until cleared by caller */
} arm7tdmi_t;

typedef struct {
    uint32_t value;
    int carry;
} arm_shift_result_t;

void arm7tdmi_init(arm7tdmi_t *cpu, psemu_bus_t *bus);
void arm7tdmi_reset(arm7tdmi_t *cpu, uint32_t reset_vector);

/* Executes one instruction, returns cycles consumed (approximate: 1 per instruction for now). */
uint32_t arm7tdmi_step(arm7tdmi_t *cpu);

/* TEMPORARY diagnostic hook (see intc.c's psemu_intc_trace_enabled): the PC
   of the instruction currently executing, updated at the top of every
   arm7tdmi_step. Lets intc_read8/write8 log real accesses with their real
   PC and CPU mode, since a real BIOS mixes ARM/Thumb code in a way that
   defeats static disassembly without mode-tracking. Remove once the
   button-input investigation in docs/hardware-notes.md is resolved. */
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

void arm_execute(arm7tdmi_t *cpu, uint32_t instr, uint32_t pc);
void thumb_execute(arm7tdmi_t *cpu, uint16_t instr, uint32_t pc);

#endif
