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

static void test_arm_long_multiply_and_swap(void) {
    psemu_t *ps = make_arm_cpu();

    /* UMULL R2,R3,R0,R1 (RdHi=2,RdLo=3,Rm=0,Rs=1): 0xFFFFFFFF * 2 unsigned */
    ps->cpu.r[0] = 0xFFFFFFFFu;
    ps->cpu.r[1] = 2u;
    uint32_t umull = (0xEu << 28) | (1u << 23) | (0u << 22) | (0u << 21) | (0u << 20) | (2u << 16) | (3u << 12) |
                      (1u << 8) | (0x9u << 4) | 0u;
    put32(ps, 0, umull);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[3] == 0xFFFFFFFEu); /* RdLo */
    assert(ps->cpu.r[2] == 1u);          /* RdHi */

    /* SMULL R4,R5,R0,R1 with R0=-2, R1=3 (signed): -2 * 3 = -6 */
    ps->cpu.r[0] = (uint32_t)-2;
    ps->cpu.r[1] = 3u;
    uint32_t smull = (0xEu << 28) | (1u << 23) | (1u << 22) | (0u << 21) | (0u << 20) | (4u << 16) | (5u << 12) |
                      (1u << 8) | (0x9u << 4) | 0u;
    put32(ps, 4, smull);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[5] == 0xFFFFFFFAu); /* RdLo: low 32 bits of -6 */
    assert(ps->cpu.r[4] == 0xFFFFFFFFu); /* RdHi: sign-extended */

    /* SWP R2,R1,[R0]: atomically exchange R1 with the word at [R0] */
    ps->cpu.r[0] = 0x300u;
    ps->cpu.r[1] = 0x99u;
    put32(ps, 0x300, 0xAAu);
    uint32_t swp = (0xEu << 28) | (1u << 24) | (0u << 16) | (2u << 12) | (0x9u << 4) | 1u;
    put32(ps, 8, swp);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[2] == 0xAAu);                          /* old memory value */
    assert(psemu_bus_read32(&ps->bus, 0x300) == 0x99u);     /* R1 written in its place */

    psemu_destroy(ps);
    printf("test_arm_long_multiply_and_swap OK\n");
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

    /* Register-offset addressing (I=1 in the single-transfer encoding) is a
       data field within the "01" class, not a class discriminator - this
       regressed once already (real BIOS code hit it before any hand-written
       test did), so pin it down explicitly. */
    ps->cpu.r[10] = 0x200;
    ps->cpu.r[11] = 0x10;
    uint32_t str_reg_offset =
        (0xEu << 28) | (1u << 26) | (1u << 25) | (1u << 24) | (1u << 23) | (10u << 16) | (1u << 12) | 11u;
    put32(ps, 24, str_reg_offset);
    arm7tdmi_step(&ps->cpu);
    assert(psemu_bus_read32(&ps->bus, 0x210) == 0x12345678u);

    uint32_t ldr_reg_offset = (0xEu << 28) | (1u << 26) | (1u << 25) | (1u << 24) | (1u << 23) | (1u << 20) |
                              (10u << 16) | (12u << 12) | 11u;
    put32(ps, 28, ldr_reg_offset);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[12] == 0x12345678u);

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

