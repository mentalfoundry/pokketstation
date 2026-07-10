#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "psemu_internal.h"

static void put32(psemu_t *ps, uint32_t addr, uint32_t value) {
    ps->bus.ram[addr + 0] = (uint8_t)(value);
    ps->bus.ram[addr + 1] = (uint8_t)(value >> 8);
    ps->bus.ram[addr + 2] = (uint8_t)(value >> 16);
    ps->bus.ram[addr + 3] = (uint8_t)(value >> 24);
}

static void put16(psemu_t *ps, uint32_t addr, uint16_t value) {
    ps->bus.ram[addr + 0] = (uint8_t)(value);
    ps->bus.ram[addr + 1] = (uint8_t)(value >> 8);
}

static psemu_t *make_arm_cpu(void) {
    psemu_t *ps = psemu_create();
    arm7tdmi_reset(&ps->cpu, 0);
    return ps;
}

static psemu_t *make_thumb_cpu(void) {
    psemu_t *ps = psemu_create();
    arm7tdmi_reset(&ps->cpu, 0);
    ps->cpu.cpsr |= CPSR_T;
    return ps;
}

static void test_arm_data_processing(void) {
    psemu_t *ps = make_arm_cpu();

    put32(ps, 0, 0xE3A00005u); /* MOV R0, #5 (well-known canonical encoding) */
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[0] == 5);

    /* ADD R1, R0, #10, S=1 */
    uint32_t add_r1 = (0xEu << 28) | (1u << 25) | (0x4u << 21) | (1u << 20) | (0u << 16) | (1u << 12) | 10u;
    put32(ps, 4, add_r1);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[1] == 15);
    assert(!(ps->cpu.cpsr & CPSR_Z));
    assert(!(ps->cpu.cpsr & CPSR_N));

    /* CMP R1, #15, S implied set for CMP */
    uint32_t cmp_r1 = (0xEu << 28) | (1u << 25) | (0xAu << 21) | (1u << 20) | (1u << 16) | (0u << 12) | 15u;
    put32(ps, 8, cmp_r1);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.cpsr & CPSR_Z);
    assert(ps->cpu.cpsr & CPSR_C); /* no borrow -> carry set */

    /* SUB R2, R1, R0 (register operand2, LSL #0) */
    uint32_t sub_r2 = (0xEu << 28) | (0u << 25) | (0x2u << 21) | (0u << 20) | (1u << 16) | (2u << 12) | 0u;
    put32(ps, 12, sub_r2);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[2] == 10); /* 15 - 5 */

    /* MOV R3, R0, LSL #2 */
    uint32_t mov_shift = (0xEu << 28) | (0u << 25) | (0xDu << 21) | (0u << 20) | (0u << 16) | (3u << 12) | (2u << 7) | 0u;
    put32(ps, 16, mov_shift);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[3] == 20); /* 5 << 2 */

    /* MUL R4, R0, R1 (Rd=4, Rm=0, Rs=1) */
    uint32_t mul_r4 = (0xEu << 28) | (0u << 21) | (0u << 20) | (4u << 16) | (1u << 8) | (0x9u << 4) | 0u;
    put32(ps, 20, mul_r4);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[4] == 75); /* 5 * 15 */

    psemu_destroy(ps);
    printf("test_arm_data_processing OK\n");
}

