#include "cpu.h"

#define SWI_VECTOR 0x08u

static uint32_t decode_operand2(arm7tdmi_t *cpu, uint32_t instr, uint32_t pc, int *carry_out) {
    int carry_in = (cpu->cpsr & CPSR_C) != 0;

    if (instr & (1u << 25)) { /* immediate: ROR(imm8, rotate*2) */
        uint32_t imm8 = instr & 0xFFu;
        uint32_t rot = ((instr >> 8) & 0xFu) * 2u;
        uint32_t value = (rot == 0) ? imm8 : ((imm8 >> rot) | (imm8 << (32 - rot)));
        *carry_out = (rot == 0) ? carry_in : (int)((value >> 31) & 1u);
        return value;
    }

    int rm = instr & 0xF;
    int shift_type = (int)((instr >> 5) & 0x3u);
    uint32_t rm_value = arm_read_reg(cpu, rm, pc, 0);
    uint32_t amount;
    int is_immediate_encoding;

    if (instr & (1u << 4)) { /* shift amount from register (low byte) */
        int rs = (int)((instr >> 8) & 0xFu);
        amount = arm_read_reg(cpu, rs, pc, 0) & 0xFFu;
        is_immediate_encoding = 0;
    } else {
        amount = (instr >> 7) & 0x1Fu;
        is_immediate_encoding = 1;
    }

    arm_shift_result_t sr = arm_apply_shift(rm_value, shift_type, amount, carry_in, is_immediate_encoding);
    *carry_out = sr.carry;
    return sr.value;
}

static void exec_data_processing(arm7tdmi_t *cpu, uint32_t instr, uint32_t pc) {
    uint32_t opcode = (instr >> 21) & 0xFu;
    int set_flags = (int)((instr >> 20) & 1u);
    int rn = (int)((instr >> 16) & 0xFu);
    int rd = (int)((instr >> 12) & 0xFu);

    int shifter_carry;
    uint32_t op2 = decode_operand2(cpu, instr, pc, &shifter_carry);
    uint32_t op1 = arm_read_reg(cpu, rn, pc, 0);
    uint32_t c_in = (cpu->cpsr & CPSR_C) ? 1u : 0u;

    uint32_t result = 0;
    int write_result = 1;
    int carry = shifter_carry;
    int overflow = (cpu->cpsr & CPSR_V) != 0;

    switch (opcode) {
    case 0x0:
        result = op1 & op2;
        break; /* AND */
    case 0x1:
        result = op1 ^ op2;
        break; /* EOR */
    case 0x2:
        result = arm_adc_raw(op1, ~op2, 1, &carry, &overflow);
        break; /* SUB */
    case 0x3:
        result = arm_adc_raw(op2, ~op1, 1, &carry, &overflow);
        break; /* RSB */
    case 0x4:
        result = arm_adc_raw(op1, op2, 0, &carry, &overflow);
        break; /* ADD */
    case 0x5:
        result = arm_adc_raw(op1, op2, c_in, &carry, &overflow);
        break; /* ADC */
    case 0x6:
        result = arm_adc_raw(op1, ~op2, c_in, &carry, &overflow);
        break; /* SBC */
    case 0x7:
        result = arm_adc_raw(op2, ~op1, c_in, &carry, &overflow);
        break; /* RSC */
    case 0x8:
        result = op1 & op2;
        write_result = 0;
        break; /* TST */
    case 0x9:
        result = op1 ^ op2;
        write_result = 0;
        break; /* TEQ */
    case 0xA:
        result = arm_adc_raw(op1, ~op2, 1, &carry, &overflow);
        write_result = 0;
        break; /* CMP */
    case 0xB:
        result = arm_adc_raw(op1, op2, 0, &carry, &overflow);
        write_result = 0;
        break; /* CMN */
    case 0xC:
        result = op1 | op2;
        break; /* ORR */
    case 0xD:
        result = op2;
        break; /* MOV */
    case 0xE:
        result = op1 & ~op2;
        break; /* BIC */
    default:
        result = ~op2;
        break; /* MVN */
    }

    if (set_flags) {
        uint32_t mode = cpu->cpsr & CPSR_MODE_MASK;
        if (rd == 15 && mode != ARM_MODE_USR && mode != ARM_MODE_SYS) {
            /* "MOVS/SUBS PC, ..." in a privileged mode is the standard
               exception-return idiom: restore the whole CPSR (mode, I/F/T,
               NZCV) from this mode's SPSR instead of just updating flags. */
            uint32_t spsr = cpu->spsr_bank[arm_current_bank(cpu)];
            arm_set_mode(cpu, spsr & CPSR_MODE_MASK);
            cpu->cpsr = spsr;
        } else {
            arm_set_nz(cpu, result);
            cpu->cpsr = (cpu->cpsr & ~(CPSR_C | CPSR_V)) | (carry ? CPSR_C : 0u) | (overflow ? CPSR_V : 0u);
        }
    }
    if (write_result) {
        arm_write_reg(cpu, rd, result);
    }
}