static void test_arm_exception_return(void) {
    psemu_t *ps = make_arm_cpu();

    /* SWI, then MOVS PC,LR should return past the SWI with CPSR fully restored. */
    arm_set_mode(&ps->cpu, ARM_MODE_USR);
    ps->cpu.cpsr |= CPSR_Z; /* a recognizable flag pattern to verify restoration */
    ps->cpu.r[13] = 0x9000;
    uint32_t old_cpsr = ps->cpu.cpsr;

    uint32_t swi_instr = (0xEu << 28) | (0xFu << 24);
    put32(ps, 0x50, swi_instr);
    ps->cpu.r[15] = 0x50;
    arm7tdmi_step(&ps->cpu); /* enters SVC, LR_svc = 0x54 */
    assert(ps->cpu.r[15] == 0x08u);

    /* MOVS PC, LR: MOV(0xD), S=1, register operand2 = LR, LSL #0 */
    uint32_t movs_pc_lr = (0xEu << 28) | (0u << 25) | (0xDu << 21) | (1u << 20) | (0u << 16) | (15u << 12) | 14u;
    put32(ps, 0x08, movs_pc_lr);
    arm7tdmi_step(&ps->cpu);

    assert(ps->cpu.r[15] == 0x54u);
    assert(ps->cpu.cpsr == old_cpsr);
    assert((ps->cpu.cpsr & CPSR_MODE_MASK) == ARM_MODE_USR);
    assert(ps->cpu.r[13] == 0x9000u); /* USR's own SP, untouched by SVC's */

    /* IRQ, then SUBS PC,LR,#4 should return to the pre-empted instruction. */
    ps->cpu.cpsr &= ~CPSR_I; /* unmask so the IRQ can actually deliver */
    old_cpsr = ps->cpu.cpsr;
    ps->cpu.r[15] = 0x60;
    ps->intc.enable |= INT_TIMER0;
    intc_set_line(&ps->intc, INT_TIMER0, 1); /* assert a real (enabled) interrupt source */
    arm7tdmi_step(&ps->cpu); /* delivers the IRQ instead of fetching at 0x60; LR_irq = 0x64 */
    assert(ps->cpu.r[15] == ARM_IRQ_VECTOR);
    assert(ps->cpu.r[14] == 0x64u);

    /* SUBS PC, LR, #4: SUB(0x2), S=1, Rn=LR, Rd=PC, imm8=4 */
    uint32_t subs_pc_lr4 = (0xEu << 28) | (1u << 25) | (0x2u << 21) | (1u << 20) | (14u << 16) | (15u << 12) | 4u;
    put32(ps, (uint32_t)ARM_IRQ_VECTOR, subs_pc_lr4);
    arm7tdmi_step(&ps->cpu);

    assert(ps->cpu.r[15] == 0x60u);
    assert(ps->cpu.cpsr == old_cpsr);
    assert((ps->cpu.cpsr & CPSR_MODE_MASK) == ARM_MODE_USR);

    psemu_destroy(ps);
    printf("test_arm_exception_return OK\n");
}

static void test_arm_ldm_exception_return(void) {
    psemu_t *ps = make_arm_cpu();

    /* SWI, then LDM SP!,{R0,PC}^ should return with CPSR fully restored -
       this is the return idiom a real BIOS SWI handler actually uses. */
    arm_set_mode(&ps->cpu, ARM_MODE_USR);
    ps->cpu.cpsr |= CPSR_Z;
    ps->cpu.r[13] = 0x9000;
    uint32_t old_cpsr = ps->cpu.cpsr;

    uint32_t swi_instr = (0xEu << 28) | (0xFu << 24);
    put32(ps, 0x50, swi_instr);
    ps->cpu.r[15] = 0x50;
    arm7tdmi_step(&ps->cpu); /* enters SVC, LR_svc = 0x54 */
    assert(ps->cpu.r[15] == 0x08u);

    ps->cpu.r[13] = 0x300;
    put32(ps, 0x300, 0xDEADBEEFu); /* R0 */
    put32(ps, 0x304, 0x54u);       /* PC: matches the SWI's own return address */

    /* LDM R13!,{R0,R15}^ : P=0(IA),U=1,S=1,W=1,L=1,Rn=13,reglist={r0,r15} */
    uint32_t ldm_instr =
        (0xEu << 28) | (1u << 27) | (1u << 23) | (1u << 22) | (1u << 21) | (1u << 20) | (13u << 16) | 0x8001u;
    put32(ps, 0x08, ldm_instr);
    arm7tdmi_step(&ps->cpu);

    assert(ps->cpu.r[0] == 0xDEADBEEFu);
    assert(ps->cpu.r[15] == 0x54u);
    assert(ps->cpu.cpsr == old_cpsr);
    assert((ps->cpu.cpsr & CPSR_MODE_MASK) == ARM_MODE_USR);
    /* Writeback lands in SVC's own r13 (0x308) before the mode switch back
       to USR swaps in USR's own banked r13 (0x9000) - the SVC-mode value
       is still safely preserved in its bank, just not the visible register. */
    assert(ps->cpu.r[13] == 0x9000u);

    psemu_destroy(ps);
    printf("test_arm_ldm_exception_return OK\n");
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
    assert(ps->cpu.r[14] == 0x55u); /* bit0 tags the return address as Thumb, for a later BX LR */

    psemu_destroy(ps);
    printf("test_thumb_memory_and_control OK\n");
}

