#include "cpu.h"

#include <string.h>

#include "intc.h"

static int bank_index(uint32_t mode) {
    switch (mode & CPSR_MODE_MASK) {
    case ARM_MODE_FIQ:
        return 0;
    case ARM_MODE_IRQ:
        return 1;
    case ARM_MODE_SVC:
        return 2;
    case ARM_MODE_ABT:
        return 3;
    case ARM_MODE_UND:
        return 4;
    default: /* USR, SYS */
        return 5;
    }
}

void arm7tdmi_init(arm7tdmi_t *cpu, psemu_bus_t *bus) {
    cpu->bus = bus;
    arm7tdmi_reset(cpu, 0);
}

void arm7tdmi_reset(arm7tdmi_t *cpu, uint32_t reset_vector) {
    memset(cpu->r, 0, sizeof(cpu->r));
    memset(cpu->r13_bank, 0, sizeof(cpu->r13_bank));
    memset(cpu->r14_bank, 0, sizeof(cpu->r14_bank));
    memset(cpu->spsr_bank, 0, sizeof(cpu->spsr_bank));
    /* Real ARM7TDMI reset always enters Supervisor mode, ARM state, with
       IRQ/FIQ disabled - this holds regardless of what any specific SoC
       built around the core does. */
    cpu->cpsr = ARM_MODE_SVC | CPSR_I | CPSR_F;
    cpu->r[15] = reset_vector;
    cpu->halted = 0;
    cpu->unimplemented = 0;
    memset(cpu->trace, 0, sizeof(cpu->trace));
    cpu->trace_pos = 0;
    cpu->total_steps = 0;
}

int arm_condition_passed(arm7tdmi_t *cpu, uint32_t cond) {
    int n = (cpu->cpsr & CPSR_N) != 0;
    int z = (cpu->cpsr & CPSR_Z) != 0;
    int c = (cpu->cpsr & CPSR_C) != 0;
    int v = (cpu->cpsr & CPSR_V) != 0;
    switch (cond & 0xFu) {
    case 0x0:
        return z; /* EQ */
    case 0x1:
        return !z; /* NE */
    case 0x2:
        return c; /* CS/HS */
    case 0x3:
        return !c; /* CC/LO */
    case 0x4:
        return n; /* MI */
    case 0x5:
        return !n; /* PL */
    case 0x6:
        return v; /* VS */
    case 0x7:
        return !v; /* VC */
    case 0x8:
        return c && !z; /* HI */
    case 0x9:
        return !c || z; /* LS */
    case 0xA:
        return n == v; /* GE */
    case 0xB:
        return n != v; /* LT */
    case 0xC:
        return (n == v) && !z; /* GT */
    case 0xD:
        return (n != v) || z; /* LE */
    case 0xE:
        return 1; /* AL */
    default:
        return 0; /* NV - reserved/never on ARMv4T */
    }
}

uint32_t arm_read_reg(arm7tdmi_t *cpu, int n, uint32_t pc, int thumb) {
    if (n == 15) {
        /* Pipeline effect: reading PC as an operand yields the address of
           the current instruction plus 8 (ARM) or 4 (Thumb), not the raw
           fetch-loop PC value. */
        return pc + (thumb ? 4u : 8u);
    }
    return cpu->r[n];
}

void arm_write_reg(arm7tdmi_t *cpu, int n, uint32_t value) {
    cpu->r[n] = value;
}

void arm_set_mode(arm7tdmi_t *cpu, uint32_t new_mode) {
    uint32_t old_mode = cpu->cpsr & CPSR_MODE_MASK;
    int old_bank = bank_index(old_mode);
    int new_bank = bank_index(new_mode);
    cpu->r13_bank[old_bank] = cpu->r[13];
    cpu->r14_bank[old_bank] = cpu->r[14];
    cpu->r[13] = cpu->r13_bank[new_bank];
    cpu->r[14] = cpu->r14_bank[new_bank];
    cpu->cpsr = (cpu->cpsr & ~CPSR_MODE_MASK) | (new_mode & CPSR_MODE_MASK);
}