static void exec_multiply(arm7tdmi_t *cpu, uint32_t instr) {
    int rd = (int)((instr >> 16) & 0xFu);
    int rn = (int)((instr >> 12) & 0xFu); /* accumulate operand */
    int rs = (int)((instr >> 8) & 0xFu);
    int rm = (int)(instr & 0xFu);
    int accumulate = (int)((instr >> 21) & 1u);
    int set_flags = (int)((instr >> 20) & 1u);

    uint32_t result = cpu->r[rm] * cpu->r[rs];
    if (accumulate) {
        result += cpu->r[rn];
    }
    cpu->r[rd] = result;
    if (set_flags) {
        arm_set_nz(cpu, result);
    }
}

static void exec_long_multiply(arm7tdmi_t *cpu, uint32_t instr) {
    int rdhi = (int)((instr >> 16) & 0xFu);
    int rdlo = (int)((instr >> 12) & 0xFu);
    int rs = (int)((instr >> 8) & 0xFu);
    int rm = (int)(instr & 0xFu);
    int is_signed = (int)((instr >> 22) & 1u);
    int accumulate = (int)((instr >> 21) & 1u);
    int set_flags = (int)((instr >> 20) & 1u);
    uint64_t result;

    if (is_signed) {
        result = (uint64_t)((int64_t)(int32_t)cpu->r[rm] * (int64_t)(int32_t)cpu->r[rs]);
    } else {
        result = (uint64_t)cpu->r[rm] * (uint64_t)cpu->r[rs];
    }
    if (accumulate) {
        result += ((uint64_t)cpu->r[rdhi] << 32) | (uint64_t)cpu->r[rdlo];
    }
    cpu->r[rdlo] = (uint32_t)result;
    cpu->r[rdhi] = (uint32_t)(result >> 32);
    if (set_flags) {
        cpu->cpsr = (cpu->cpsr & ~(CPSR_N | CPSR_Z)) | ((result & 0x8000000000000000ull) ? CPSR_N : 0u) |
                    (result == 0 ? CPSR_Z : 0u);
    }
}

static void exec_swap(arm7tdmi_t *cpu, uint32_t instr, uint32_t pc) {
    int byte = (int)((instr >> 22) & 1u);
    int rn = (int)((instr >> 16) & 0xFu);
    int rd = (int)((instr >> 12) & 0xFu);
    int rm = (int)(instr & 0xFu);
    uint32_t addr = arm_read_reg(cpu, rn, pc, 0);

    if (byte) {
        uint8_t old = psemu_bus_read8(cpu->bus, addr);
        psemu_bus_write8(cpu->bus, addr, (uint8_t)cpu->r[rm]);
        cpu->r[rd] = old;
    } else {
        uint32_t old = psemu_bus_read32(cpu->bus, addr & ~3u);
        psemu_bus_write32(cpu->bus, addr & ~3u, cpu->r[rm]);
        cpu->r[rd] = old;
    }
}

static void exec_bx(arm7tdmi_t *cpu, uint32_t instr, uint32_t pc) {
    int rm = (int)(instr & 0xFu);
    uint32_t target = arm_read_reg(cpu, rm, pc, 0);
    if (target & 1u) {
        cpu->cpsr |= CPSR_T;
        target &= ~1u;
    } else {
        cpu->cpsr &= ~CPSR_T;
        target &= ~3u;
    }
    cpu->r[15] = target;
}

static void exec_mrs(arm7tdmi_t *cpu, uint32_t instr) {
    int rd = (int)((instr >> 12) & 0xFu);
    int from_spsr = (int)((instr >> 22) & 1u);
    cpu->r[rd] = from_spsr ? cpu->spsr_bank[arm_current_bank(cpu)] : cpu->cpsr;
}