static void test_thumb_bl_bx_lr_stays_thumb(void) {
    psemu_t *ps = make_thumb_cpu();

    /* BL 0x100 at pc=0: high half offset_high=0 (LR becomes pc+4=4), low
       half offset_low=0x7E so target = 4 + 0x7E*2 = 0x100. */
    put16(ps, 0, (uint16_t)((0x1Eu << 11) | 0u));
    put16(ps, 2, (uint16_t)((0x1Fu << 11) | 0x7Eu));
    arm7tdmi_step(&ps->cpu); /* high half */
    arm7tdmi_step(&ps->cpu); /* low half: PC=0x100, LR=(2+2)|1=5 */
    assert(ps->cpu.r[15] == 0x100u);
    assert(ps->cpu.r[14] == 5u);

    /* BX LR at 0x100: format5, op=3(BX), H2=1, Rs field=6 (+8 -> r14). */
    uint32_t bx_lr = (0x11u << 10) | (3u << 8) | (1u << 6) | (6u << 3);
    put16(ps, 0x100, (uint16_t)bx_lr);
    arm7tdmi_step(&ps->cpu);

    /* This is exactly the case that broke against a real BIOS+app: without
       the bit0 tag, BX LR would incorrectly switch to ARM mode here. */
    assert(ps->cpu.cpsr & CPSR_T);
    assert(ps->cpu.r[15] == 4u);

    psemu_destroy(ps);
    printf("test_thumb_bl_bx_lr_stays_thumb OK\n");
}

static void test_intc_status_sources_also_latch_hold(void) {
    psemu_t *ps = make_arm_cpu();

    /* Buttons and the RTC tick (INT_STATUS_MASK bits) must latch into
       BOTH status and hold, not status only. An earlier version of
       intc_set_line only set `status` for these bits (ported directly
       - confirmed wrong by disassembling
       the real BIOS: its top-level IRQ handler tests
       `hold & enable & INT_RTC` and its installed periodic callback
       tests `hold & INT_BTN_ACTION`, both driving real handlers that
       could never run if these bits never reached `hold`. Without this,
       button presses and RTC ticks could be polled via `status` but
       could never actually raise a real IRQ. */
    ps->intc.enable |= INT_RTC;
    intc_set_line(&ps->intc, INT_RTC, 1);
    assert((ps->intc.hold & INT_RTC) != 0u);
    assert((ps->intc.status & INT_RTC) != 0u);
    assert(intc_irq_asserted(&ps->intc));

    ps->intc.enable |= INT_BTN_ACTION;
    intc_set_line(&ps->intc, INT_BTN_ACTION, 1);
    assert((ps->intc.hold & INT_BTN_ACTION) != 0u);
    assert((ps->intc.status & INT_BTN_ACTION) != 0u);

    /* Clearing (e.g. a button release, or the RTC's own alternating
       tick) drops both. A tempting-looking alternative was tried and
       disproved empirically: making only STATUS follow de-assertion
       (leaving HOLD latched until an explicit acknowledge, matching
       the documented INT_INPUT-vs-INT_LATCH naming and the real RTC handler's
       own explicit ack) sounds plausible, but the real button-action
       callback (traced at 0x04003784) never acknowledges its bit -
       making a button release leave it latched forever caused the CPU
       to re-enter the IRQ handler on nearly every subsequent instruction
       after a single press (559034 re-entries across a 20M-instruction
       real-BIOS run). A real, usable device can't work that way, so
       buttons (and by extension everything else, for consistency -
       ack already clears both regardless) clear both hold and status
       on release. */
    intc_set_line(&ps->intc, INT_RTC, 0);
    assert((ps->intc.hold & INT_RTC) == 0u);
    assert((ps->intc.status & INT_RTC) == 0u);

    psemu_destroy(ps);
    printf("test_intc_status_sources_also_latch_hold OK\n");
}