int arm_current_bank(arm7tdmi_t *cpu) {
    return bank_index(cpu->cpsr);
}

void arm_set_nz(arm7tdmi_t *cpu, uint32_t result) {
    cpu->cpsr = (cpu->cpsr & ~(CPSR_N | CPSR_Z)) | ((result & 0x80000000u) ? CPSR_N : 0u) | (result == 0u ? CPSR_Z : 0u);
}

uint32_t arm_adc_raw(uint32_t a, uint32_t b, uint32_t carry_in, int *carry_out, int *overflow) {
    uint64_t sum = (uint64_t)a + (uint64_t)b + carry_in;
    uint32_t result = (uint32_t)sum;
    *carry_out = (int)((sum >> 32) & 1u);
    *overflow = (((a ^ b) & 0x80000000u) == 0u) && (((a ^ result) & 0x80000000u) != 0u);
    return result;
}

arm_shift_result_t arm_apply_shift(uint32_t value, int shift_type, uint32_t amount, int carry_in, int is_immediate_encoding) {
    arm_shift_result_t r;
    r.value = value;
    r.carry = carry_in;

    switch (shift_type & 0x3) {
    case 0: /* LSL */
        if (amount == 0) {
            /* unchanged */
        } else if (amount < 32) {
            r.carry = (int)((value >> (32 - amount)) & 1u);
            r.value = value << amount;
        } else if (amount == 32) {
            r.carry = (int)(value & 1u);
            r.value = 0;
        } else {
            r.carry = 0;
            r.value = 0;
        }
        break;

    case 1: /* LSR */
        if (amount == 0) {
            if (is_immediate_encoding) { /* LSR #0 means LSR #32 */
                r.carry = (int)((value >> 31) & 1u);
                r.value = 0;
            }
        } else if (amount < 32) {
            r.carry = (int)((value >> (amount - 1)) & 1u);
            r.value = value >> amount;
        } else if (amount == 32) {
            r.carry = (int)((value >> 31) & 1u);
            r.value = 0;
        } else {
            r.carry = 0;
            r.value = 0;
        }
        break;

    case 2: /* ASR */
        if (amount == 0 && is_immediate_encoding) {
            amount = 32; /* ASR #0 means ASR #32 */
        }
        if (amount == 0) {
            /* unchanged (register-specified amount of 0) */
        } else if (amount >= 32) {
            r.value = (value & 0x80000000u) ? 0xFFFFFFFFu : 0u;
            r.carry = (int)((value >> 31) & 1u);
        } else {
            r.carry = (int)((value >> (amount - 1)) & 1u);
            r.value = (uint32_t)((int32_t)value >> amount);
        }
        break;

    case 3: /* ROR */
        if (amount == 0) {
            if (is_immediate_encoding) { /* ROR #0 means RRX */
                r.carry = (int)(value & 1u);
                r.value = (value >> 1) | ((uint32_t)carry_in << 31);
            }
        } else {
            uint32_t rot = amount & 31u;
            if (rot == 0) {
                r.carry = (int)((value >> 31) & 1u);
            } else {
                r.carry = (int)((value >> (rot - 1)) & 1u);
                r.value = (value >> rot) | (value << (32 - rot));
            }
        }
        break;

    default:
        break;
    }

    return r;
}

void arm_enter_exception(arm7tdmi_t *cpu, uint32_t mode, uint32_t vector, uint32_t return_addr) {
    uint32_t old_cpsr = cpu->cpsr;
    arm_set_mode(cpu, mode);
    cpu->spsr_bank[arm_current_bank(cpu)] = old_cpsr;
    cpu->r[14] = return_addr;
    cpu->cpsr |= CPSR_I;
    cpu->cpsr &= ~CPSR_T;
    cpu->r[15] = vector;
}

uint32_t psemu_debug_current_pc = 0;

