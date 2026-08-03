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
    memset(cpu->r8_12_bank, 0, sizeof(cpu->r8_12_bank));
    /* Real ARM7TDMI reset always enters Supervisor mode, ARM state.
       IRQ and FIQ are disabled. This holds for any SoC built around
       the core. */
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
        /* Pipeline effect: reading PC as an operand does not return the
           raw fetch-loop PC value.
           It returns the current instruction's address plus 8 in ARM
           state, or plus 4 in Thumb state. */
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
    int old_fiq = (old_bank == ARM_BANK_FIQ);
    int new_fiq = (new_bank == ARM_BANK_FIQ);

    cpu->r13_bank[old_bank] = cpu->r[13];
    cpu->r14_bank[old_bank] = cpu->r[14];
    cpu->r[13] = cpu->r13_bank[new_bank];
    cpu->r[14] = cpu->r14_bank[new_bank];

    /* r8-r12 swap only when crossing the FIQ boundary: every non-FIQ mode shares one copy of them, so a
       SVC-to-IRQ switch must leave them alone. See r8_12_bank in cpu.h. */
    if (old_fiq != new_fiq) {
        int i;
        for (i = 0; i < 5; i++) {
            cpu->r8_12_bank[old_fiq][i] = cpu->r[8 + i];
            cpu->r[8 + i] = cpu->r8_12_bank[new_fiq][i];
        }
    }
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

void arm7tdmi_add_cycles(arm7tdmi_t *cpu, uint32_t n) {
    cpu->bus->pending_cycles += n;
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

#ifdef PSEMU_TRACE_HOOKS
/* See psemu_exec_trace_cb in cpu.h. Same compile-out rule as memory.c's bus
   hooks: only the psemu_trace target defines PSEMU_TRACE_HOOKS. */
void (*psemu_exec_trace_cb)(uint32_t pc, uint32_t cpsr) = NULL;
#endif

uint32_t arm7tdmi_step(arm7tdmi_t *cpu) {
    psemu_debug_current_pc = cpu->r[15];
    /* A confirmed bug, found via a desktop-app crash report.
       This check used to gate only on `halted`, which nothing ever sets.
       Once `unimplemented` tripped, a caller that does not check it every
       step kept fetching and executing from whatever address the CPU
       landed on. psemu_run's cycle-budget loop has this gap;
       tools/inspect.c's own per-instruction loop does not.
       This let the CPU run thousands of instructions past the real fault
       site before the caller noticed. Register state and the executed-PC
       trace below then reflected wherever the CPU wandered off to, not
       the original fault. This actively misled the diagnostics
       psemu_write_crash_report exists to provide.
       Fixed by gating on both flags. Once either flag is set, nothing
       well-defined is left to do, so this function stops advancing
       everything, including the trace, from here on. */
    if (cpu->halted || cpu->unimplemented) {
        return 1;
    }
#ifdef PSEMU_TRACE_HOOKS
    if (psemu_exec_trace_cb) {
        psemu_exec_trace_cb(cpu->r[15], cpu->cpsr);
    }
#endif
    cpu->trace[cpu->trace_pos % PSEMU_TRACE_SIZE].pc = cpu->r[15];
    cpu->trace[cpu->trace_pos % PSEMU_TRACE_SIZE].cpsr = cpu->cpsr;
    cpu->trace_pos++;
    cpu->total_steps++;
    cpu->bus->pending_cycles = 0;
    /* A confirmed bug, fixed this session. See docs/hardware-notes.md,
       "Interrupt controller".
       This emulator never delivered FIQ, for any app. intc_fiq_asserted
       (intc.c) already existed and was confirmed correct against real
       hardware's bit-mapping (INT_FIQ_MASK, Timer2/COM). Nothing here
       ever called it.
       Real ARM7TDMI checks FIQ before IRQ: FIQ has strictly higher
       priority in the exception scheme. FIQ is also level-triggered,
       like IRQ: the interrupt controller's hold & enable & INT_FIQ_MASK,
       not a one-shot request. This code polls it live every step,
       mirroring the IRQ check below. */
    if (!(cpu->cpsr & CPSR_F) && intc_fiq_asserted(cpu->bus->intc)) {
        /* Return address follows the same "SUBS PC, LR, #4" handler-exit
           convention IRQ uses.
           LR_fiq = address of the next instruction + 4. */
        arm_enter_exception(cpu, ARM_MODE_FIQ, ARM_FIQ_VECTOR, cpu->r[15] + 4u);
        /* Real ARM7TDMI sets both F and I on FIQ entry.
           IRQ, SWI, and abort entry set only I.
           Setting F blocks a second FIQ from interrupting the handler
           before it saves state.
           arm_enter_exception is shared by every exception type and sets
           only I, so this code sets F here instead. */
        cpu->cpsr |= CPSR_F;
        /* Exception entry is a pipeline refill: two ARM-state fetches at
           the vector. Same "2S+1N" shape as SWI/branches. See
           docs/hardware-notes.md, "Memory access timing". */
        arm7tdmi_add_cycles(cpu, 2u * psemu_region_fetch_cycles(ARM_FIQ_VECTOR, 0));
        return cpu->bus->pending_cycles;
    }
    /* IRQ is level-triggered on real hardware: the interrupt controller's
       hold & enable & INT_IRQ_MASK, not a one-shot request.
       This code polls it live every step, rather than latching a
       "pending" flag. This lets the CPU keep re-entering the handler for
       as long as the line stays asserted. */
    if (!(cpu->cpsr & CPSR_I) && intc_irq_asserted(cpu->bus->intc)) {
        /* Return address follows the "SUBS PC, LR, #4" handler-exit
           convention: LR_irq = address of the next instruction + 4. */
        arm_enter_exception(cpu, ARM_MODE_IRQ, ARM_IRQ_VECTOR, cpu->r[15] + 4u);
        arm7tdmi_add_cycles(cpu, 2u * psemu_region_fetch_cycles(ARM_IRQ_VECTOR, 0));
        return cpu->bus->pending_cycles;
    }
    uint32_t pc = cpu->r[15];
    if (cpu->cpsr & CPSR_T) {
        uint16_t instr = psemu_bus_fetch16(cpu->bus, pc & ~1u);
        cpu->r[15] = pc + 2u;
        thumb_execute(cpu, instr, pc);
    } else {
        uint32_t instr = psemu_bus_fetch32(cpu->bus, pc & ~3u);
        cpu->r[15] = pc + 4u;
        arm_execute(cpu, instr, pc);
    }
    return cpu->bus->pending_cycles;
}