static void test_timer_and_irq(void) {
    psemu_t *ps = make_arm_cpu();

    /* Timer0: period=count=10, enabled; also enable its source in the INTC -
       hold alone isn't enough to assert IRQ, matching real hardware.
       Control bits 0-1 are left at 0 -> the slowest selectable divisor is
       actually /2 (0 and 3 both mean /2, per both an earlier, unconfirmed source's timer-start function and
       the officially documented divider table) - confirmed real behavior, not
       a raw 1:1 cycle-to-count ratio, so the cycle counts below are
       doubled relative to `period` to account for it. */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x0, 10u); /* period */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x4, 10u); /* count */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x8, TIMER_CTRL_ENABLE);
    ps->intc.enable |= INT_TIMER0;
    assert(psemu_bus_read32(&ps->bus, PSEMU_TIMER_BASE + 0x0) == 10u);

    timer_tick(&ps->timer, &ps->intc, 10u);
    assert(!intc_irq_asserted(&ps->intc)); /* fewer than `period` /2-divided ticks: no expiry yet */
    timer_tick(&ps->timer, &ps->intc, 10u);
    assert(intc_irq_asserted(&ps->intc)); /* the rest pushes it past the period value */

    /* Give USR mode its own SP so we can confirm IRQ entry banks r13/r14/SPSR
       independently of it, then trigger delivery via a single step. */
    arm_set_mode(&ps->cpu, ARM_MODE_USR);
    ps->cpu.cpsr &= ~CPSR_I; /* reset leaves IRQs masked; unmask like real startup code would */
    ps->cpu.r[13] = 0x9000;
    uint32_t old_cpsr = ps->cpu.cpsr;
    ps->cpu.r[15] = 0x30; /* address the pending IRQ preempts */

    arm7tdmi_step(&ps->cpu); /* delivers the IRQ instead of fetching at 0x30 */

    assert((ps->cpu.cpsr & CPSR_MODE_MASK) == ARM_MODE_IRQ);
    assert(ps->cpu.r[15] == ARM_IRQ_VECTOR);
    assert(ps->cpu.r[14] == 0x34u); /* 0x30 + 4, per the SUBS PC,LR,#4 exit convention */
    assert(ps->cpu.spsr_bank[arm_current_bank(&ps->cpu)] == old_cpsr);
    assert(ps->cpu.cpsr & CPSR_I); /* IRQs disabled on entry until the handler re-enables them */

    /* With IRQs masked (I set), the still-asserted line must not re-deliver -
       real hardware is level-triggered, not a one-shot request. */
    uint32_t pc_before = ps->cpu.r[15];
    put32(ps, pc_before, 0xE1A00000u); /* MOV R0,R0 (no-op) */
    arm7tdmi_step(&ps->cpu);
    assert((ps->cpu.cpsr & CPSR_MODE_MASK) == ARM_MODE_IRQ); /* unchanged, no re-entry happened */
    assert(ps->cpu.r[15] == pc_before + 4u);                 /* normal execution proceeded instead */

    psemu_destroy(ps);
    printf("test_timer_and_irq OK\n");
}