static void exec_msr(arm7tdmi_t *cpu, uint32_t instr, uint32_t pc) {
    int immediate = (int)((instr >> 25) & 1u);
    int to_spsr = (int)((instr >> 22) & 1u);
    uint32_t field_mask = (instr >> 16) & 0xFu;
    uint32_t value;

    if (immediate) {
        uint32_t imm8 = instr & 0xFFu;
        uint32_t rot = ((instr >> 8) & 0xFu) * 2u;
        value = (rot == 0) ? imm8 : ((imm8 >> rot) | (imm8 << (32 - rot)));
    } else {
        value = arm_read_reg(cpu, (int)(instr & 0xFu), pc, 0);
    }

    uint32_t byte_mask = 0;
    if (field_mask & 0x1u) byte_mask |= 0x000000FFu; /* control: mode, I, F, T */
    if (field_mask & 0x2u) byte_mask |= 0x0000FF00u; /* unused on ARMv4T */
    if (field_mask & 0x4u) byte_mask |= 0x00FF0000u; /* unused on ARMv4T */
    if (field_mask & 0x8u) byte_mask |= 0xFF000000u; /* flags: N,Z,C,V */

    if (to_spsr) {
        uint32_t *spsr = &cpu->spsr_bank[arm_current_bank(cpu)];
        *spsr = (*spsr & ~byte_mask) | (value & byte_mask);
        return;
    }

    if ((byte_mask & 0xFFu) && (value & CPSR_MODE_MASK) != (cpu->cpsr & CPSR_MODE_MASK)) {
        arm_set_mode(cpu, value & CPSR_MODE_MASK); /* re-banks r13/r14 for the new mode */
    }
    cpu->cpsr = (cpu->cpsr & ~byte_mask) | (value & byte_mask);
}

static void exec_single_transfer(arm7tdmi_t *cpu, uint32_t instr, uint32_t pc) {
    int immediate = !(instr & (1u << 25));
    int pre_index = (int)((instr >> 24) & 1u);
    int up = (int)((instr >> 23) & 1u);
    int byte = (int)((instr >> 22) & 1u);
    int writeback = (int)((instr >> 21) & 1u);
    int load = (int)((instr >> 20) & 1u);
    int rn = (int)((instr >> 16) & 0xFu);
    int rd = (int)((instr >> 12) & 0xFu);

    uint32_t offset;
    if (immediate) {
        offset = instr & 0xFFFu;
    } else {
        int shift_carry;
        offset = decode_operand2(cpu, instr & ~(1u << 25), pc, &shift_carry);
    }

    uint32_t base = arm_read_reg(cpu, rn, pc, 0);
    uint32_t address = pre_index ? (up ? base + offset : base - offset) : base;

    if (load) {
        uint32_t value;
        if (byte) {
            value = psemu_bus_read8(cpu->bus, address);
        } else {
            value = psemu_bus_read32(cpu->bus, address & ~3u);
            if (address & 3u) {
                uint32_t rot = (address & 3u) * 8u;
                value = (value >> rot) | (value << (32 - rot));
            }
        }
        arm_write_reg(cpu, rd, value);
    } else {
        uint32_t value = arm_read_reg(cpu, rd, pc, 0);
        if (byte) {
            psemu_bus_write8(cpu->bus, address, (uint8_t)value);
        } else {
            psemu_bus_write32(cpu->bus, address & ~3u, value);
        }
    }

    uint32_t final_address = pre_index ? address : (up ? base + offset : base - offset);
    if ((!pre_index || writeback) && rn != 15) {
        cpu->r[rn] = final_address;
    }
}

static void exec_halfword_transfer(arm7tdmi_t *cpu, uint32_t instr, uint32_t pc) {
    int pre_index = (int)((instr >> 24) & 1u);
    int up = (int)((instr >> 23) & 1u);
    int imm_offset = (int)((instr >> 22) & 1u);
    int writeback = (int)((instr >> 21) & 1u);
    int load = (int)((instr >> 20) & 1u);
    int rn = (int)((instr >> 16) & 0xFu);
    int rd = (int)((instr >> 12) & 0xFu);
    int sh = (int)((instr >> 5) & 0x3u);

    uint32_t offset =
        imm_offset ? (((instr >> 8) & 0xFu) << 4) | (instr & 0xFu) : cpu->r[instr & 0xFu];

    uint32_t base = arm_read_reg(cpu, rn, pc, 0);
    uint32_t address = pre_index ? (up ? base + offset : base - offset) : base;

    if (load) {
        uint32_t value;
        switch (sh) {
        case 1:
            value = psemu_bus_read16(cpu->bus, address & ~1u);
            break; /* LDRH */
        case 2: {
            int8_t b = (int8_t)psemu_bus_read8(cpu->bus, address);
            value = (uint32_t)(int32_t)b;
            break; /* LDRSB */
        }
        case 3: {
            int16_t h = (int16_t)psemu_bus_read16(cpu->bus, address & ~1u);
            value = (uint32_t)(int32_t)h;
            break; /* LDRSH */
        }
        default:
            value = 0;
            break;
        }
        arm_write_reg(cpu, rd, value);
    } else {
        uint32_t value = arm_read_reg(cpu, rd, pc, 0);
        psemu_bus_write16(cpu->bus, address & ~1u, (uint16_t)value); /* STRH */
    }

    uint32_t final_address = pre_index ? address : (up ? base + offset : base - offset);
    if ((!pre_index || writeback) && rn != 15) {
        cpu->r[rn] = final_address;
    }
}