static void test_arm_memory(void) {
    psemu_t *ps = make_arm_cpu();
    ps->cpu.r[0] = 0x100;
    ps->cpu.r[1] = 0x12345678u;

    /* STR R1, [R0] */
    uint32_t str_r1 = (0xEu << 28) | (1u << 26) | (1u << 24) | (1u << 23) | (0u << 16) | (1u << 12);
    put32(ps, 0, str_r1);
    arm7tdmi_step(&ps->cpu);
    assert(psemu_bus_read32(&ps->bus, 0x100) == 0x12345678u);

    /* LDR R2, [R0], #4 (post-indexed) */
    uint32_t ldr_r2 = (0xEu << 28) | (1u << 26) | (0u << 24) | (1u << 23) | (1u << 20) | (0u << 16) | (2u << 12) | 4u;
    put32(ps, 4, ldr_r2);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[2] == 0x12345678u);
    assert(ps->cpu.r[0] == 0x104u);

    /* STRB R1, [R0, #1] */
    uint32_t strb_r1 = (0xEu << 28) | (1u << 26) | (1u << 24) | (1u << 23) | (1u << 22) | (0u << 16) | (1u << 12) | 1u;
    put32(ps, 8, strb_r1);
    arm7tdmi_step(&ps->cpu);
    assert(psemu_bus_read8(&ps->bus, 0x105) == 0x78u);

    /* STRH R1, [R0, #4] -> halfword transfer, immediate offset (sh=01) */
    uint32_t strh_r1 = (0xEu << 28) | (1u << 24) | (1u << 23) | (1u << 22) | (0u << 21) | (0u << 20) | (0u << 16) |
                        (1u << 12) | (0u << 8) | (1u << 7) | (1u << 5) | (1u << 4) | 4u;
    put32(ps, 12, strh_r1);
    arm7tdmi_step(&ps->cpu);
    assert(psemu_bus_read16(&ps->bus, 0x108) == 0x5678u);

    /* Block transfer: R4=0x200 base, STMIA R4!, {R5,R6,R7} */
    ps->cpu.r[4] = 0x200;
    ps->cpu.r[5] = 0x11;
    ps->cpu.r[6] = 0x22;
    ps->cpu.r[7] = 0x33;
    uint32_t stmia = (0xEu << 28) | (1u << 27) | (1u << 23) | (1u << 21) | (4u << 16) | 0xE0u;
    put32(ps, 16, stmia);
    arm7tdmi_step(&ps->cpu);
    assert(psemu_bus_read32(&ps->bus, 0x200) == 0x11u);
    assert(psemu_bus_read32(&ps->bus, 0x204) == 0x22u);
    assert(psemu_bus_read32(&ps->bus, 0x208) == 0x33u);
    assert(ps->cpu.r[4] == 0x20Cu);

    /* LDMIA R4!, {R8,R9} loading back from the freshly written block */
    ps->cpu.r[4] = 0x200;
    uint32_t ldmia = (0xEu << 28) | (1u << 27) | (1u << 23) | (1u << 21) | (1u << 20) | (4u << 16) | 0x300u; /* r8,r9 */
    put32(ps, 20, ldmia);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[8] == 0x11u);
    assert(ps->cpu.r[9] == 0x22u);
    assert(ps->cpu.r[4] == 0x208u);

    psemu_destroy(ps);
    printf("test_arm_memory OK\n");
}

static void test_arm_control_flow(void) {
    psemu_t *ps = make_arm_cpu();

    /* B forward: at pc=0x40, imm24=6 -> target = pc+8+(6*4) = 0x60 */
    uint32_t b_instr = (0xEu << 28) | (1u << 27) | (1u << 25) | 6u;
    put32(ps, 0x40, b_instr);
    ps->cpu.r[15] = 0x40;
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[15] == 0x60u);

    /* BL at pc=0x60: imm24=1 -> target = pc+8+(1*4) = 0x6C, LR = pc+4 */
    uint32_t bl_instr = (0xEu << 28) | (1u << 27) | (1u << 25) | (1u << 24) | 1u;
    put32(ps, 0x60, bl_instr);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[15] == 0x6Cu);
    assert(ps->cpu.r[14] == 0x64u);

    /* BX R0 with R0 = 0x6D (odd -> Thumb) */
    ps->cpu.r[0] = 0x6Du;
    uint32_t bx_instr = (0xEu << 28) | 0x012FFF10u | 0u; /* Rm = r0 */
    put32(ps, 0x6C, bx_instr);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.cpsr & CPSR_T);
    assert(ps->cpu.r[15] == 0x6Cu);

    psemu_destroy(ps);
    printf("test_arm_control_flow OK\n");
}