static void test_timer_clock_divisor(void) {
    psemu_t *ps = make_arm_cpu();

    /* Control bits 0-1 = 1 selects /32 (confirmed via both an earlier, unconfirmed source's
       timer_start and the officially documented divider table) - an earlier
       version of timer_tick ignored this entirely and decremented count
       by raw cycles directly, which would fire this timer 16x too often
       relative to the /2 case. period=count=1 so a single expiry needs
       exactly one selected timer tick, i.e. 32 raw cycles. */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x0, 1u); /* period */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x4, 1u); /* count */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x8, TIMER_CTRL_ENABLE | 1u); /* /32, enabled */
    ps->intc.enable |= INT_TIMER0;

    timer_tick(&ps->timer, &ps->intc, 31u);
    assert(!intc_irq_asserted(&ps->intc)); /* one short of a full /32 tick: must not expire yet */
    timer_tick(&ps->timer, &ps->intc, 1u);
    assert(intc_irq_asserted(&ps->intc)); /* the 32nd raw cycle completes the tick */

    psemu_destroy(ps);
    printf("test_timer_clock_divisor OK\n");
}

static void test_boot_ready_stub(void) {
    psemu_t *ps = make_arm_cpu();

    /* Real BIOS polls this address (LDR/TST #0x10/BEQ) before flash-control
       init; see docs/hardware-notes.md. Must read back with bit 4 set or
       a real boot sequence hangs forever. */
    assert(psemu_bus_read32(&ps->bus, PSEMU_HW_READY_BASE) & 0x10u);

    /* Bit 9 of INT_INPUT is the RTC's toggling interrupt line, not a static
       flag - see test_rtc_defaults_and_increment for its actual behavior. */

    psemu_destroy(ps);
    printf("test_boot_ready_stub OK\n");
}

static void test_rtc_defaults_and_increment(void) {
    psemu_t *ps = make_arm_cpu();

    /* Real silicon power-on-reset values per the real register
       table: date 1998-01-01, time 00:00:00 with day-of-week BCD 4 - see
       rtc.h for why this isn't an earlier, unconfirmed source's arbitrary 1999-01-01. */
    assert(psemu_bus_read32(&ps->bus, PSEMU_RTC_BASE + 0xC) == 0x00980101u); /* date: day,month,year,(unused) */
    assert(psemu_bus_read32(&ps->bus, PSEMU_RTC_BASE + 0x8) == 0x04000000u); /* time: sec,min,hour,dow */

    /* Writing 1 to control while it's already 1 increments the field
       selected by mode>>1 (4 = day) - the real "write 1 twice" idiom.
       mode's bit0 (PRGSEL) is left clear (0 = paused/program mode) so the
       later auto-advance-on-tick check below doesn't also perturb `date`
       via the manual write path under test here. */
    psemu_bus_write32(&ps->bus, PSEMU_RTC_BASE + 0x0, (4u << 1) | 1u); /* mode = day, paused */
    psemu_bus_write8(&ps->bus, PSEMU_RTC_BASE + 0x4, 1u);       /* control: 0 -> 1, just stored */
    assert(psemu_bus_read32(&ps->bus, PSEMU_RTC_BASE + 0xC) == 0x00980101u); /* unchanged so far */
    psemu_bus_write8(&ps->bus, PSEMU_RTC_BASE + 0x4, 1u);       /* control: 1 -> increments day */
    assert(psemu_bus_read32(&ps->bus, PSEMU_RTC_BASE + 0xC) == 0x00980102u); /* day is now 2 */
    assert(psemu_bus_read8(&ps->bus, PSEMU_RTC_BASE + 0x4) == 0u);           /* control reset to 0 */

    /* Real BIOS waits for a full pulse (rising then falling) on the RTC's
       line in INTC status, not just a level - a constant value can't
       satisfy that. Still paused (mode bit0 set above), so this exercises
       the faster ~4096Hz paused tick rate and must NOT auto-advance time. */
    uint32_t time_before_tick = psemu_bus_read32(&ps->bus, PSEMU_RTC_BASE + 0x8);
    rtc_tick(&ps->rtc, &ps->intc, RTC_TICK_CYCLES_PAUSED);
    assert(intc_get_line(&ps->intc, INT_RTC) != 0u);
    assert((psemu_bus_read32(&ps->bus, PSEMU_INTC_BASE + 0x4) & INT_RTC) != 0u);
    rtc_tick(&ps->rtc, &ps->intc, RTC_TICK_CYCLES_PAUSED);
    assert(intc_get_line(&ps->intc, INT_RTC) == 0u);
    assert((psemu_bus_read32(&ps->bus, PSEMU_INTC_BASE + 0x4) & INT_RTC) == 0u);
    assert(psemu_bus_read32(&ps->bus, PSEMU_RTC_BASE + 0x8) == time_before_tick); /* paused: no auto-advance */

    /* Switch to running mode (bit0 clear) and confirm the tick now both
       pulses INT_RTC and auto-advances the seconds field - the behavior
       missing entirely before this fix (only the interrupt line toggled,
       the clock itself never moved on its own). */
    psemu_bus_write32(&ps->bus, PSEMU_RTC_BASE + 0x0, 0u); /* mode = running */
    rtc_tick(&ps->rtc, &ps->intc, RTC_TICK_CYCLES_RUN);
    assert((psemu_bus_read32(&ps->bus, PSEMU_RTC_BASE + 0x8) & 0xFFu) == 0x01u); /* seconds: 0 -> 1 */

    psemu_destroy(ps);
    printf("test_rtc_defaults_and_increment OK\n");
}