static void exec_block_transfer(arm7tdmi_t *cpu, uint32_t instr, uint32_t pc) {
    int pre_index = (int)((instr >> 24) & 1u);
    int up = (int)((instr >> 23) & 1u);
    int writeback = (int)((instr >> 21) & 1u);
    int load = (int)((instr >> 20) & 1u);
    int rn = (int)((instr >> 16) & 0xFu);
    uint32_t reg_list = instr & 0xFFFFu;

    uint32_t base = cpu->r[rn];
    int count = 0;
    for (int i = 0; i < 16; i++) {
        if (reg_list & (1u << i)) {
            count++;
        }
    }
    if (count == 0) {
        return; /* empty register list is architecturally unpredictable */
    }

    uint32_t addr = base;
    int step = up ? 4 : -4;
    for (int i = 0; i < 16; i++) {
        int reg = up ? i : (15 - i); /* keeps lowest register at the lowest address either way */
        if (!(reg_list & (1u << reg))) {
            continue;
        }
        if (pre_index) {
            addr = (uint32_t)((int32_t)addr + step);
        }
        if (load) {
            uint32_t value = psemu_bus_read32(cpu->bus, addr & ~3u);
            cpu->r[reg] = value;
        } else {
            uint32_t value = (reg == 15) ? arm_read_reg(cpu, 15, pc, 0) : cpu->r[reg];
            psemu_bus_write32(cpu->bus, addr & ~3u, value);
        }
        if (!pre_index) {
            addr = (uint32_t)((int32_t)addr + step);
        }
    }

    if (writeback && rn != 15) {
        cpu->r[rn] = up ? base + (uint32_t)count * 4u : base - (uint32_t)count * 4u;
    }
}

static void exec_branch(arm7tdmi_t *cpu, uint32_t instr, uint32_t pc) {
    int link = (int)((instr >> 24) & 1u);
    int32_t offset = (int32_t)((instr & 0xFFFFFFu) << 8) >> 6; /* sign-extend 24-bit field, x4 */
    uint32_t target = (pc + 8u) + (uint32_t)offset;
    if (link) {
        cpu->r[14] = pc + 4u;
    }
    cpu->r[15] = target;
}

void arm_execute(arm7tdmi_t *cpu, uint32_t instr, uint32_t pc) {
    uint32_t cond = instr >> 28;
    if (!arm_condition_passed(cpu, cond)) {
        return;
    }

    if ((instr & 0x0FFFFFF0u) == 0x012FFF10u) {
        exec_bx(cpu, instr, pc);
        return;
    }
    if ((instr & 0x0FC000F0u) == 0x00000090u) {
        exec_multiply(cpu, instr);
        return;
    }
    if ((instr & 0x0F8000F0u) == 0x00800090u) {
        exec_long_multiply(cpu, instr);
        return;
    }
    if ((instr & 0x0FB000F0u) == 0x01000090u) {
        exec_swap(cpu, instr, pc);
        return;
    }
    if ((instr & 0x0E000090u) == 0x00000090u && ((instr >> 5) & 0x3u) != 0) {
        exec_halfword_transfer(cpu, instr, pc);
        return;
    }
    if ((instr & 0x0FBF0FFFu) == 0x010F0000u) {
        exec_mrs(cpu, instr);
        return;
    }
    if ((instr & 0x0DB0F000u) == 0x0120F000u) {
        exec_msr(cpu, instr, pc);
        return;
    }
    if ((instr & 0x0C000000u) == 0x00000000u) {
        exec_data_processing(cpu, instr, pc);
        return;
    }
    if ((instr & 0x0E000000u) == 0x04000000u) {
        exec_single_transfer(cpu, instr, pc);
        return;
    }
    if ((instr & 0x0E000000u) == 0x08000000u) {
        exec_block_transfer(cpu, instr, pc);
        return;
    }
    if ((instr & 0x0E000000u) == 0x0A000000u) {
        exec_branch(cpu, instr, pc);
        return;
    }
    if ((instr & 0x0F000000u) == 0x0F000000u) {
        arm_enter_exception(cpu, ARM_MODE_SVC, SWI_VECTOR, pc + 4u);
        return;
    }
    cpu->unimplemented = 1;
}