static void test_arm_exceptions_and_psr(void) {
    psemu_t *ps = make_arm_cpu();

    /* Switch to User mode directly, give USR its own SP, then trigger SWI
       and verify SVC banking (SPSR_svc, LR_svc, mode, vector) is correct. */
    arm_set_mode(&ps->cpu, ARM_MODE_USR);
    ps->cpu.r[13] = 0x9000;
    uint32_t old_cpsr = ps->cpu.cpsr;

    uint32_t swi_instr = (0xEu << 28) | (0xFu << 24);
    put32(ps, 0x50, swi_instr);
    ps->cpu.r[15] = 0x50;
    arm7tdmi_step(&ps->cpu);

    assert((ps->cpu.cpsr & CPSR_MODE_MASK) == ARM_MODE_SVC);
    assert(ps->cpu.r[15] == 0x08u);
    assert(ps->cpu.r[14] == 0x54u); /* return address = pc + 4 */
    assert(ps->cpu.spsr_bank[arm_current_bank(&ps->cpu)] == old_cpsr);

    /* Switching back to USR should restore its banked SP, untouched by SVC's. */
    ps->cpu.r[13] = 0x1234; /* SVC's own SP, distinct from USR's */
    arm_set_mode(&ps->cpu, ARM_MODE_USR);
    assert(ps->cpu.r[13] == 0x9000u);

    /* MRS/MSR round trip on the control byte (mode + I/F/T bits). */
    ps->cpu.r[0] = ARM_MODE_SVC | CPSR_I; /* value to load into CPSR control byte */
    uint32_t msr_instr = (0xEu << 28) | (1u << 24) | (1u << 21) | (1u << 16) | (0xFu << 12) | 0u;
    put32(ps, 0x58, msr_instr);
    ps->cpu.r[15] = 0x58;
    arm7tdmi_step(&ps->cpu);
    assert((ps->cpu.cpsr & CPSR_MODE_MASK) == ARM_MODE_SVC);
    assert(ps->cpu.cpsr & CPSR_I);

    uint32_t mrs_instr = (0xEu << 28) | (1u << 24) | (0u << 22) | (0xFu << 16) | (1u << 12);
    put32(ps, 0x5C, mrs_instr);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[1] == ps->cpu.cpsr);

    psemu_destroy(ps);
    printf("test_arm_exceptions_and_psr OK\n");
}

static void test_thumb_basic(void) {
    psemu_t *ps = make_thumb_cpu();

    /* MOV R0, #5 -> format3, op=00, Rd=0, imm8=5 */
    put16(ps, 0, (uint16_t)((0x1u << 13) | (0x0u << 11) | (0u << 8) | 5u));
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[0] == 5);

    /* ADD R0, #10 -> format3, op=10, Rd=0, imm8=10 */
    put16(ps, 2, (uint16_t)((0x1u << 13) | (0x2u << 11) | (0u << 8) | 10u));
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[0] == 15);

    /* LSL R1, R0, #2 -> format1, op=00, offset5=2, Rs=0, Rd=1 */
    put16(ps, 4, (uint16_t)((0x0u << 13) | (0x0u << 11) | (2u << 6) | (0u << 3) | 1u));
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[1] == 60);

    /* SUB R2, R1, R0 -> format2, I=0, op=sub(1), Rn=R0(field=0), Rs=1, Rd=2 */
    put16(ps, 6, (uint16_t)((0x0u << 13) | (0x3u << 11) | (1u << 9) | (0u << 6) | (1u << 3) | 2u));
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[2] == 45); /* 60 - 15 */

    /* AND R3, R2 (format4, op=0000): first set R3 = 0xFF via MOV imm, reuse R2. */
    put16(ps, 8, (uint16_t)((0x1u << 13) | (0x0u << 11) | (3u << 8) | 0xFFu)); /* MOV R3,#0xFF */
    arm7tdmi_step(&ps->cpu);
    put16(ps, 10, (uint16_t)((0x2u << 13) | (0x0u << 12) | (0x0u << 10) | (0x0u << 6) | (2u << 3) | 3u)); /* AND R3,R2 */
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[3] == (0xFFu & 45u));

    psemu_destroy(ps);
    printf("test_thumb_basic OK\n");
}