uint32_t arm7tdmi_step(arm7tdmi_t *cpu) {
    psemu_debug_current_pc = cpu->r[15];
    /* A real, confirmed bug found via a desktop-app crash report: this
       used to only gate on `halted` (which nothing ever sets), so once
       `unimplemented` tripped, callers that don't check it every single
       step (psemu_run's whole cycle-budget loop, unlike
       tools/inspect.c's own per-instruction loop) kept right on
       fetching and executing from whatever nonsense address the CPU
       landed on - potentially thousands of instructions past the real
       fault site before the caller ever noticed. That left register
       state, and the executed-PC trace below, reflecting wherever the
       CPU wandered off to afterward rather than the original fault -
       actively misleading for exactly the diagnostics
       psemu_write_crash_report exists to provide. Once either flag is
       set there is nothing well-defined left to do, so stop advancing
       anything (trace included) from here on. */
    if (cpu->halted || cpu->unimplemented) {
        return 1;
    }
    cpu->trace[cpu->trace_pos % PSEMU_TRACE_SIZE].pc = cpu->r[15];
    cpu->trace[cpu->trace_pos % PSEMU_TRACE_SIZE].cpsr = cpu->cpsr;
    cpu->trace_pos++;
    cpu->total_steps++;
    /* A REAL, CONFIRMED BUG this session (see docs/hardware-notes.md,
       "Interrupt controller"): FIQ was never actually delivered
       by this emulator, for any app, ever - intc_fiq_asserted (intc.c)
       has always existed and was already verified correct against real
       hardware's bit-mapping (INT_FIQ_MASK, Timer2/COM), but nothing
       here ever called it. Real ARM7TDMI checks FIQ before IRQ (FIQ has
       strictly higher priority in the exception scheme), and it too is
       level-triggered (the interrupt controller's hold & enable &
       INT_FIQ_MASK, not a one-shot request) - poll it live every step,
       mirroring the IRQ check below exactly. */
    if (!(cpu->cpsr & CPSR_F) && intc_fiq_asserted(cpu->bus->intc)) {
        /* Return address follows the same "SUBS PC, LR, #4" handler-exit
           convention IRQ uses: LR_fiq = address of the next instruction + 4. */
        arm_enter_exception(cpu, ARM_MODE_FIQ, ARM_FIQ_VECTOR, cpu->r[15] + 4u);
        /* Real ARM7TDMI additionally sets F (not just I) on FIQ entry -
           unlike IRQ/SWI/aborts, which only ever disable IRQ - to prevent
           a second FIQ from recursively interrupting the handler before
           it's had a chance to save state. arm_enter_exception is shared
           by every exception type and only sets I, so this one extra bit
           is set here instead of generically. */
        cpu->cpsr |= CPSR_F;
        return 1;
    }
    /* IRQ is level-triggered on real hardware (the interrupt controller's
       hold & enable & INT_IRQ_MASK, not a one-shot request) - poll it live
       every step rather than latching a "pending" flag, so the CPU keeps
       re-entering the handler for as long as the line stays asserted. */
    if (!(cpu->cpsr & CPSR_I) && intc_irq_asserted(cpu->bus->intc)) {
        /* Return address follows the "SUBS PC, LR, #4" handler-exit
           convention: LR_irq = address of the next instruction + 4. */
        arm_enter_exception(cpu, ARM_MODE_IRQ, ARM_IRQ_VECTOR, cpu->r[15] + 4u);
        return 1;
    }
    uint32_t pc = cpu->r[15];
    if (cpu->cpsr & CPSR_T) {
        uint16_t instr = psemu_bus_read16(cpu->bus, pc & ~1u);
        cpu->r[15] = pc + 2u;
        thumb_execute(cpu, instr, pc);
    } else {
        uint32_t instr = psemu_bus_read32(cpu->bus, pc & ~3u);
        cpu->r[15] = pc + 4u;
        arm_execute(cpu, instr, pc);
    }
    return 1;
}