static void test_flash_bank_select(void) {
    psemu_t *ps = make_arm_cpu();

    /* Distinct markers at FLASH2 block 0 and block 1 (8192 bytes apart). */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH2_BASE + 0, 0xAAAAAAAAu);
    psemu_bus_write32(&ps->bus, PSEMU_FLASH2_BASE + 8192, 0xBBBBBBBBu);

    /* Before any bank-select write, FLASH1 aliases block 0 (offset 0). */
    assert(psemu_bus_read32(&ps->bus, PSEMU_FLASH1_BASE) == 0xAAAAAAAAu);

    /* Select block 1 the same way the real BIOS does: bitmask to +8, then
       the observed commit value 2 to +0 (see docs/hardware-notes.md). */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 8, 1u << 1);
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0, 2u);

    /* FLASH1 now windows onto block 1; FLASH2 stays a plain physical view. */
    assert(psemu_bus_read32(&ps->bus, PSEMU_FLASH1_BASE) == 0xBBBBBBBBu);
    assert(psemu_bus_read32(&ps->bus, PSEMU_FLASH2_BASE) == 0xAAAAAAAAu);
    assert(psemu_bus_read32(&ps->bus, PSEMU_FLASH2_BASE + 8192) == 0xBBBBBBBBu);

    /* Writes through FLASH1 land at the windowed offset, not the base. */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH1_BASE + 4, 0xCCCCCCCCu);
    assert(psemu_bus_read32(&ps->bus, PSEMU_FLASH2_BASE + 8192 + 4) == 0xCCCCCCCCu);

    psemu_destroy(ps);
    printf("test_flash_bank_select OK\n");
}

int main(void) {
    test_arm_data_processing();
    test_arm_long_multiply_and_swap();
    test_arm_memory();
    test_arm_control_flow();
    test_arm_exceptions_and_psr();
    test_arm_exception_return();
    test_arm_ldm_exception_return();
    test_thumb_basic();
    test_thumb_memory_and_control();
    test_thumb_bl_bx_lr_stays_thumb();
    test_intc_status_sources_also_latch_hold();
    test_timer_and_irq();
    test_timer_clock_divisor();
    test_boot_ready_stub();
    test_rtc_defaults_and_increment();
    test_flash_bank_select();
    printf("all cpu tests passed\n");
    return 0;
}