static void test_thumb_memory_and_control(void) {
    psemu_t *ps = make_thumb_cpu();

    /* MOV R0, #0x50 ; MOV R1, #0x34 ; STR R1, [R0, #0] ; LDR R2, [R0, #0] */
    put16(ps, 0, (uint16_t)((0x1u << 13) | (0x0u << 11) | (0u << 8) | 0x50u));
    arm7tdmi_step(&ps->cpu);
    put16(ps, 2, (uint16_t)((0x1u << 13) | (0x0u << 11) | (1u << 8) | 0x34u));
    arm7tdmi_step(&ps->cpu);

    /* format9: STR (byte=0,load=0), imm5=0, Rb=0, Rd=1 */
    put16(ps, 4, (uint16_t)((0x3u << 13) | (0u << 12) | (0u << 11) | (0u << 6) | (0u << 3) | 1u));
    arm7tdmi_step(&ps->cpu);
    assert(psemu_bus_read32(&ps->bus, 0x50) == 0x34u);

    /* format9: LDR into R2 */
    put16(ps, 6, (uint16_t)((0x3u << 13) | (0u << 12) | (1u << 11) | (0u << 6) | (0u << 3) | 2u));
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[2] == 0x34u);

    /* PUSH {R0,R1}; RAM is only 2KB, so pick an SP inside it (reset default is 0). */
    ps->cpu.r[13] = 0x700;
    put16(ps, 8, (uint16_t)((0x5u << 13) | (0x2u << 11) | (0x2u << 9) | (0u << 8) | 0x3u)); /* PUSH {r0,r1} */
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[13] == 0x6F8u);
    assert(psemu_bus_read32(&ps->bus, 0x6F8) == 0x50u);
    assert(psemu_bus_read32(&ps->bus, 0x6FC) == 0x34u);

    /* POP {R3,R4} from what PUSH just wrote */
    put16(ps, 10, (uint16_t)((0x5u << 13) | (0x2u << 11) | (0x2u << 9) | (1u << 11) | 0x18u)); /* POP {r3,r4} */
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[3] == 0x50u);
    assert(ps->cpu.r[4] == 0x34u);
    assert(ps->cpu.r[13] == 0x700u);

    /* Unconditional branch: at pc=0x40, imm11 offset such that target = pc+4+offset.
       offset field = 4 (word count in the 11-bit signed field, *2 applied by decode) -> +8 bytes. */
    put16(ps, 0x40, (uint16_t)((0x1Cu << 11) | 4u));
    ps->cpu.r[15] = 0x40;
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[15] == (0x40u + 4u + 8u));

    /* BL pair at pc=0x50/0x52: high half offset_high=0, low half offset_low=2 (-> +4 bytes) */
    put16(ps, 0x50, (uint16_t)((0x1Eu << 11) | 0u)); /* 11110 = high half */
    put16(ps, 0x52, (uint16_t)((0x1Fu << 11) | 2u)); /* 11111 = low half */
    ps->cpu.r[15] = 0x50;
    arm7tdmi_step(&ps->cpu); /* high half: LR = pc+4 */
    arm7tdmi_step(&ps->cpu); /* low half: PC = LR + offset_low*2, LR = return addr */
    assert(ps->cpu.r[15] == (0x50u + 4u + 4u));
    assert(ps->cpu.r[14] == 0x54u);

    psemu_destroy(ps);
    printf("test_thumb_memory_and_control OK\n");
}

int main(void) {
    test_arm_data_processing();
    test_arm_memory();
    test_arm_control_flow();
    test_arm_exceptions_and_psr();
    test_thumb_basic();
    test_thumb_memory_and_control();
    printf("all cpu tests passed\n");
    return 0;
}
