/* Each test in this file is an assert() call. A Release build defines NDEBUG, and NDEBUG makes each
   assert() call do nothing. Thus this full test suite executed as a few hundred empty operations, and
   one printf call for each test, in each Release configuration. It reported "all cpu tests passed",
   and it tested nothing. The release workflows (.github/workflows/) execute ctest in Release, thus
   this fault was most important there.
   A test proves this. It is not an inference: an assert(0) call at the top of main still gave an exit
   code of 0, and it printed the full passing output in a Release build.
   The #undef below keeps the tests in each configuration, and it needs no change to the build files.
   It must come before <assert.h>, because that header gives the definition of assert() at the time of
   its inclusion. */
#undef NDEBUG

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

    /* Register-offset addressing (I = 1 in the single-transfer encoding) is a
       data field in the "01" class. It is not a class selector. This code was
       incorrect one time before, and real BIOS code found the fault before a
       manual test did. Thus this test confirms the behavior explicitly. */
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

static void test_arm_ldrh_misaligned_quirks(void) {
    /* A real, confirmed hardware quirk of the ARM7TDMI (see
       exec_halfword_transfer in arm_exec.c, and docs/hardware-notes.md,
       "CPU"):
       a misaligned halfword load does not round down to the aligned halfword
       below it. Real silicon rotates the loaded LDRH value right by 8 bits,
       which exchanges its two bytes. A misaligned LDRSH instead operates as a
       sign-extended BYTE load (LDRSB) from that same odd address. The
       font-glyph routine of a real PocketStation homebrew ID editor found
       this behavior. That routine reads a byte-packed table with
       `LDRH Rd,[Rn],#1`, which is a post-increment of 1 and not 2, and masks
       the result to the low byte. That method operates correctly on real
       hardware, and it depends fully on this rotation to get each byte.
       Without the rotation, every second byte was incorrect, and each glyph
       on the screen was incorrect. */
    psemu_t *ps = make_arm_cpu();

    psemu_bus_write8(&ps->bus, 0x100, 0x11u);
    psemu_bus_write8(&ps->bus, 0x101, 0x22u);
    psemu_bus_write8(&ps->bus, 0x102, 0x33u);
    psemu_bus_write8(&ps->bus, 0x103, 0x80u);
    ps->cpu.r[0] = 0x100;

    /* LDRH R1, [R0] - aligned (address 0x100): plain halfword read, no
       rotation - regression safety that the ordinary case is unaffected. */
    uint32_t ldrh_even = (0xEu << 28) | (1u << 24) | (1u << 23) | (1u << 22) | (1u << 20) | (0u << 16) | (1u << 12) |
                          (0u << 8) | (1u << 7) | (1u << 5) | (1u << 4) | 0u;
    put32(ps, 0, ldrh_even);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[1] == 0x2211u);

    /* LDRH R2, [R0, #1] is misaligned, at address 0x101. It rotates the
       aligned halfword (0x2211) right by 8 bits, which gives 0x1122. Thus the
       byte at the odd address (0x22) is in the low 8 bits. This result agrees
       with the byte reader of the real font routine. */
    uint32_t ldrh_odd = (0xEu << 28) | (1u << 24) | (1u << 23) | (1u << 22) | (1u << 20) | (0u << 16) | (2u << 12) |
                         (0u << 8) | (1u << 7) | (1u << 5) | (1u << 4) | 1u;
    put32(ps, 4, ldrh_odd);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[2] == 0x1122u);
    assert((ps->cpu.r[2] & 0xFFu) == 0x22u);

    /* LDRSH R3, [R0] - aligned (address 0x100): ordinary signed halfword
       read, unaffected (0x2211's sign bit is clear). */
    uint32_t ldrsh_even = (0xEu << 28) | (1u << 24) | (1u << 23) | (1u << 22) | (1u << 20) | (0u << 16) |
                           (3u << 12) | (0u << 8) | (1u << 7) | (1u << 6) | (1u << 5) | (1u << 4) | 0u;
    put32(ps, 8, ldrsh_even);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[3] == 0x00002211u);

    /* LDRSH R4, [R0, #3] is misaligned, at address 0x103. It operates as a
       sign-extended BYTE load from 0x103 (0x80, with the sign bit set). It is
       NOT a rotated halfword read. This is the recorded difference of real
       hardware from the LDRH rotation behavior above. */
    uint32_t ldrsh_odd = (0xEu << 28) | (1u << 24) | (1u << 23) | (1u << 22) | (1u << 20) | (0u << 16) |
                          (4u << 12) | (0u << 8) | (1u << 7) | (1u << 6) | (1u << 5) | (1u << 4) | 3u;
    put32(ps, 12, ldrsh_odd);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[4] == 0xFFFFFF80u);

    psemu_destroy(ps);
    printf("test_arm_ldrh_misaligned_quirks OK\n");
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

    /* An MRS and MSR round trip on the control byte, which holds the mode and
       the I, F, and T bits. This test executes here, in SVC mode from the SWI
       above. SVC mode is privileged. On real hardware, a write to the control
       byte of the CPSR is successful only from a privileged mode. See
       test_msr_user_mode_control_byte_is_ignored for the User-mode
       condition. */
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

    /* Switching back to USR should restore its banked SP, untouched by SVC's. */
    ps->cpu.r[13] = 0x1234; /* SVC's own SP, distinct from USR's */
    arm_set_mode(&ps->cpu, ARM_MODE_USR);
    assert(ps->cpu.r[13] == 0x9000u);

    psemu_destroy(ps);
    printf("test_arm_exceptions_and_psr OK\n");
}

static void test_msr_user_mode_control_byte_is_ignored(void) {
    /* On real ARM7TDMI and ARMv4T silicon, an MSR to the CPSR from User mode
       can change only the condition flags, which are the highest byte. The
       control byte (the mode, T, F, and I) is protected. The hardware ignores
       a write to it and gives no error. This is true for each control bit
       that the instruction selects. A real homebrew app (pk_timing_bench)
       found this behavior. That app depended on the earlier incorrect
       behavior of this emulator without intent: it wrote CPSR_I and CPSR_F
       from User mode, and expected no effect on real hardware. But the write
       took effect here. Thus the interrupts stayed masked globally for the
       full runtime of the app, which real hardware never does. See
       docs/hardware-notes.md. */
    psemu_t *ps = make_arm_cpu();
    arm_set_mode(&ps->cpu, ARM_MODE_USR);
    ps->cpu.cpsr &= ~(CPSR_I | CPSR_F); /* reset() sets these - start from a known-clear state */
    uint32_t before_cpsr = ps->cpu.cpsr;

    /* MSR CPSR_c, r0 - attempt to switch to SVC mode and set the I bit. */
    ps->cpu.r[0] = ARM_MODE_SVC | CPSR_I;
    uint32_t msr_c_instr = (0xEu << 28) | (1u << 24) | (1u << 21) | (1u << 16) | (0xFu << 12) | 0u;
    put32(ps, 0x50, msr_c_instr);
    ps->cpu.r[15] = 0x50;
    arm7tdmi_step(&ps->cpu);

    assert(ps->cpu.cpsr == before_cpsr); /* fully unchanged - mode, I, F, T all ignored */
    assert((ps->cpu.cpsr & CPSR_MODE_MASK) == ARM_MODE_USR);
    assert(!(ps->cpu.cpsr & CPSR_I));

    /* MSR CPSR_f, r1 - the flags byte IS writable from User mode. */
    ps->cpu.r[1] = CPSR_N | CPSR_Z | CPSR_C | CPSR_V;
    uint32_t msr_f_instr = (0xEu << 28) | (1u << 24) | (1u << 21) | (8u << 16) | (0xFu << 12) | 1u;
    put32(ps, 0x54, msr_f_instr);
    ps->cpu.r[15] = 0x54;
    arm7tdmi_step(&ps->cpu);

    assert((ps->cpu.cpsr & CPSR_MODE_MASK) == ARM_MODE_USR); /* still unprivileged */
    assert(ps->cpu.cpsr & CPSR_N);
    assert(ps->cpu.cpsr & CPSR_Z);
    assert(ps->cpu.cpsr & CPSR_C);
    assert(ps->cpu.cpsr & CPSR_V);

    psemu_destroy(ps);
    printf("test_msr_user_mode_control_byte_is_ignored OK\n");
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
    /* The writeback goes into the r13 of SVC mode (0x308). The change back to
       USR mode then loads the banked r13 of USR mode (0x9000). The SVC-mode
       value stays in its bank. It is only not in the visible register. */
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

    /* An unconditional branch at pc = 0x40. The imm11 offset gives
       target = pc + 4 + offset.
       The offset field is 4. That field is a signed 11-bit count, and the decode
       operation multiplies it by 2. Thus the offset is +8 bytes. */
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

static void test_cpu_faulted_flag(void) {
    /* psemu_cpu_faulted(). This project added that function during an
       investigation of a real, repeatable crash (see
       docs/hardware-notes.md). In that crash, the desktop frontend had no
       method to find that the CPU met an unrecognized opcode. It continued to
       execute instructions, corrupted the state permanently, and gave no
       diagnostic data. 0xB800 is a Thumb "format 12/13/14" pattern, with the
       top 3 bits equal to 101. It agrees with no test in exec_load_address,
       exec_add_sp_offset, or exec_push_pop. Thus it goes to the unimplemented
       default. */
    psemu_t *ps = make_thumb_cpu();
    assert(!psemu_cpu_faulted(ps));
    put16(ps, 0, 0xB800u);
    arm7tdmi_step(&ps->cpu);
    assert(psemu_cpu_faulted(ps));
    psemu_destroy(ps);
    printf("test_cpu_faulted_flag OK\n");
}

static void test_faulted_cpu_stops_advancing(void) {
    /* A real, confirmed fault that a crash report from the desktop app found.
       arm7tdmi_step tested only `halted`, and no code sets that flag. It did
       not test `unimplemented`. Thus a caller that does not test the flag
       after each instruction continued to fetch and execute from incorrect
       addresses. The cycle-budget loop of psemu_run is such a caller. The
       per-instruction loop of tools/inspect.c is not. That continued
       execution can be thousands of instructions after the real fault, and it
       corrupts the registers and the PC before a user finds the condition.
       This test confirms two conditions: r15 stops, and the trace ring buffer
       stops its record after the fault. Thus a crash report shows the original
       fault, and not the later addresses. */
    psemu_t *ps = make_thumb_cpu();
    uint32_t pc_at_fault;

    put16(ps, 0, 0xB800u); /* guaranteed-unimplemented, see test_cpu_faulted_flag */
    arm7tdmi_step(&ps->cpu);
    assert(psemu_cpu_faulted(ps));
    pc_at_fault = ps->cpu.r[15];

    arm7tdmi_step(&ps->cpu);
    arm7tdmi_step(&ps->cpu);
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[15] == pc_at_fault);
    assert(ps->cpu.total_steps == 1u);

    psemu_destroy(ps);
    printf("test_faulted_cpu_stops_advancing OK\n");
}

static void test_crash_report_contents(void) {
    /* psemu_write_crash_report(). This project added that function so that
       the desktop frontend can write a real diagnostic file. Before it, the
       frontend wrote only a one-line stderr message at a fault. See
       docs/hardware-notes.md for the crash investigation that caused this
       work. This test confirms that the report gives the true fault opcode
       and its address, and that the report contains the executed-PC trace. It
       does not test only that the function does not crash. */
    psemu_t *ps = make_thumb_cpu();
    FILE *f = tmpfile();
    char buf[8192];
    size_t n;
    assert(f != NULL);

    put16(ps, 0, 0x1F00u); /* SUB R0,R0,#4 - ordinary, so the trace has >1 entry */
    arm7tdmi_step(&ps->cpu);
    put16(ps, 2, 0xB800u); /* guaranteed-unimplemented pattern, see test_cpu_faulted_flag */
    arm7tdmi_step(&ps->cpu);
    assert(psemu_cpu_faulted(ps));

    psemu_write_crash_report(ps, f);
    rewind(f);
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    assert(strstr(buf, "cpu faulted (unrecognized opcode): YES") != NULL);
    assert(strstr(buf, "unrecognized thumb opcode 0xB800, fetched from 0x00000002") != NULL);
    assert(strstr(buf, "pc=0x00000000 (thumb)") != NULL);
    assert(strstr(buf, "pc=0x00000002 (thumb)") != NULL);

    psemu_destroy(ps);
    printf("test_crash_report_contents OK\n");
}

static void test_intc_status_sources_also_latch_hold(void) {
    psemu_t *ps = make_arm_cpu();

    /* The buttons and the RTC tick (the INT_STATUS_MASK bits) must latch into
       BOTH status and hold. They must not latch into status only. An earlier
       version of intc_set_line set only `status` for these bits. A
       disassembly of the real BIOS confirms that this was incorrect: the
       top-level IRQ handler of the BIOS tests `hold & enable & INT_RTC`, and
       the periodic callback that it installs tests `hold & INT_BTN_ACTION`.
       Both tests reach real handlers, and those handlers can never execute if
       these bits never get to `hold`. Without this behavior, code can read
       button presses and RTC ticks through `status`, but they can never cause
       a real IRQ. */
    ps->intc.enable |= INT_RTC;
    intc_set_line(&ps->intc, INT_RTC, 1);
    assert((ps->intc.hold & INT_RTC) != 0u);
    assert((ps->intc.status & INT_RTC) != 0u);
    assert(intc_irq_asserted(&ps->intc));

    ps->intc.enable |= INT_BTN_ACTION;
    intc_set_line(&ps->intc, INT_BTN_ACTION, 1);
    assert((ps->intc.hold & INT_BTN_ACTION) != 0u);
    assert((ps->intc.status & INT_BTN_ACTION) != 0u);

    /* A clear operation, for example a button release or the alternating RTC
       tick, clears both registers. This project tested a different method and
       a test disproved it. That method made only STATUS follow the
       de-assertion, and left HOLD latched until an explicit acknowledge. It
       agreed with the INT_INPUT and INT_LATCH names, and with the explicit
       acknowledge of the real RTC handler. But the real button-action
       callback, at address 0x04003784, never acknowledges its bit. Thus a
       button release left the bit latched permanently, and the CPU entered
       the IRQ handler again at almost each instruction after one press. A
       real-BIOS run of 20 million instructions gave 559034 re-entries. A real
       device that a person can use cannot operate this way. Thus the buttons,
       and each other source for consistency, clear both hold and status at a
       release. An acknowledge already clears both registers. */
    intc_set_line(&ps->intc, INT_RTC, 0);
    assert((ps->intc.hold & INT_RTC) == 0u);
    assert((ps->intc.status & INT_RTC) == 0u);

    psemu_destroy(ps);
    printf("test_intc_status_sources_also_latch_hold OK\n");
}

static void test_button_hold_pulses_not_sustained(void) {
    psemu_t *ps = make_arm_cpu();

    /* A real, confirmed fault that a direct test on real hardware found. On
       the real device, a held Action button does nothing: an item that blinks
       continues to blink normally. Only a release confirms the selection. But
       the button HOLD bit of this emulator stayed latched as a continuous
       level for the full time of the hold. A real BIOS callback tests
       `hold & INT_BTN_ACTION` *before* its RTC redraw test. Thus a hold bit
       that stays set permanently prevented that redraw path while the user
       held the button. A watchpoint during execution confirmed this: the
       RTC blink counter did not change during a long hold. The correction:
       HOLD pulses only at the press edge. For a button that the user still
       holds, the next psemu_set_buttons call with no new edge clears HOLD
       again. That call does not clear STATUS, which continues to follow the
       live level. */
    ps->intc.enable |= INT_BTN_ACTION;

    psemu_set_buttons(ps, PSEMU_BUTTON_FIRE); /* press edge */
    assert((ps->intc.hold & INT_BTN_ACTION) != 0u);
    assert((ps->intc.status & INT_BTN_ACTION) != 0u);

    psemu_set_buttons(ps, PSEMU_BUTTON_FIRE); /* still held, no edge */
    assert((ps->intc.hold & INT_BTN_ACTION) == 0u);
    assert((ps->intc.status & INT_BTN_ACTION) != 0u); /* status still reflects the live level */

    psemu_set_buttons(ps, 0); /* release */
    assert((ps->intc.hold & INT_BTN_ACTION) == 0u);
    assert((ps->intc.status & INT_BTN_ACTION) == 0u);

    psemu_destroy(ps);
    printf("test_button_hold_pulses_not_sustained OK\n");
}

static void test_button_status_survives_acknowledge(void) {
    psemu_t *ps = make_arm_cpu();

    /* docs/hardware-notes.md, "Buttons": `status` follows the live button
       level, for code that reads the register directly. An acknowledge clears
       a latched interrupt REQUEST, thus it must clear HOLD. It must not clear
       that live level.

       This test failed before. The buttons were absent from INT_LEVEL_MASK,
       thus each acknowledge cleared their STATUS bit. A button that the user
       still held then read back as released, until something pressed the
       button again. The real BIOS acknowledges the button interrupts during
       its boot sequence. Thus an app that read STATUS for a held button saw
       nothing.

       A real app depends on this behavior: pk_timing_bench opens its exit
       prompt after 75000 sequential reads of STATUS with Action held. It then
       waits on the same level to see the release of the button before it
       exits. */
    ps->intc.enable |= INT_BTN_ACTION;

    psemu_set_buttons(ps, PSEMU_BUTTON_FIRE); /* press edge */
    assert((ps->intc.hold & INT_BTN_ACTION) != 0u);
    assert((ps->intc.status & INT_BTN_ACTION) != 0u);

    psemu_bus_write32(&ps->bus, PSEMU_INTC_BASE + 0x10, INT_BTN_ACTION); /* acknowledge */
    assert((ps->intc.hold & INT_BTN_ACTION) == 0u);   /* the request is cleared */
    assert((ps->intc.status & INT_BTN_ACTION) != 0u); /* the live level is not */

    psemu_set_buttons(ps, PSEMU_BUTTON_FIRE); /* still held, no new edge */
    assert((ps->intc.status & INT_BTN_ACTION) != 0u);

    psemu_set_buttons(ps, 0); /* release finally clears it */
    assert((ps->intc.status & INT_BTN_ACTION) == 0u);

    /* An acknowledge still clears STATUS for a latched, non-level source. */
    intc_set_line(&ps->intc, INT_RTC, 1);
    assert((ps->intc.status & INT_RTC) != 0u);
    psemu_bus_write32(&ps->bus, PSEMU_INTC_BASE + 0x10, INT_RTC);
    assert((ps->intc.status & INT_RTC) == 0u);

    psemu_destroy(ps);
    printf("test_button_status_survives_acknowledge OK\n");
}

static void test_timer_and_irq(void) {
    psemu_t *ps = make_arm_cpu();

    /* Timer0: period and count are 10, and the timer is enabled. This code
       also enables its source in the INTC, because hold alone does not assert
       an IRQ. This behavior agrees with real hardware.
       Control bits 0-1 stay at 0. Thus the divisor is /2, because 0 and 3
       both give /2. This is confirmed real hardware behavior. The ratio of
       cycles to counts is not 1:1. Thus the cycle counts below are two times
       the `period` value. */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x0, 10u); /* period */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x4, 10u); /* count */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x8, TIMER_CTRL_ENABLE);
    ps->intc.enable |= INT_TIMER0;
    assert(psemu_bus_read32(&ps->bus, PSEMU_TIMER_BASE + 0x0) == 10u);

    timer_tick(&ps->timer, &ps->intc, 10u);
    assert(!intc_irq_asserted(&ps->intc)); /* fewer than `period` /2-divided ticks: no expiry yet */
    timer_tick(&ps->timer, &ps->intc, 10u);
    /* 10 more /2 ticks make count exactly 0. But zero is a state of the real
       counter. Thus the counter reloads and expires at the NEXT tick, and the
       real period is P+1 and not P. A direct measurement on real hardware
       confirms this. See timer_tick in timer.c, and
       pk_timing_bench/VERIFICATION.md. This assertion expected the IRQ one
       tick earlier before. */
    assert(!intc_irq_asserted(&ps->intc));
    timer_tick(&ps->timer, &ps->intc, 2u); /* one more /2-divided tick: now it reloads and fires */
    assert(intc_irq_asserted(&ps->intc));

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

static void test_fiq_delivery_and_priority(void) {
    /* A real, confirmed fault (see docs/hardware-notes.md, "Interrupt
       controller"). This emulator never delivered a FIQ, for any app.
       intc_fiq_asserted (intc.c) already existed, and a comparison against
       the bit mapping of real hardware already confirmed it. But
       arm7tdmi_step tested only intc_irq_asserted. A trace found this fault.
       That trace examined the reason that the Timer2 confirmation sound of a
       real homebrew app gave silence. Timer2 uses FIQ, and not IRQ. The app
       wrote the period register and the enable register of Timer2, and
       INT_TIMER2 latched into the hold register of the interrupt controller.
       But the CPU mode never left USR. Thus the interrupt stayed asserted
       permanently, and nothing serviced it. */
    psemu_t *ps = make_arm_cpu();

    /* Timer2, and not Timer0 or Timer1. Timer2 is the FIQ source of real
       hardware, from the confirmed INT_FIQ_MASK bit mapping in
       docs/hardware-notes.md. The setup pattern is the same as the Timer0 and
       IRQ pattern of test_timer_and_irq. */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x20, 10u); /* period */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x24, 10u); /* count */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x28, TIMER_CTRL_ENABLE);
    ps->intc.enable |= INT_TIMER2;
    timer_tick(&ps->timer, &ps->intc, 10u);
    assert(!intc_fiq_asserted(&ps->intc));
    timer_tick(&ps->timer, &ps->intc, 10u);
    /* Same real P+1 period as test_timer_and_irq's IRQ case: count reaching zero is not yet an expiry. */
    assert(!intc_fiq_asserted(&ps->intc));
    timer_tick(&ps->timer, &ps->intc, 2u);
    assert(intc_fiq_asserted(&ps->intc));

    arm_set_mode(&ps->cpu, ARM_MODE_USR);
    ps->cpu.cpsr &= ~(CPSR_F | CPSR_I); /* reset leaves both masked; unmask like real startup code would */
    ps->cpu.r[13] = 0x9000;
    uint32_t old_cpsr = ps->cpu.cpsr;
    ps->cpu.r[15] = 0x30;

    arm7tdmi_step(&ps->cpu); /* delivers the FIQ instead of fetching at 0x30 */

    assert((ps->cpu.cpsr & CPSR_MODE_MASK) == ARM_MODE_FIQ);
    assert(ps->cpu.r[15] == ARM_FIQ_VECTOR);
    assert(ps->cpu.r[14] == 0x34u); /* 0x30 + 4, per the SUBS PC,LR,#4 exit convention */
    assert(ps->cpu.spsr_bank[arm_current_bank(&ps->cpu)] == old_cpsr);
    /* A real ARM7TDMI sets BOTH F and I at FIQ entry. IRQ entry sets only I.
       F prevents a second FIQ from an interruption of the handler before the
       handler saves its state. */
    assert(ps->cpu.cpsr & CPSR_F);
    assert(ps->cpu.cpsr & CPSR_I);

    /* Level-triggered, same as IRQ: with F still masked, the still-
       asserted line must not re-deliver. */
    uint32_t pc_before = ps->cpu.r[15];
    put32(ps, pc_before, 0xE1A00000u); /* MOV R0,R0 (no-op) */
    arm7tdmi_step(&ps->cpu);
    assert((ps->cpu.cpsr & CPSR_MODE_MASK) == ARM_MODE_FIQ); /* unchanged, no re-entry happened */
    assert(ps->cpu.r[15] == pc_before + 4u);

    psemu_destroy(ps);
    printf("test_fiq_delivery_and_priority OK\n");
}

static void test_fiq_takes_priority_over_irq(void) {
    /* A real ARM7TDMI tests FIQ before IRQ, because FIQ has a higher priority
       in the exception scheme. While both are pending and unmasked, the CPU
       must enter FIQ. It must not enter IRQ. */
    psemu_t *ps = make_arm_cpu();

    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x20, 1u); /* Timer2/FIQ: period */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x24, 1u); /* count */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x28, TIMER_CTRL_ENABLE);
    ps->intc.enable |= INT_TIMER2;

    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x0, 1u); /* Timer0/IRQ: period */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x4, 1u); /* count */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x8, TIMER_CTRL_ENABLE);
    ps->intc.enable |= INT_TIMER0;

    timer_tick(&ps->timer, &ps->intc, 4u);
    assert(intc_fiq_asserted(&ps->intc));
    assert(intc_irq_asserted(&ps->intc));

    arm_set_mode(&ps->cpu, ARM_MODE_USR);
    ps->cpu.cpsr &= ~(CPSR_F | CPSR_I);
    ps->cpu.r[13] = 0x9000;
    ps->cpu.r[15] = 0x30;

    arm7tdmi_step(&ps->cpu);

    assert((ps->cpu.cpsr & CPSR_MODE_MASK) == ARM_MODE_FIQ);
    assert(ps->cpu.r[15] == ARM_FIQ_VECTOR);

    psemu_destroy(ps);
    printf("test_fiq_takes_priority_over_irq OK\n");
}

static void test_timer_clock_divisor(void) {
    psemu_t *ps = make_arm_cpu();

    /* Control bits 0-1 with the value 1 select the /32 divisor. An earlier
       version of timer_tick did not use this field. It decreased count by the
       raw cycles directly, which makes this timer expire 16 times too
       frequently against the /2 condition. period and count are 1, and the
       real period is P+1 selected ticks (see timer.c). Thus an expiry needs
       two /32 ticks, which is 64 raw cycles. This test confirms the divisor.
       Without the divisor, the timer expires in the first few raw cycles. */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x0, 1u); /* period */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x4, 1u); /* count */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x8, TIMER_CTRL_ENABLE | 1u); /* /32, enabled */
    ps->intc.enable |= INT_TIMER0;

    timer_tick(&ps->timer, &ps->intc, 31u);
    assert(!intc_irq_asserted(&ps->intc)); /* one short of a full /32 tick: no tick has even elapsed */
    timer_tick(&ps->timer, &ps->intc, 1u);
    assert(!intc_irq_asserted(&ps->intc)); /* the 32nd raw cycle completes tick 1: count hits 0, not yet an expiry */
    timer_tick(&ps->timer, &ps->intc, 32u);
    assert(intc_irq_asserted(&ps->intc)); /* tick 2 reloads and fires */

    psemu_destroy(ps);
    printf("test_timer_clock_divisor OK\n");
}

static void test_timer_registers_are_16_bit(void) {
    psemu_t *ps = make_arm_cpu();

    /* On real hardware, period and count are 16-bit registers. A wider store
       keeps only the low half (see TIMER_REG_MASK in timer.h, and
       docs/hardware-notes.md, "Timers").
       This emulator modeled both registers as a full uint32_t before. The FIQ
       audio timer of one music app programs Timer2 with a value whose upper
       half is not zero. If the register keeps that half, the period increases
       from 851 ticks to approximately 52.6 million. Thus the audio interrupt
       never occurred, and the app made no sound. */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x20, 0x03240353u); /* Timer2 period */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x24, 0x0323B1FDu); /* Timer2 count */
    assert(ps->timer.timers[2].period == 0x0353u);
    assert(ps->timer.timers[2].count == 0xB1FDu);
    assert(psemu_bus_read32(&ps->bus, PSEMU_TIMER_BASE + 0x20) == 0x0353u);

    /* Enabling reloads count from the masked period, so a timer armed this way expires on
       the real 851-tick schedule rather than never. */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x28, TIMER_CTRL_ENABLE);
    assert(ps->timer.timers[2].count == 0x0353u);

    /* The reload at an expiry also uses the mask. Thus a state from the older
       32-bit model becomes correct. It does not stay at a very large
       count. */
    ps->timer.timers[2].period = 0x00FF0010u;
    ps->timer.timers[2].count = 0u;
    ps->intc.enable |= INT_TIMER2;
    timer_tick(&ps->timer, &ps->intc, 4u); /* /2 divisor: 2 ticks, enough to pass zero and reload */
    assert(ps->timer.timers[2].count <= TIMER_REG_MASK);

    psemu_destroy(ps);
    printf("test_timer_registers_are_16_bit OK\n");
}

static void test_boot_ready_stub(void) {
    psemu_t *ps = make_arm_cpu();

    /* The real BIOS reads this address (LDR, TST #0x10, BEQ) before it
       initializes the flash control. See docs/hardware-notes.md. The register
       must read back with bit 4 set. If it does not, a real boot sequence
       continues for an unlimited time. */
    assert(psemu_bus_read32(&ps->bus, PSEMU_CLK_BASE) & 0x10u);

    /* Bit 9 of INT_INPUT is the interrupt line of the RTC, which changes
       state. It is not a static flag. See test_rtc_defaults_and_increment for
       its behavior. */

    psemu_destroy(ps);
    printf("test_boot_ready_stub OK\n");
}

static void test_clk_mode_scales_run_speed(void) {
    /* Real hardware executes more raw instructions in each real frame at a
       higher CLK_MODE. A trace of a real boot and sound sequence confirms
       this: the BIOS sets mode 7 (approximately 4MHz, from the CLK_MODE and
       SetCpuSpeed table in clk.c) for the full HELLO, heart, and sound
       period. It then changes to a slower mode. The cycle budget of psemu_run
       uses the PSEMU_ASSUMED_CPU_HZ reference rate. Thus the maximum CLK_MODE
       executes more raw cycles in the same budget than the idle default,
       which is mode 0. The Timer also scales with CLK_MODE (see
       test_timer_scales_with_clk_mode). The RTC and the DAC stay at real time
       (see test_clk_mode_keeps_rtc_dac_on_real_time). */
    psemu_t *ps_idle = make_arm_cpu();
    psemu_t *ps_max = make_arm_cpu();
    ps_idle->has_bios = 1; /* psemu_run is a no-op without a loaded BIOS */
    ps_max->has_bios = 1;

    psemu_bus_write32(&ps_max->bus, PSEMU_CLK_BASE, 7u);

    uint32_t ran_idle = psemu_run(ps_idle, 100000u);
    uint32_t ran_max = psemu_run(ps_max, 100000u);

    /* Mode 7 (approximately 4MHz) is approximately 122 times mode 0
       (approximately 32.768kHz). This test uses a lower limit of 10 times.
       Thus the test does not depend on the exact table values, and it still
       finds a psemu_run function that applies no scale at all. */
    assert(ran_max > ran_idle * 10u);

    psemu_destroy(ps_idle);
    psemu_destroy(ps_max);
    printf("test_clk_mode_scales_run_speed OK\n");
}

static void test_timer_scales_with_clk_mode(void) {
    /* The history of this function. See docs/hardware-notes.md, "CLK_MODE",
       for the current behavior. Earlier work two times made the Timer stay at
       real time, the same as the RTC and the DAC. The reasoning was that the
       Timer operates the Timer1 IRQ audio loop of the app, and thus it must
       not advance faster than real time. That change corrected a real report
       that a sound played much too fast. But a later direct measurement showed
       that the change broke a different function: with the Timer independent
       of CLK_MODE, the HELLO animation was approximately 4 times too slow at
       CLK_MODE 7. The same Timer1 heartbeat drives that animation, and both
       the audio and the general GUI ticks are confirmed real uses of the same
       IRQ. Also, the blink on a date-setting screen was approximately 2 times
       too fast at CLK_MODE 4. Both errors agree almost exactly with the ratio
       between the real Hz value of those CLK_MODE settings and the fixed
       reference rate.
       The System Clock clocks the real timers, and that clock changes with
       CLK_MODE. Thus the Timer now follows CLK_MODE, the same as the
       throughput of the outer loop. The earlier "sound too fast" report is an
       inference: it came from the separate DAC output-pacing fault that
       test_clk_mode_keeps_rtc_dac_on_real_time below now covers. It was not a
       result of a Timer that follows CLK_MODE. */
    psemu_t *ps_idle = make_arm_cpu();
    psemu_t *ps_max = make_arm_cpu();
    ps_idle->has_bios = 1;
    ps_max->has_bios = 1;

    psemu_bus_write32(&ps_max->bus, PSEMU_CLK_BASE, 7u);

    /* The period and the count are large. Thus the timer never expires, and
       it never reloads or wraps, in the budget of this test, at either
       CLK_MODE value. Thus the difference (initial count - final count), in
       ticks, gives the total raw cycles. cycle_accumulator cannot give that
       total, because it holds only the remainder below the divisor.
       This value is the largest period that the hardware can hold: these
       registers are 16-bit (see TIMER_REG_MASK in timer.h). This test loaded
       100000000 before, and no real timer can store that value. After the
       emulator started to mask the value to the real width, that value
       wrapped almost immediately, and the tick arithmetic below had no
       meaning.
       The budget is one half of the earlier budget, to agree with this value.
       At 0xFFFF and the /2 divisor, mode 7 accumulates approximately 50000 of
       the 65535 available ticks over approximately 0.05s. Thus the counter
       still never wraps. */
    uint32_t big = TIMER_REG_MASK;
    psemu_bus_write32(&ps_idle->bus, PSEMU_TIMER_BASE + 0x10, big); /* T1 period */
    psemu_bus_write32(&ps_idle->bus, PSEMU_TIMER_BASE + 0x14, big); /* T1 count */
    psemu_bus_write32(&ps_idle->bus, PSEMU_TIMER_BASE + 0x18, TIMER_CTRL_ENABLE);
    psemu_bus_write32(&ps_max->bus, PSEMU_TIMER_BASE + 0x10, big);
    psemu_bus_write32(&ps_max->bus, PSEMU_TIMER_BASE + 0x14, big);
    psemu_bus_write32(&ps_max->bus, PSEMU_TIMER_BASE + 0x18, TIMER_CTRL_ENABLE);

    uint32_t budget = PSEMU_ASSUMED_CPU_HZ / 20u; /* ~0.05 real second */
    psemu_run(ps_idle, budget);
    psemu_run(ps_max, budget);

    uint32_t idle_ticks = big - ps_idle->timer.timers[1].count;
    uint32_t max_ticks = big - ps_max->timer.timers[1].count;

    /* Neither run may have wrapped, or the subtraction above is not a tick count. */
    assert(ps_idle->timer.timers[1].count <= big);
    assert(ps_max->timer.timers[1].count <= big);

    /* Mode 7 is ~4MHz vs mode 0's ~32.768kHz - Timer1 should have ticked
       much further under the elevated clock. */
    assert(max_ticks > idle_ticks * 10u);

    psemu_destroy(ps_idle);
    psemu_destroy(ps_max);
    printf("test_timer_scales_with_clk_mode OK\n");
}

static void test_clk_mode_keeps_rtc_dac_on_real_time(void) {
    /* The Timer follows CLK_MODE (see test_timer_scales_with_clk_mode), but
       the RTC does not. The RTC is a separate real 1Hz oscillator, and it is
       independent of the CPU clock. A test on real hardware confirms this:
       the RTC ticks at a constant rate for each CPU_FREQ value. The DAC
       resample function of this emulator needs the same independence from the
       CPU clock, for its fixed PSEMU_DAC_SAMPLE_RATE_HZ output rate. Thus, for
       each CLK_MODE value, the RTC and the DAC must advance by the same
       quantity for the same real-time budget. */
    psemu_t *ps_idle = make_arm_cpu();
    psemu_t *ps_max = make_arm_cpu();
    ps_idle->has_bios = 1;
    ps_max->has_bios = 1;

    psemu_bus_write32(&ps_max->bus, PSEMU_CLK_BASE, 7u);

    uint32_t budget = PSEMU_ASSUMED_CPU_HZ / 10u; /* ~0.1 real second */
    psemu_run(ps_idle, budget);
    psemu_run(ps_max, budget);

    long rtc_diff = (long)ps_idle->rtc.tick_accumulator - (long)ps_max->rtc.tick_accumulator;
    /* Only a small overshoot at the last step. But this limit is larger than
       the earlier limit, because arm7tdmi_step now returns a real cycle cost
       that depends on the memory region (see docs/hardware-notes.md, "Memory
       access timing"). It does not return a flat value of 1. The instruction
       stream of this test is all zeros, and it continues past the end of the
       2KB WRAM region during the budget. After that point, each step costs 2
       raw cycles and not 1. Thus the worst-case overshoot of one step can be
       two times the earlier value. The observed value is approximately 38.
       The earlier flat-cost model needed less than 20. */
    assert(rtc_diff > -60 && rtc_diff < 60);

    int16_t buf_idle[4096], buf_max[4096];
    uint32_t n_idle = psemu_get_audio_samples(ps_idle, buf_idle, 4096u);
    uint32_t n_max = psemu_get_audio_samples(ps_max, buf_max, 4096u);
    long sample_diff = (long)n_idle - (long)n_max;
    assert(sample_diff > -2 && sample_diff < 2);

    psemu_destroy(ps_idle);
    psemu_destroy(ps_max);
    printf("test_clk_mode_keeps_rtc_dac_on_real_time OK\n");
}

static void test_rtc_defaults_and_increment(void) {
    psemu_t *ps = make_arm_cpu();

    /* The power-on-reset values of real silicon: the date is 1998-01-01, and
       the time is 00:00:00 with the day of the week as BCD 4. See rtc.h for
       the reason that these values are not the earlier assumption of
       1999-01-01. */
    assert(psemu_bus_read32(&ps->bus, PSEMU_RTC_BASE + 0xC) == 0x00980101u); /* date: day,month,year,(unused) */
    assert(psemu_bus_read32(&ps->bus, PSEMU_RTC_BASE + 0x8) == 0x04000000u); /* time: sec,min,hour,dow */

    /* A write of 1 to control while control holds 1 increases the field that
       mode>>1 selects. The value 4 selects the day. This is the real "write 1
       two times" method.
       Bit 0 of mode (PRGSEL) stays clear, and 0 is the paused or program
       mode. Thus the automatic-advance test below does not also change `date`
       through the manual write path that this test covers. */
    psemu_bus_write32(&ps->bus, PSEMU_RTC_BASE + 0x0, (4u << 1) | 1u); /* mode = day, paused */
    psemu_bus_write8(&ps->bus, PSEMU_RTC_BASE + 0x4, 1u);       /* control: 0 -> 1, just stored */
    assert(psemu_bus_read32(&ps->bus, PSEMU_RTC_BASE + 0xC) == 0x00980101u); /* unchanged so far */
    psemu_bus_write8(&ps->bus, PSEMU_RTC_BASE + 0x4, 1u);       /* control: 1 -> increments day */
    assert(psemu_bus_read32(&ps->bus, PSEMU_RTC_BASE + 0xC) == 0x00980102u); /* day is now 2 */
    assert(psemu_bus_read8(&ps->bus, PSEMU_RTC_BASE + 0x4) == 0u);           /* control reset to 0 */

    /* The real BIOS waits for a full pulse on the RTC line in the INTC status
       register. A full pulse is a rise and then a fall. The BIOS does not
       wait for a level, thus a constant value cannot satisfy the wait.
       The RTC is still paused, because bit 0 of mode is set above. Thus this
       test covers the faster paused tick rate of approximately 4096Hz, and
       the RTC must NOT advance the time. */
    uint32_t time_before_tick = psemu_bus_read32(&ps->bus, PSEMU_RTC_BASE + 0x8);
    rtc_tick(&ps->rtc, &ps->intc, RTC_TICK_CYCLES_PAUSED);
    assert(intc_get_line(&ps->intc, INT_RTC) != 0u);
    assert((psemu_bus_read32(&ps->bus, PSEMU_INTC_BASE + 0x4) & INT_RTC) != 0u);
    rtc_tick(&ps->rtc, &ps->intc, RTC_TICK_CYCLES_PAUSED);
    assert(intc_get_line(&ps->intc, INT_RTC) == 0u);
    assert((psemu_bus_read32(&ps->bus, PSEMU_INTC_BASE + 0x4) & INT_RTC) == 0u);
    assert(psemu_bus_read32(&ps->bus, PSEMU_RTC_BASE + 0x8) == time_before_tick); /* paused: no auto-advance */

    /* Change to the running mode (bit 0 clear), and confirm two conditions:
       the tick pulses INT_RTC, and the tick advances the seconds field. This
       emulator did not have the second behavior before the correction. Only
       the interrupt line changed state, and the clock never advanced. */
    psemu_bus_write32(&ps->bus, PSEMU_RTC_BASE + 0x0, 0u); /* mode = running */
    /* Two transitions, because one second is one full pulse, and the hardware
       makes two transitions for each pulse (see rtc.h). One transition alone
       must NOT advance the clock. */
    rtc_tick(&ps->rtc, &ps->intc, RTC_TICK_CYCLES_RUN);
    assert((psemu_bus_read32(&ps->bus, PSEMU_RTC_BASE + 0x8) & 0xFFu) == 0x00u);
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

static void test_flash_bank_val_remapping(void) {
    /* The index of F_BANK_VAL is the PHYSICAL bank: table[p] = v. This is the
       opposite direction from a usual page table (see
       docs/hardware-notes.md, "Flash memory"). The FLASH1 window is a real
       remapping table, and it can change the order of the blocks. It is not a
       simple linear offset. This test covers a mapping that is not
       contiguous: physical blocks 2 and 5 are enabled, block 5 goes to
       virtual bank 0, and block 2 goes to virtual bank 1. That order is the
       reverse of the order that a linear-offset model gives. */
    psemu_t *ps = make_arm_cpu();

    psemu_bus_write32(&ps->bus, PSEMU_FLASH2_BASE + 2 * 8192, 0x22222222u);
    psemu_bus_write32(&ps->bus, PSEMU_FLASH2_BASE + 5 * 8192, 0x55555555u);

    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 8, (1u << 2) | (1u << 5)); /* F_BANK_FLG */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x100 + 5 * 4, 0u);        /* F_BANK_VAL[5] = virtual 0 */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x100 + 2 * 4, 1u);        /* F_BANK_VAL[2] = virtual 1 */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0, 2u);                    /* commit */

    /* Virtual bank 0 (FLASH1 offset 0) goes to physical block 5. It does not
       go to the enabled block with the lowest number (block 2), which a
       linear-offset model selects. */
    assert(psemu_bus_read32(&ps->bus, PSEMU_FLASH1_BASE) == 0x55555555u);
    /* Virtual bank 1 (FLASH1 offset 8192) -> physical block 2. */
    assert(psemu_bus_read32(&ps->bus, PSEMU_FLASH1_BASE + 8192) == 0x22222222u);

    /* Writes respect the same remapping. */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH1_BASE + 4, 0x99999999u);
    assert(psemu_bus_read32(&ps->bus, PSEMU_FLASH2_BASE + 5 * 8192 + 4) == 0x99999999u);

    psemu_destroy(ps);
    printf("test_flash_bank_val_remapping OK\n");
}

static void test_flash_ctrl_busy_wait_bits(void) {
    psemu_t *ps = make_arm_cpu();

    /* Two real, confirmed faults. Both are busy-wait loops in real BIOS code
       or app code, and this emulator made both loops continue for an
       unlimited time with no message. This project found the faults only
       after real app execution got to them (see the app-dispatch
       investigation in docs/app-notes.md). */

    /* Fault 1: on real hardware, +0 is a write-command and read-status
       register. It is not a simple mirror. A real routine writes 2 here, and
       then waits for bit 0 of this same address to read back as 1 ("ready").
       A return of the raw command value gives bit 0 as 0, because the command
       is 2. Thus that loop continued for an unlimited time. */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0, 2u);
    assert((psemu_bus_read32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0) & 1u) != 0u);

    /* Fault 2: this emulator did not model +0x10 (F_WAIT2, which holds the
       waitstates and the flash write control and status). The span stopped at
       +0xC. The flash-write routine of a real app reads bit 2 here, and waits
       for the bit to read back as set after the write completes. An unmapped
       read gave a default of 0, thus that loop also continued for an unlimited
       time. Writes complete immediately here, thus this register must always
       report "not busy". */
    assert((psemu_bus_read32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x10) & 0x04u) != 0u);

    psemu_destroy(ps);
    printf("test_flash_ctrl_busy_wait_bits OK\n");
}

static void test_flash_serial_number_default_and_override(void) {
    /* F_SN (see docs/hardware-notes.md, "Hardware ID (F_SN)") has a default
       of 0x410000D3, which is "410000D3" in hex form. Its low 24 bits are
       211. The companion app of one console game removes the high byte, reads
       the remainder with SWI 0Ah, and uses the last 3 decimal digits as an
       "ID" statistic. That statistic alone sets the rank, and public research
       gives 211 as the best rank. The high byte ('A') is the ASCII letter
       that real hardware prints on its serial sticker. See
       psemu_parse_hardware_id and psemu_format_hardware_id. */
    psemu_t *ps = make_arm_cpu();
    assert(psemu_get_hardware_id(ps) == (((uint32_t)'A' << 24) | 211u));

    psemu_set_hardware_id(ps, 0x12345678u);
    assert(psemu_get_hardware_id(ps) == 0x12345678u);

    psemu_destroy(ps);
    printf("test_flash_serial_number_default_and_override OK\n");
}

static void test_hardware_id_string_conversion(void) {
    /* The only accepted form is exactly 8 hex digits. A real homebrew "ID
       rewriter" app shows and changes this same form on real hardware. A test
       on real hardware confirms that the first digit does not have to be a
       letter: a real unit accepts and keeps the value "EEEEEEEE". This code
       deliberately does NOT accept the "sticker" form of one letter and 8
       decimal digits that real units print below the front cover, for example
       "A02374684". A hardware-ID string that this app keeps holds the raw
       value exactly. It hides nothing and translates nothing. A converter
       from the sticker form to the raw value belongs in the desktop app, as a
       separate function. See docs/hardware-notes.md, "Hardware ID (F_SN)". */
    uint32_t id;
    char buf[PSEMU_HARDWARE_ID_STRING_SIZE];

    assert(psemu_parse_hardware_id("EEEEEEEE", &id) != 0);
    assert(id == 0xEEEEEEEEu);
    psemu_format_hardware_id(id, buf, sizeof(buf));
    assert(strcmp(buf, "EEEEEEEE") == 0);

    /* Case-insensitive on input; output is always uppercase. */
    assert(psemu_parse_hardware_id("410000d3", &id) != 0);
    assert(id == 0x410000D3u);
    psemu_format_hardware_id(id, buf, sizeof(buf));
    assert(strcmp(buf, "410000D3") == 0);

    /* Round-trips the core default too. */
    psemu_format_hardware_id((((uint32_t)'A' << 24) | 211u), buf, sizeof(buf));
    assert(strcmp(buf, "410000D3") == 0);
    assert(psemu_parse_hardware_id("410000D3", &id) != 0);
    assert(id == (((uint32_t)'A' << 24) | 211u));

    /* Malformed input is rejected outright, not silently truncated/guessed
       or reinterpreted as some other format. */
    assert(psemu_parse_hardware_id("410000D", &id) == 0);    /* too short */
    assert(psemu_parse_hardware_id("410000D30", &id) == 0);  /* too long */
    assert(psemu_parse_hardware_id("410000DG", &id) == 0);   /* not a valid hex digit */
    assert(psemu_parse_hardware_id("A02374684", &id) == 0);  /* sticker form - no longer accepted here */
    assert(psemu_parse_hardware_id(NULL, &id) == 0);

    printf("test_hardware_id_string_conversion OK\n");
}

static void test_flash_serial_number_register_access(void) {
    /* F_SN_LO and F_SN_HI are at FLASH_CTRL+0x300 and FLASH_CTRL+0x302
       (F_EXTRA, see flash.h). Real hardware needs two separate 16-bit
       halfword loads for these registers. It does not accept one 32-bit load.
       But the bytes have the same addresses in both conditions. This test
       uses the halfword size that real code uses. A homebrew ID editor writes
       these registers directly, and does not use the SWI. Thus the bus path
       must operate independently of psemu_set_hardware_id. */
    psemu_t *ps = make_arm_cpu();

    /* The default value is "A00000211" (see FLASH_DEFAULT_SERIAL). F_SN_LO
       holds the low 16 bits, which are 211, and the high byte has no effect
       on them. F_SN_HI holds the high 16 bits, which include the letter byte
       'A' (0x41) in its own upper half. */
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x300) == 211u);
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x302) == 0x4100u);

    psemu_bus_write16(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x300, 0xBEEFu);
    psemu_bus_write16(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x302, 0xCAFEu);
    assert(psemu_get_hardware_id(ps) == 0xCAFEBEEFu);

    /* F_CAL (+0x308) is a real, separate register in the same F_EXTRA region.
       It has the recorded reset value as its default, and code can read it
       and write it independently. */
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x308) == 0x001Au);
    psemu_bus_write16(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x308, 0x0099u);
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x308) == 0x0099u);

    /* The range between the end of F_BANK_VAL (+0x140) and the start of
       F_EXTRA (+0x300) is unmapped. It must stay at 0. It must not mirror
       last_command, because FLASH_CTRL_SPAN now continues to F_EXTRA. */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0, 2u); /* nonzero last_command */
    assert(psemu_bus_read32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x200) == 0u);

    /* Same for unknown bytes inside F_EXTRA itself, e.g. the reserved
       halfword between F_SN_HI and F_CAL. */
    assert(psemu_bus_read32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x304) == 0u);

    psemu_destroy(ps);
    printf("test_flash_serial_number_register_access OK\n");
}

/* Builds a card with one named save. `data_fill` fills the data area of that
   save. Thus a test can change the stored data without a change to the identity
   of the card. */
static void make_identity_card(uint8_t *card, const char *name, uint8_t data_fill) {
    memset(card, 0, PSEMU_FLASH_SIZE);
    memcpy(card, "MC", 2);
    card[128] = 0x51u; /* frame 1: in use, first block of a file */
    memcpy(&card[128 + 0x0A], name, strlen(name));
    {
        uint32_t frame;
        for (frame = 2; frame < 16u; frame++) {
            card[frame * 128u] = 0xA0u; /* free */
        }
    }
    memset(&card[FLASH_BLOCK_SIZE], data_fill, FLASH_BLOCK_SIZE);
    memcpy(&card[FLASH_BLOCK_SIZE], "SC", 2);
}

static void test_content_identity_hash_survives_a_save(void) {
    /* The purpose of this function: a save state must continue to agree with
       its own card after an app writes to that card. A frontend now writes the
       card back to disk. A hash of the file cannot give this result, because
       the file is the data that changed. See psemu_content_identity_hash. */
    static uint8_t card[PSEMU_FLASH_SIZE];
    static uint8_t other[PSEMU_FLASH_SIZE];
    static uint8_t app[8192];
    static uint8_t mcs[128 + 8192];
    uint32_t before, after;

    make_identity_card(card, "BASLUS-01411-YUGIOH", 0x11);
    before = psemu_content_identity_hash(card, sizeof(card));

    /* The app saves: data changes all over the card's data blocks, exactly as a real trade does. */
    memset(&card[FLASH_BLOCK_SIZE + 0x200], 0x99, 0x400);
    card[FLASH_BLOCK_SIZE + 0x259] = 0x01u;
    after = psemu_content_identity_hash(card, sizeof(card));
    assert(before == after);

    /* A different card is still a different card. */
    make_identity_card(other, "BASCUS-94163-CHOCOBO", 0x11);
    assert(psemu_content_identity_hash(other, sizeof(other)) != before);

    /* The same card with an added file or a removed file also gives a different
       hash. That change is a real change of identity, and not a change of the
       stored data. Thus a refusal of a state across that change is correct. */
    make_identity_card(other, "BASLUS-01411-YUGIOH", 0x11);
    other[2 * 128u] = 0x51u;
    assert(psemu_content_identity_hash(other, sizeof(other)) != before);

    /* A bare app: its title-sector metadata identifies it, and its own blocks are free to change. */
    memset(app, 0, sizeof(app));
    memcpy(&app[0x52], "MCX0", 4);
    memcpy(&app[0x04], "A TEST APP", 10);
    app[0x02] = 0x11u; /* one standard icon frame */
    memset(&app[0x80], 0x5Au, 128); /* the icon bitmap */
    before = psemu_content_identity_hash(app, sizeof(app));
    memset(&app[0x600], 0xEE, 0x100); /* the app saves, well past its icon */
    assert(psemu_content_identity_hash(app, sizeof(app)) == before);
    memcpy(&app[0x04], "B TEST APP", 10); /* a different app entirely */
    assert(psemu_content_identity_hash(app, sizeof(app)) != before);
    memcpy(&app[0x04], "A TEST APP", 10);
    app[0x80] ^= 0xFFu; /* a different icon, same title */
    assert(psemu_content_identity_hash(app, sizeof(app)) != before);
    app[0x80] ^= 0xFFu;

    /* A .mcs identifies by its directory frame as well as its body. */
    memset(mcs, 0, sizeof(mcs));
    mcs[0x00] = 0x51u;
    mcs[0x04] = (uint8_t)(8192u & 0xFFu);
    mcs[0x05] = (uint8_t)((8192u >> 8) & 0xFFu);
    memcpy(&mcs[0x0A], "BESLES-99999-TEST", 17);
    memcpy(&mcs[128], app, sizeof(app));
    assert(psemu_identify_content(mcs, sizeof(mcs)) == PSEMU_CONTENT_MCS);
    before = psemu_content_identity_hash(mcs, sizeof(mcs));
    memset(&mcs[128 + 0x600], 0x77, 0x100); /* the app saves */
    assert(psemu_content_identity_hash(mcs, sizeof(mcs)) == before);
    memcpy(&mcs[0x0A], "BESLES-11111-TEST", 17); /* a different file on the card */
    assert(psemu_content_identity_hash(mcs, sizeof(mcs)) != before);

    printf("test_content_identity_hash_survives_a_save OK\n");
}

static void test_flash_load_app_synthesizes_directory(void) {
    /* A real, confirmed fault (see docs/app-notes.md, "App-selection and
       dispatch"). The app-selection routine of the real BIOS needs a real
       memory-card directory in FLASH2. The bytes of the app at offset 0 are
       not sufficient. flash_load_app wrote the raw Title Sector directly to
       offset 0 before. Thus the real menu could never reach a loaded single
       app. This test protects the correction: a synthesized directory with
       one entry at slot 1, and the specific byte that the menu-browsing code
       of the real BIOS needs (frame offset 0x10 = 'P'). See the comment on
       DIRECTORY_POCKETSTATION_FLAG_OFFSET in flash.c for the method that
       isolated that byte against a real card dump. */
    psemu_t *ps = make_arm_cpu();

    uint8_t app[2 * 8192];
    memset(app, 0, sizeof(app));
    memcpy(&app[0x52], "MCX0", 4);
    app[0] = 0xAAu;              /* marks the start of block 1's data */
    app[8192] = 0xBBu;           /* marks the start of block 2's data */
    assert(psemu_load_app(ps, app, sizeof(app)) == PSEMU_OK);

    /* Card header frame. */
    assert(psemu_bus_read8(&ps->bus, PSEMU_FLASH2_BASE + 0x00) == 'M');
    assert(psemu_bus_read8(&ps->bus, PSEMU_FLASH2_BASE + 0x01) == 'C');

    /* Slot 1 is the first directory frame, at FLASH2 + 1*128. It holds the
       in-use and first marker, the real file size, the PocketStation flag
       byte, and a link to slot 2. That link is the 0-based data-block index
       1, because this is a chain of 2 blocks. */
    assert(psemu_bus_read8(&ps->bus, PSEMU_FLASH2_BASE + 128) == 0x51u);
    assert(psemu_bus_read32(&ps->bus, PSEMU_FLASH2_BASE + 128 + 0x04) == sizeof(app));
    assert(psemu_bus_read8(&ps->bus, PSEMU_FLASH2_BASE + 128 + 0x10) == 'P');
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH2_BASE + 128 + 0x08) == 1u);

    /* Slot 2 (last frame of the chain): end-of-chain marker and sentinel
       link, no filesize (only the first frame of a file carries it). */
    assert(psemu_bus_read8(&ps->bus, PSEMU_FLASH2_BASE + 256) == 0x53u);
    assert(psemu_bus_read32(&ps->bus, PSEMU_FLASH2_BASE + 256 + 0x04) == 0u);
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH2_BASE + 256 + 0x08) == 0xFFFFu);

    /* Slot 3 onward: free, matching a blank real card (see BlankMCD-style
       real card layout). */
    assert(psemu_bus_read8(&ps->bus, PSEMU_FLASH2_BASE + 384) == 0xA0u);
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH2_BASE + 384 + 0x08) == 0xFFFFu);

    /* The app's own data starts at physical block 1 (right after the
       directory block), not at offset 0. */
    assert(psemu_bus_read8(&ps->bus, PSEMU_FLASH2_BASE + FLASH_BLOCK_SIZE) == 0xAAu);
    assert(psemu_bus_read8(&ps->bus, PSEMU_FLASH2_BASE + FLASH_BLOCK_SIZE + 8192) == 0xBBu);
    assert(psemu_bus_read8(&ps->bus, PSEMU_FLASH2_BASE + FLASH_BLOCK_SIZE + 0x52) == 'M');

    psemu_destroy(ps);
    printf("test_flash_load_app_synthesizes_directory OK\n");
}

static void test_flash_load_app_rejects_oversized_app(void) {
    /* flash_load_app now keeps physical block 0 for the synthesized directory
       (see test_flash_load_app_synthesizes_directory above). Thus the maximum
       app size is 15 blocks, and not the full 16 blocks of flash. That is one
       block less than the earlier limit. This test protects the exact limit,
       thus a later change cannot make it larger or smaller without a test
       failure. */
    psemu_t *ps = make_arm_cpu();

    static uint8_t max_size_app[15 * 8192];
    memset(max_size_app, 0, sizeof(max_size_app));
    memcpy(&max_size_app[0x52], "MCX0", 4);
    assert(psemu_load_app(ps, max_size_app, sizeof(max_size_app)) == PSEMU_OK);

    static uint8_t oversized_app[15 * 8192 + 1];
    memset(oversized_app, 0, sizeof(oversized_app));
    memcpy(&oversized_app[0x52], "MCX0", 4);
    assert(psemu_load_app(ps, oversized_app, sizeof(oversized_app)) == PSEMU_ERR_BAD_SIZE);

    psemu_destroy(ps);
    printf("test_flash_load_app_rejects_oversized_app OK\n");
}

static void test_psemu_load_mcs_validates_and_unwraps(void) {
    /* psemu_load_mcs had no direct test. Only a manual test covered it,
       during the work that added .mcs support. This test protects two
       properties. First, a correct single-save .mcs file, which is a real PS1
       directory frame and then data blocks, gives the same
       synthesized-directory layout that psemu_load_app gives. Second, this
       code refuses an incorrect .mcs file in three conditions: the file is
       too short for a directory frame, the payload is not a whole number of
       blocks, and the file size in the directory frame does not agree with
       the payload. */
    psemu_t *ps = make_arm_cpu();

    uint8_t mcs[0x80 + 8192];
    memset(mcs, 0, sizeof(mcs));
    mcs[0] = 0x51; /* directory frame: in-use/first marker */
    uint32_t payload_size = 8192;
    mcs[0x04] = (uint8_t)(payload_size & 0xFFu);
    mcs[0x05] = (uint8_t)((payload_size >> 8) & 0xFFu);
    mcs[0x06] = (uint8_t)((payload_size >> 16) & 0xFFu);
    mcs[0x07] = (uint8_t)((payload_size >> 24) & 0xFFu);
    memcpy(&mcs[0x80 + 0x52], "MCX0", 4); /* Title Sector magic, inside the payload */
    assert(psemu_load_mcs(ps, mcs, sizeof(mcs)) == PSEMU_OK);
    /* Unwrapped the same way psemu_load_app would have: synthesized
       directory at slot 1, app data starting at physical block 1. */
    assert(psemu_bus_read8(&ps->bus, PSEMU_FLASH2_BASE + 128) == 0x51u);
    assert(psemu_bus_read8(&ps->bus, PSEMU_FLASH2_BASE + FLASH_BLOCK_SIZE + 0x52) == 'M');

    /* Too short to even contain a full directory frame. */
    uint8_t too_short[0x80];
    memset(too_short, 0, sizeof(too_short));
    assert(psemu_load_mcs(ps, too_short, sizeof(too_short)) == PSEMU_ERR_BAD_SIZE);

    /* Payload present but not a whole number of 8192-byte blocks. */
    uint8_t misaligned[0x80 + 100];
    memset(misaligned, 0, sizeof(misaligned));
    assert(psemu_load_mcs(ps, misaligned, sizeof(misaligned)) == PSEMU_ERR_BAD_SIZE);

    /* Directory frame's stored size doesn't match the actual payload. */
    uint8_t bad_size_field[0x80 + 8192];
    memcpy(bad_size_field, mcs, sizeof(bad_size_field));
    bad_size_field[0x04] = (uint8_t)((payload_size - 1) & 0xFFu);
    assert(psemu_load_mcs(ps, bad_size_field, sizeof(bad_size_field)) == PSEMU_ERR_BAD_FORMAT);

    psemu_destroy(ps);
    printf("test_psemu_load_mcs_validates_and_unwraps OK\n");
}

static void test_psemu_load_content_dispatches_by_size(void) {
    /* psemu_load_content holds the .mcr, .mcs, and .pss priority dispatch in
       one location. Both frontends contained a copy of that logic before, and
       the two copies became different one time, at a change to the priority
       order. This test protects all four results: a full card, a .mcs file, a
       .pss file, and data that no loader accepts. */

    /* A full memory-card image goes directly to psemu_load_flash_image. This
       code does not read it as a Title Sector. A synthesized directory writes
       'M' and 'C' at offset 0. Thus a byte pattern that does not change at
       that offset proves that the code used the raw path. */
    {
        psemu_t *ps = make_arm_cpu();
        static uint8_t card[PSEMU_FLASH_SIZE];
        memset(card, 0, sizeof(card));
        card[0] = 0xABu;
        assert(psemu_load_content(ps, card, sizeof(card)) == PSEMU_OK);
        assert(psemu_bus_read8(&ps->bus, PSEMU_FLASH2_BASE + 0) == 0xABu);
        psemu_destroy(ps);
    }

    /* Input in the .mcs form, which is a directory frame and a block-aligned
       payload. This code extracts the payload and makes a directory, the same
       way that psemu_load_mcs does. */
    {
        psemu_t *ps = make_arm_cpu();
        uint8_t mcs[0x80 + 8192];
        memset(mcs, 0, sizeof(mcs));
        mcs[0] = 0x51;
        uint32_t payload_size = 8192;
        mcs[0x04] = (uint8_t)(payload_size & 0xFFu);
        mcs[0x05] = (uint8_t)((payload_size >> 8) & 0xFFu);
        memcpy(&mcs[0x80 + 0x52], "MCX0", 4);
        assert(psemu_load_content(ps, mcs, sizeof(mcs)) == PSEMU_OK);
        assert(psemu_bus_read8(&ps->bus, PSEMU_FLASH2_BASE + 128) == 0x51u);
        assert(psemu_bus_read8(&ps->bus, PSEMU_FLASH2_BASE + FLASH_BLOCK_SIZE + 0x52) == 'M');
        psemu_destroy(ps);
    }

    /* A Title Sector body, which is not in a valid .mcs form. The value
       8192 - 0x80 is not a multiple of FLASH_BLOCK_SIZE, thus psemu_load_mcs
       refuses the data on its size alone, and the code then tries
       psemu_load_app. This data still gets the synthesized directory that a
       .pss file always gets. */
    {
        psemu_t *ps = make_arm_cpu();
        uint8_t pss[8192];
        memset(pss, 0, sizeof(pss));
        memcpy(&pss[0x52], "MCX0", 4);
        assert(psemu_load_content(ps, pss, sizeof(pss)) == PSEMU_OK);
        assert(psemu_bus_read8(&ps->bus, PSEMU_FLASH2_BASE + 128) == 0x51u);
        assert(psemu_bus_read8(&ps->bus, PSEMU_FLASH2_BASE + FLASH_BLOCK_SIZE + 0x52) == 'M');
        psemu_destroy(ps);
    }

    /* Neither shape - too small to be anything. */
    {
        psemu_t *ps = make_arm_cpu();
        uint8_t garbage[50];
        memset(garbage, 0, sizeof(garbage));
        assert(psemu_load_content(ps, garbage, sizeof(garbage)) != PSEMU_OK);
        psemu_destroy(ps);
    }

    printf("test_psemu_load_content_dispatches_by_size OK\n");
}

static void test_flash_key_addresses_are_not_data_storage(void) {
    /* A real, confirmed fault that a real crash report found (see
       docs/hardware-notes.md, "Flash memory").
       F_KEY1 (0x08002A54) and F_KEY2 (0x080055AA) are the flash
       unlock-sequence trigger addresses of real hardware. They are not data
       storage. A real flash chip intercepts writes there as unlock commands,
       and it does not store them. This emulator stored them as usual data
       before. Thus the unlock sequence of each real flash write permanently
       corrupted the byte at those two fixed addresses. For one commercial
       app, that byte was live app code. */
    psemu_t *ps = make_arm_cpu();

    /* Real hardware writes these as a 16-bit halfword
       ("[8002A54h]=FF55h") - matching that access width here. */
    psemu_bus_write16(&ps->bus, PSEMU_FLASH2_BASE + 0x2A54, 0xFF55u);
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH2_BASE + 0x2A54) == 0x0000u);

    psemu_bus_write16(&ps->bus, PSEMU_FLASH2_BASE + 0x55AA, 0xFFAAu);
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH2_BASE + 0x55AA) == 0x0000u);

    /* Nearby ordinary writes must still work - the guard is narrowly
       targeted, not accidentally blocking a wider range. */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH2_BASE + 0x2A50, 0x33333333u);
    assert(psemu_bus_read32(&ps->bus, PSEMU_FLASH2_BASE + 0x2A50) == 0x33333333u);

    /* The same protection applies through the FLASH1 virtual window. That
       window uses the same data array directly, and it does not call
       flash_write8. With the default bank state, FLASH1 offset 0x2A54
       resolves to the same physical offset. */
    psemu_bus_write16(&ps->bus, PSEMU_FLASH1_BASE + 0x2A54, 0xFF55u);
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH1_BASE + 0x2A54) == 0x0000u);

    psemu_destroy(ps);
    printf("test_flash_key_addresses_are_not_data_storage OK\n");
}

/* A real, confirmed sequence. A disassembly of a real homebrew ID editor gives
   it, and it agrees with the available register description: "[8000000h]=new
   F_SN_LO value [8000002h]=new F_SN_HI value". A test on a real retail-BIOS unit
   confirms that the sequence operates correctly. See docs/hardware-notes.md,
   "Hardware ID (F_SN)". */
static void flash_perform_unlock_sequence(psemu_t *ps) {
    psemu_bus_write16(&ps->bus, PSEMU_FLASH2_BASE + 0x55AA, 0xFFAAu); /* F_KEY2 */
    psemu_bus_write16(&ps->bus, PSEMU_FLASH2_BASE + 0x2A54, 0xFF55u); /* F_KEY1 */
    psemu_bus_write16(&ps->bus, PSEMU_FLASH2_BASE + 0x55AA, 0xFFA0u); /* F_KEY2 again */
}

static void test_flash_header_write_via_unlock_sequence(void) {
    psemu_t *ps = make_arm_cpu();

    flash_perform_unlock_sequence(ps);
    psemu_bus_write16(&ps->bus, PSEMU_FLASH2_BASE + 0x0000, 0xBEEFu); /* new F_SN_LO */
    psemu_bus_write16(&ps->bus, PSEMU_FLASH2_BASE + 0x0002, 0xCAFEu); /* new F_SN_HI */
    psemu_bus_write16(&ps->bus, PSEMU_FLASH2_BASE + 0x0008, 0x002Au); /* new F_CAL */

    assert(psemu_get_hardware_id(ps) == 0xCAFEBEEFu);
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x308) == 0x002Au);

    psemu_destroy(ps);
    printf("test_flash_header_write_via_unlock_sequence OK\n");
}

/* The path that a PocketStation app uses to write the PS1 save of the console game on the same card.

   An app can read that save directly, because FLASH2 is the full card in memory with no window. But
   an app cannot write the save that way. Writes use kernel SWI 0x10, one frame of 128 bytes at a
   time. Reverse engineering of the J110 BIOS handler at 0x0400126C gives this sequence. That handler
   sets F_WAIT2 (FLASH_CTRL+0x10) to 0x21, does the three-step unlock above, copies 0x40 halfwords to
   FLASH2 + frame*128, and waits for bit 2 of F_WAIT2 to read back as set. It then *verifies* the
   write: it compares 32 words, and returns 0 for success or 1 for a difference. One trading-card app
   uses this handler from 0x020028FA. That app calculates the frame number as (save_pointer >> 7), and
   it tries a maximum of 10 times for each frame.

   Each step must operate correctly. If one step fails, the write fails and gives no message: a retry
   loop that is never successful simply stops. The mode-bit test that discarded IR transmissions had
   the same behavior. This test does the sequence directly, and not through the BIOS. Thus it needs no
   BIOS image and no app image, and it can execute in CI. The readback assertion is the same
   comparison that the real handler uses to decide success. */
static void test_flash_frame_write_lands_in_a_ps1_save_block(void) {
    psemu_t *ps = make_arm_cpu();
    /* Block 1 + 0x200: where a PS1 save's data starts, past its title/icon header. Frame 68 of the card. */
    const uint32_t frame = (0x2000u + 0x200u) / 128u;
    const uint32_t dest = PSEMU_FLASH2_BASE + frame * 128u;
    int i;

    /* Bit 2 of F_WAIT2 must read back set, or the BIOS's completion poll never exits. */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x10, 0x21u);
    assert((psemu_bus_read32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x10) & 0x04u) != 0u);

    flash_perform_unlock_sequence(ps);
    for (i = 0; i < 64; i++) {
        psemu_bus_write16(&ps->bus, dest + (uint32_t)i * 2u, (uint16_t)(0xC0DEu + i));
    }

    /* The handler's own success test: read the frame back and compare. */
    for (i = 0; i < 64; i++) {
        assert(psemu_bus_read16(&ps->bus, dest + (uint32_t)i * 2u) == (uint16_t)(0xC0DEu + i));
    }
    /* The unlock must not send save data to F_SN or F_CAL. Those registers cover FLASH2 offsets 0, 2,
       and 8. A frame at this position in the card must go to usual data storage. */
    assert(psemu_get_hardware_id(ps) == PSEMU_DEFAULT_HARDWARE_ID);

    psemu_destroy(ps);
    printf("test_flash_frame_write_lands_in_a_ps1_save_block OK\n");
}

static void test_flash_header_write_requires_unlock_first(void) {
    /* The safety property that makes this design conditional, and not
       unconditional: physical offsets 0, 2, and 8 are ALSO usual card-data
       storage. In the normal condition, they hold the directory header of
       block 0. The F_KEY1 and F_KEY2 corruption fault above also confirms
       that real save-write mechanisms, for example the mechanism of one
       commercial app, write to other addresses in this same physical range.
       Without the real unlock sequence immediately before it, a write to
       these offsets must operate as a usual data write. It must not go to
       F_SN or F_CAL. */
    psemu_t *ps = make_arm_cpu();
    uint32_t default_id = psemu_get_hardware_id(ps);

    psemu_bus_write16(&ps->bus, PSEMU_FLASH2_BASE + 0x0000, 0xBEEFu);
    psemu_bus_write16(&ps->bus, PSEMU_FLASH2_BASE + 0x0002, 0xCAFEu);

    assert(psemu_get_hardware_id(ps) == default_id); /* unchanged */
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH2_BASE + 0x0000) == 0xBEEFu); /* stored as plain data instead */
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH2_BASE + 0x0002) == 0xCAFEu);

    psemu_destroy(ps);
    printf("test_flash_header_write_requires_unlock_first OK\n");
}

static void test_flash_header_write_disarms_after_unrelated_write(void) {
    /* The armed state covers exactly one real write session. A real header
       update is 3 separate halfword writes after one unlock sequence. The
       armed state must not continue into later, different writes, only
       because those writes also go to offset 0, 2, or 8. */
    psemu_t *ps = make_arm_cpu();
    uint32_t default_id = psemu_get_hardware_id(ps);

    flash_perform_unlock_sequence(ps);
    psemu_bus_write16(&ps->bus, PSEMU_FLASH2_BASE + 0x0004, 0x1234u); /* unrelated offset - disarms */
    psemu_bus_write16(&ps->bus, PSEMU_FLASH2_BASE + 0x0000, 0xBEEFu); /* no unlock since disarm */

    assert(psemu_get_hardware_id(ps) == default_id); /* unchanged - not redirected */
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH2_BASE + 0x0000) == 0xBEEFu); /* plain data instead */

    psemu_destroy(ps);
    printf("test_flash_header_write_disarms_after_unrelated_write OK\n");
}

static void test_flash_header_write_requires_correct_key_order(void) {
    /* Wrong order (F_KEY1 before F_KEY2) never arms - matches how a real
       NOR flash unlock sequence requires its exact byte order. */
    psemu_t *ps = make_arm_cpu();
    uint32_t default_id = psemu_get_hardware_id(ps);

    psemu_bus_write16(&ps->bus, PSEMU_FLASH2_BASE + 0x2A54, 0xFF55u); /* F_KEY1 first - wrong */
    psemu_bus_write16(&ps->bus, PSEMU_FLASH2_BASE + 0x55AA, 0xFFAAu);
    psemu_bus_write16(&ps->bus, PSEMU_FLASH2_BASE + 0x2A54, 0xFF55u);
    psemu_bus_write16(&ps->bus, PSEMU_FLASH2_BASE + 0x0000, 0xBEEFu);

    assert(psemu_get_hardware_id(ps) == default_id);
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH2_BASE + 0x0000) == 0xBEEFu);

    psemu_destroy(ps);
    printf("test_flash_header_write_requires_correct_key_order OK\n");
}

static void test_lcd_mode_dison_and_rotate(void) {
    /* This emulator did not model LCD_MODE (0x0D000000) before. Bit 6 is
       DISON, which sets the display on or off. Bit 7 is ROT, which rotates
       the display 180 degrees, and hardware sets it for docked mode.
       psemu_get_framebuffer() now applies these bits to the VRAM. It does not
       return the raw VRAM. */
    psemu_t *ps = make_arm_cpu();
    const uint8_t *fb;

    psemu_bus_write32(&ps->bus, PSEMU_LCD_VRAM_BASE, 0x000000FFu); /* row 0 */
    psemu_bus_write32(&ps->bus, PSEMU_LCD_VRAM_BASE + 4, 0x0000FF00u); /* row 1 */

    /* Default (no LCD_MODE write yet): DISON assumed on, matching this
       emulator's previously-validated always-visible behavior. */
    fb = psemu_get_framebuffer(ps);
    assert(fb[0] == 0xFFu && fb[1] == 0x00u);
    assert(fb[4] == 0x00u && fb[5] == 0xFFu);

    /* DISON cleared: blank output regardless of VRAM contents. */
    psemu_bus_write32(&ps->bus, PSEMU_LCD_MODE_BASE, 0u);
    fb = psemu_get_framebuffer(ps);
    assert(fb[0] == 0u && fb[1] == 0u && fb[4] == 0u && fb[5] == 0u);

    /* ROT is set, and DISON is set again. The result is a rotation of 180
       degrees: the order of the rows is reversed, and the 32 bits of each row
       are reversed from left to right. Row 0 (0x000000FF, with little-endian
       bytes 0xFF, 00, 00, 00) becomes the last row, at offset 124 to 127,
       with its bits reversed (0xFF000000, with bytes 00, 00, 00, 0xFF). Row 1
       (0x0000FF00, with bytes 00, 0xFF, 00, 00) becomes the second row from
       the end, at offset 120 to 123, and it reverses to 0x00FF0000 (with
       bytes 00, 00, 0xFF, 00). */
    psemu_bus_write32(&ps->bus, PSEMU_LCD_MODE_BASE, LCD_MODE_DISON | LCD_MODE_ROT);
    fb = psemu_get_framebuffer(ps);
    assert(fb[124] == 0x00u && fb[125] == 0x00u && fb[126] == 0x00u && fb[127] == 0xFFu);
    assert(fb[120] == 0x00u && fb[121] == 0x00u && fb[122] == 0xFFu && fb[123] == 0x00u);

    psemu_destroy(ps);
    printf("test_lcd_mode_dison_and_rotate OK\n");
}

static void test_dac_basic(void) {
    psemu_t *ps = make_arm_cpu();
    int16_t samples[4];
    uint32_t n;

    /* Disabled (ctrl's enable bit clear): always silence, regardless of
       whatever DACV is held. */
    psemu_bus_write32(&ps->bus, PSEMU_DAC_BASE + 0x4, 0x100u << 6);
    dac_tick(&ps->dac, DAC_CYCLES_PER_SAMPLE);
    n = psemu_get_audio_samples(ps, samples, 4);
    assert(n == 1u);
    assert(samples[0] == 0);

    /* Enabled, positive DACV: rescaled to a full int16 range (*64). */
    psemu_bus_write32(&ps->bus, PSEMU_DAC_BASE + 0x0, 1u);
    psemu_bus_write32(&ps->bus, PSEMU_DAC_BASE + 0x4, 0x100u << 6);
    dac_tick(&ps->dac, DAC_CYCLES_PER_SAMPLE);
    n = psemu_get_audio_samples(ps, samples, 4);
    assert(n == 1u);
    assert(samples[0] == (int16_t)(0x100 * 64));

    /* Negative DACV (10-bit two's complement, -1 = 0x3FF) sign-extends
       correctly rather than reading as a large positive value. */
    psemu_bus_write32(&ps->bus, PSEMU_DAC_BASE + 0x4, 0x3FFu << 6);
    dac_tick(&ps->dac, DAC_CYCLES_PER_SAMPLE);
    n = psemu_get_audio_samples(ps, samples, 4);
    assert(n == 1u);
    assert(samples[0] == (int16_t)(-1 * 64));

    /* Real hardware has no fixed sample rate, because software controls
       DAC_DATA bit by bit. dac_tick resamples at a fixed internal rate. Thus
       N cycles must give N/DAC_CYCLES_PER_SAMPLE output samples. */
    dac_tick(&ps->dac, DAC_CYCLES_PER_SAMPLE * 3u);
    n = psemu_get_audio_samples(ps, samples, 4);
    assert(n == 3u);

    psemu_destroy(ps);
    printf("test_dac_basic OK\n");
}

static void test_iop_sound_gate_mutes_dac(void) {
    /* Two bits must be set before audio plays: DAC_CTRL bit 0, and bit 5 of
       IOP_STOP and IOP_START ("Sound Enable"). An earlier version of this
       emulator did not model IOP_STOP and IOP_START. It discarded each write
       to that address range, and gave no message. */
    psemu_t *ps = make_arm_cpu();
    int16_t samples[4];
    uint32_t n;

    psemu_bus_write32(&ps->bus, PSEMU_DAC_BASE + 0x0, 1u); /* DAC_CTRL enable */
    psemu_bus_write32(&ps->bus, PSEMU_DAC_BASE + 0x4, 0x100u << 6);

    /* IOP defaults to "started" (not stopped) - DAC_CTRL alone is enough. */
    dac_tick(&ps->dac, DAC_CYCLES_PER_SAMPLE);
    n = psemu_get_audio_samples(ps, samples, 4);
    assert(n == 1u);
    assert(samples[0] == (int16_t)(0x100 * 64));

    /* IOP_STOP bit5: stops sound even though DAC_CTRL is still enabled. */
    psemu_bus_write32(&ps->bus, PSEMU_IOP_BASE + 0x4, IOP_BIT_SOUND_STOPPED);
    dac_tick(&ps->dac, DAC_CYCLES_PER_SAMPLE);
    n = psemu_get_audio_samples(ps, samples, 4);
    assert(n == 1u);
    assert(samples[0] == 0);

    /* IOP_START bit5: resumes sound. */
    psemu_bus_write32(&ps->bus, PSEMU_IOP_BASE + 0x8, IOP_BIT_SOUND_STOPPED);
    dac_tick(&ps->dac, DAC_CYCLES_PER_SAMPLE);
    n = psemu_get_audio_samples(ps, samples, 4);
    assert(n == 1u);
    assert(samples[0] == (int16_t)(0x100 * 64));

    psemu_destroy(ps);
    printf("test_iop_sound_gate_mutes_dac OK\n");
}

static void test_iop_stop_start_take_effect_via_single_byte_writes(void) {
    /* A real, confirmed fault. An investigation of a report that one app
       played no sound found it. A direct trace of the BIOS and the app (see
       docs/hardware-notes.md) showed that real code writes IOP_STOP and
       IOP_START with single-byte stores. It does not always use a full 32-bit
       store. An earlier version of iop_write8 applied the effect of a STOP or
       START write only after the highest byte of a full 32-bit store arrived
       (shift == 24). A single-byte write to the low byte (shift == 0, which
       holds bit 5, "Sound Enable") never got to that gate, and this emulator
       discarded it with no message. Thus IOP_STOP and IOP_START had no effect
       at all when real code used single-byte stores. */
    psemu_t *ps = make_arm_cpu();
    int16_t samples[4];
    uint32_t n;

    psemu_bus_write32(&ps->bus, PSEMU_DAC_BASE + 0x0, 1u); /* DAC_CTRL enable */
    psemu_bus_write32(&ps->bus, PSEMU_DAC_BASE + 0x4, 0x100u << 6);

    /* Single-byte IOP_STOP write (offset 0 of the register, shift 0 -
       exactly the case the old commit-on-shift==24 gate missed). */
    psemu_bus_write8(&ps->bus, PSEMU_IOP_BASE + 0x4, IOP_BIT_SOUND_STOPPED);
    dac_tick(&ps->dac, DAC_CYCLES_PER_SAMPLE);
    n = psemu_get_audio_samples(ps, samples, 4);
    assert(n == 1u);
    assert(samples[0] == 0);

    /* Single-byte IOP_START write resumes sound the same way. */
    psemu_bus_write8(&ps->bus, PSEMU_IOP_BASE + 0x8, IOP_BIT_SOUND_STOPPED);
    dac_tick(&ps->dac, DAC_CYCLES_PER_SAMPLE);
    n = psemu_get_audio_samples(ps, samples, 4);
    assert(n == 1u);
    assert(samples[0] == (int16_t)(0x100 * 64));

    psemu_destroy(ps);
    printf("test_iop_stop_start_take_effect_via_single_byte_writes OK\n");
}

static void test_psemu_reset_restores_defaults_and_preserves_content(void) {
    /* psemu_reset must be a true hardware-level reset: each peripheral
       register goes back to its power-on default. But the loaded content, which
       is the flash data and the hardware ID, is not a register, and it must
       continue. RAM is a separate condition. See the comment on psemu_reset in
       psemu.c. Without this behavior, a mid-session load of a different BIOS or
       app left the peripheral state of the earlier session in position. That
       state caused the errors on the screen after a load. */
    psemu_t *ps = make_arm_cpu();
    int16_t samples[4];

    /* Move several peripheral registers away from their power-on defaults. */
    psemu_bus_write32(&ps->bus, PSEMU_INTC_BASE + 0x8, 0xFFu);   /* enable */
    psemu_bus_write32(&ps->bus, PSEMU_CLK_BASE + 0x0, 7u);       /* CLK_MODE */
    psemu_bus_write32(&ps->bus, PSEMU_DAC_BASE + 0x0, 1u);       /* DAC_CTRL enable */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 8, 1u);  /* F_BANK_FLG, select block 0 */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0, 2u);  /* commit */
    psemu_bus_write8(&ps->bus, 0x10, 0xAAu);                     /* RAM content */
    psemu_set_buttons(ps, PSEMU_BUTTON_UP);
    ps->cpu.unimplemented = 1; /* simulate a prior CPU fault */

    /* Content that must survive the reset. */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH2_BASE, 0x12345678u);
    psemu_set_hardware_id(ps, 0xDEADBEEFu);

    assert(ps->flash.bank_mask != 0);
    assert(ps->buttons != 0);
    assert(psemu_cpu_faulted(ps));

    psemu_reset(ps);

    /* Peripheral registers back to power-on defaults. */
    assert(psemu_bus_read32(&ps->bus, PSEMU_INTC_BASE + 0x8) == 0u);
    assert(psemu_bus_read32(&ps->bus, PSEMU_CLK_BASE + 0x0) == 0x10u); /* mode 0, steady bit always set */
    assert(psemu_bus_read32(&ps->bus, PSEMU_DAC_BASE + 0x0) == 0u);
    assert(ps->flash.bank_mask == 0u);
    assert(ps->flash.unlock_step == 0u);
    assert(ps->buttons == 0u);
    assert(!psemu_cpu_faulted(ps));

    /* RAM zeroed. */
    assert(psemu_bus_read8(&ps->bus, 0x10) == 0u);

    /* Loaded content and hardware ID preserved. */
    assert(psemu_bus_read32(&ps->bus, PSEMU_FLASH2_BASE) == 0x12345678u);
    assert(psemu_get_hardware_id(ps) == 0xDEADBEEFu);

    /* Audio outputs silence again, and the framebuffer is marked dirty so
       the frontend redraws the now-blank screen immediately. */
    dac_tick(&ps->dac, DAC_CYCLES_PER_SAMPLE);
    assert(psemu_get_audio_samples(ps, samples, 4) == 1u);
    assert(samples[0] == 0);
    assert(ps->lcd.dirty);

    psemu_destroy(ps);
    printf("test_psemu_reset_restores_defaults_and_preserves_content OK\n");
}

/* The settings that the BIOS owns. These settings are in RAM, and not in a
   register. See docs/hardware-notes.md, "System sound volume setting" and "Where
   the date/time settings actually live", for the method that found these
   addresses. */
static void test_volume_override_writes_bios_setting_byte(void) {
    psemu_t *ps = psemu_create();

    /* A fresh machine reads as Loud, because reset zeroes RAM and the BIOS
       never initializes this byte itself. */
    assert(psemu_get_volume(ps) == PSEMU_VOLUME_LOUD);

    psemu_set_volume(ps, PSEMU_VOLUME_SOFT);
    assert(ps->bus.ram[0x290] == 0x02);
    assert(psemu_get_volume(ps) == PSEMU_VOLUME_SOFT);

    psemu_set_volume(ps, PSEMU_VOLUME_MUTE);
    assert(ps->bus.ram[0x290] == 0x04);

    psemu_destroy(ps);
    printf("test_volume_override_writes_bios_setting_byte OK\n");
}

/* psemu_set_volume alone cannot hold the setting, because emulated code writes
   over the byte. The BIOS clears RAM early in its boot sequence, before it reads
   the volume during the sound initialization. Thus a frontend that writes only
   between frames is always too late, and the boot sound plays at full volume.
   psemu_set_volume_override prevents this: it makes the byte read-only to
   emulated code, and it writes the value again at each reset. */
static void test_volume_override_survives_emulated_writes_and_reset(void) {
    psemu_t *ps = psemu_create();

    /* Without the override, emulated code owns the byte. */
    psemu_set_volume(ps, PSEMU_VOLUME_MUTE);
    psemu_bus_write8(&ps->bus, 0x290, 0x00);
    assert(psemu_get_volume(ps) == PSEMU_VOLUME_LOUD);

    /* With it, the same write is ignored - at every access width, since the
       BIOS's RAM clear need not be a byte store. */
    psemu_set_volume_override(ps, PSEMU_VOLUME_MUTE);
    psemu_bus_write8(&ps->bus, 0x290, 0x00);
    assert(psemu_get_volume(ps) == PSEMU_VOLUME_MUTE);
    psemu_bus_write16(&ps->bus, 0x290, 0x0000);
    assert(psemu_get_volume(ps) == PSEMU_VOLUME_MUTE);
    psemu_bus_write32(&ps->bus, 0x290, 0x00000000u);
    assert(psemu_get_volume(ps) == PSEMU_VOLUME_MUTE);

    /* Only the locked byte is protected; the rest of the word it sits in is
       ordinary RAM. */
    assert(ps->bus.ram[0x291] == 0x00 && ps->bus.ram[0x292] == 0x00);
    psemu_bus_write8(&ps->bus, 0x291, 0xAB);
    assert(ps->bus.ram[0x291] == 0xAB);

    /* A reset stands in for a power cycle, which on real hardware is exactly
       what the battery-backed byte survives. */
    psemu_reset(ps);
    assert(psemu_get_volume(ps) == PSEMU_VOLUME_MUTE);
    assert(ps->bus.ram[0x291] == 0x00); /* unlocked RAM still wiped */

    /* Clearing hands the byte back to the BIOS's own sound menu. */
    psemu_clear_volume_override(ps);
    psemu_bus_write8(&ps->bus, 0x290, PSEMU_VOLUME_SOFT);
    assert(psemu_get_volume(ps) == PSEMU_VOLUME_SOFT);
    psemu_reset(ps);
    assert(psemu_get_volume(ps) == PSEMU_VOLUME_LOUD);

    psemu_destroy(ps);
    printf("test_volume_override_survives_emulated_writes_and_reset OK\n");
}

static void test_set_datetime_writes_rtc_and_both_century_bytes(void) {
    psemu_t *ps = psemu_create();

    assert(psemu_set_datetime(ps, 2026, 8, 1, 12, 45, 30, 7) == 1);

    /* RTC_DATE packs day, month, year from the LSB up; RTC_TIME packs
       seconds, minutes, hours, day-of-week. All BCD. */
    assert(ps->rtc.date == 0x00260801u);
    assert(ps->rtc.time == 0x07124530u);

    /* Both century bytes, not just one: the clock screen reads 0x426 and
       GetBcdDate reads 0x0CF, and they are genuinely independent. */
    assert(ps->bus.ram[0x426] == 0x20);
    assert(ps->bus.ram[0x0CF] == 0x20);

    /* A year in the 1900s, to be sure the century is computed rather than
       hardcoded to 0x20. */
    assert(psemu_set_datetime(ps, 1999, 1, 1, 0, 0, 0, 6) == 1);
    assert(ps->rtc.date == 0x00990101u);
    assert(ps->bus.ram[0x426] == 0x19);
    assert(ps->bus.ram[0x0CF] == 0x19);

    psemu_destroy(ps);
    printf("test_set_datetime_writes_rtc_and_both_century_bytes OK\n");
}

/* The BIOS sets the clock with a loop of RTC_ADJUST increments, until each field
   gets to a target value. A write to the registers during that loop can prevent
   the loop from reaching its target. Thus the setter function must make no change
   while PRGSEL is set, and the caller must try again later. */
static void test_set_datetime_refuses_while_rtc_in_program_mode(void) {
    psemu_t *ps = psemu_create();
    assert(psemu_set_datetime(ps, 2026, 8, 1, 12, 45, 30, 7) == 1);
    uint32_t date_before = ps->rtc.date;
    uint32_t time_before = ps->rtc.time;

    ps->rtc.mode |= 1u; /* PRGSEL */
    assert(psemu_set_datetime(ps, 1999, 1, 1, 0, 0, 0, 6) == 0);
    assert(ps->rtc.date == date_before);
    assert(ps->rtc.time == time_before);
    assert(ps->bus.ram[0x426] == 0x20);

    /* Once the BIOS leaves program mode, the same call goes through. */
    ps->rtc.mode &= ~1u;
    assert(psemu_set_datetime(ps, 1999, 1, 1, 0, 0, 0, 6) == 1);
    assert(ps->rtc.date == 0x00990101u);

    psemu_destroy(ps);
    printf("test_set_datetime_refuses_while_rtc_in_program_mode OK\n");
}

static void test_set_datetime_rejects_out_of_range_arguments(void) {
    psemu_t *ps = psemu_create();
    assert(psemu_set_datetime(ps, 2026, 8, 1, 12, 0, 0, 7) == 1);
    uint32_t date_before = ps->rtc.date;

    assert(psemu_set_datetime(ps, 2026, 13, 1, 0, 0, 0, 1) == 0);  /* month */
    assert(psemu_set_datetime(ps, 2026, 0, 1, 0, 0, 0, 1) == 0);   /* month */
    assert(psemu_set_datetime(ps, 2026, 8, 32, 0, 0, 0, 1) == 0);  /* day */
    assert(psemu_set_datetime(ps, 2026, 8, 1, 24, 0, 0, 1) == 0);  /* hour */
    assert(psemu_set_datetime(ps, 2026, 8, 1, 0, 60, 0, 1) == 0);  /* minute */
    assert(psemu_set_datetime(ps, 2026, 8, 1, 0, 0, 60, 1) == 0);  /* second */
    assert(psemu_set_datetime(ps, 2026, 8, 1, 0, 0, 0, 0) == 0);   /* day-of-week */
    assert(psemu_set_datetime(ps, 2026, 8, 1, 0, 0, 0, 8) == 0);   /* day-of-week */

    /* A rejected call must not have written anything partway through. */
    assert(ps->rtc.date == date_before);

    psemu_destroy(ps);
    printf("test_set_datetime_rejects_out_of_range_arguments OK\n");
}

/* The override addresses have a meaning only on the BIOS revisions that this
   project traced. Each other revision must fail safe. Thus a frontend can disable
   its controls, and it does not corrupt other kernel RAM. */
static void test_settings_offsets_unknown_without_a_known_bios(void) {
    psemu_t *ps = psemu_create();
    assert(psemu_settings_offsets_known(ps) == 0); /* no BIOS loaded at all */

    static uint8_t fake_bios[PSEMU_BIOS_SIZE];
    memset(fake_bios, 0xA5, sizeof(fake_bios));
    assert(psemu_load_bios(ps, fake_bios, sizeof(fake_bios)) == PSEMU_OK);
    assert(psemu_settings_offsets_known(ps) == 0);

    psemu_destroy(ps);
    printf("test_settings_offsets_unknown_without_a_known_bios OK\n");
}

/* The settings overrides above, which occur at each frame, are safe only while
   the BIOS shell owns its RAM. psemu_app_running tells a frontend when the BIOS
   shell no longer owns that RAM. See the comment on that function in
   psemu/psemu.h for the fault that made it necessary. */
static void test_app_running_follows_flash1_execution(void) {
    psemu_t *ps = psemu_create();
    ps->has_bios = 1; /* psemu_run is a no-op without a loaded BIOS */

    /* Nothing has run yet, so the BIOS shell is presumed to own the machine. */
    assert(psemu_app_running(ps) == 0);

    /* Put an app in FLASH1 that only loops: a "B ." instruction at the base of
       the window, and the same instruction one bank later. Thus this test does
       not depend on the bank resolution. */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH2_BASE + 0, 0xEAFFFFFEu);
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 8, 1u); /* F_BANK_FLG: enable block 0 */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0, 2u); /* commit */
    assert(psemu_bus_read32(&ps->bus, PSEMU_FLASH1_BASE) == 0xEAFFFFFEu);

    /* Executing from the FLASH1 window is what "an app is running" means. */
    arm7tdmi_reset(&ps->cpu, PSEMU_FLASH1_BASE);
    psemu_run(ps, 1000u);
    assert(psemu_app_running(ps) == 1);

    /* An app is inside the BIOS for the duration of each SWI that it issues. A
       short period there must not read as "the app exited". If it does, the
       frontend writes its overrides into the RAM of the app during that SWI. */
    put32(ps, 0, 0xEAFFFFFEu); /* B . in RAM, standing in for a BIOS routine */
    arm7tdmi_reset(&ps->cpu, 0);
    psemu_run(ps, 1000u);
    assert(psemu_app_running(ps) == 1);

    /* A sufficiently long period outside FLASH1 is a true return of control to
       the BIOS shell. The grace period is a real-time duration at the reference
       rate. Thus a budget in the same unit is longer than the grace period at
       each CLK_MODE value. That property makes this test possible. */
    psemu_run(ps, 4u * (uint32_t)PSEMU_ASSUMED_CPU_HZ);
    assert(psemu_app_running(ps) == 0);

    /* A reset drops it immediately - no grace period, since a reset really is
       an instant return to the BIOS. */
    arm7tdmi_reset(&ps->cpu, PSEMU_FLASH1_BASE);
    psemu_run(ps, 1000u);
    assert(psemu_app_running(ps) == 1);
    psemu_reset(ps);
    assert(psemu_app_running(ps) == 0);

    psemu_destroy(ps);
    printf("test_app_running_follows_flash1_execution OK\n");
}

static void test_fiq_banks_r8_to_r12(void) {
    /* A real ARM7TDMI banks r8-r12 for FIQ only. That bank lets a FIQ handler use those registers as
       scratch registers with no save operation, and it is the reason that a "fast interrupt" is fast.
       This emulator banked only r13 and r14. Thus a FIQ handler destroyed r8-r12 of the interrupted
       code, and gave no error. */
    psemu_t *ps = psemu_create();
    int i;

    arm_set_mode(&ps->cpu, ARM_MODE_USR);
    for (i = 8; i <= 12; i++) {
        ps->cpu.r[i] = 0xAA000000u + (uint32_t)i;
    }
    ps->cpu.r[7] = 0x77777777u;  /* r0-r7 are never banked */
    ps->cpu.r[13] = 0x00000800u;

    /* Entering FIQ must present a separate, independent set of r8-r12. */
    arm_set_mode(&ps->cpu, ARM_MODE_FIQ);
    for (i = 8; i <= 12; i++) {
        assert(ps->cpu.r[i] != 0xAA000000u + (uint32_t)i);
        ps->cpu.r[i] = 0xFF000000u + (uint32_t)i; /* handler scribbles on them */
    }
    assert(ps->cpu.r[7] == 0x77777777u); /* still shared */

    /* Returning restores every one of them. */
    arm_set_mode(&ps->cpu, ARM_MODE_USR);
    for (i = 8; i <= 12; i++) {
        assert(ps->cpu.r[i] == 0xAA000000u + (uint32_t)i);
    }
    assert(ps->cpu.r[13] == 0x00000800u);

    /* And the FIQ bank kept its own values for next time. */
    arm_set_mode(&ps->cpu, ARM_MODE_FIQ);
    for (i = 8; i <= 12; i++) {
        assert(ps->cpu.r[i] == 0xFF000000u + (uint32_t)i);
    }

    /* A switch that does not cross the FIQ boundary must leave r8-r12 completely alone: every non-FIQ mode
       shares one copy. */
    arm_set_mode(&ps->cpu, ARM_MODE_SVC);
    for (i = 8; i <= 12; i++) {
        assert(ps->cpu.r[i] == 0xAA000000u + (uint32_t)i);
        ps->cpu.r[i] = 0xBB000000u + (uint32_t)i;
    }
    arm_set_mode(&ps->cpu, ARM_MODE_IRQ);
    for (i = 8; i <= 12; i++) {
        assert(ps->cpu.r[i] == 0xBB000000u + (uint32_t)i);
    }

    psemu_destroy(ps);
    printf("test_fiq_banks_r8_to_r12 OK\n");
}

static void test_ldm_stm_user_bank_transfer(void) {
    /* "LDM ...^" or "STM ...^" with the PC absent from the list moves the registers of USER mode. It
       does not move the registers of the current mode. The real BIOS uses this form to save and
       restore r13 and r14 of an app, with no mode change: STMIA r0!,{r13,r14}^ at 0x04001944, and the
       LDM at 0x04001B90. exec_block_transfer ignored the S bit in this condition before, and it
       transferred the registers of the current mode. */
    psemu_t *ps = psemu_create();

    /* Give User mode a known r13/r14, then leave it. */
    arm_set_mode(&ps->cpu, ARM_MODE_USR);
    ps->cpu.r[13] = 0x00000800u;
    ps->cpu.r[14] = 0x02001234u;
    arm_set_mode(&ps->cpu, ARM_MODE_SVC);
    ps->cpu.r[13] = 0x00000180u; /* SVC's own, deliberately different */
    ps->cpu.r[14] = 0x04001234u;
    ps->cpu.r[0] = 0x00000400u;  /* somewhere in RAM to spill to */

    /* STMIA r0!, {r13,r14}^ - must write USER's pair, not SVC's. */
    put32(ps, 0, 0xE8E06000u);
    ps->cpu.r[15] = 0;
    arm7tdmi_step(&ps->cpu);
    assert(psemu_bus_read32(&ps->bus, 0x400u) == 0x00000800u);
    assert(psemu_bus_read32(&ps->bus, 0x404u) == 0x02001234u);
    assert(ps->cpu.r[0] == 0x00000408u); /* writeback still happens */
    assert(ps->cpu.r[13] == 0x00000180u); /* SVC's own pair untouched */
    assert(ps->cpu.r[14] == 0x04001234u);

    /* Now rewrite the spilled values and load them back the same way. */
    psemu_bus_write32(&ps->bus, 0x400u, 0x000007C0u);
    psemu_bus_write32(&ps->bus, 0x404u, 0x0200ABCDu);
    ps->cpu.r[0] = 0x00000400u;
    put32(ps, 0, 0xE8F06000u); /* LDMIA r0!, {r13,r14}^ */
    ps->cpu.r[15] = 0;
    arm7tdmi_step(&ps->cpu);
    assert(ps->cpu.r[13] == 0x00000180u); /* still SVC's own */
    assert(ps->cpu.r[14] == 0x04001234u);
    assert(ps->cpu.r13_bank[ARM_BANK_USR] == 0x000007C0u); /* USER's pair took the load */
    assert(ps->cpu.r14_bank[ARM_BANK_USR] == 0x0200ABCDu);

    /* Confirm it end-to-end: switching to User mode presents the loaded values. */
    arm_set_mode(&ps->cpu, ARM_MODE_USR);
    assert(ps->cpu.r[13] == 0x000007C0u);
    assert(ps->cpu.r[14] == 0x0200ABCDu);

    psemu_destroy(ps);
    printf("test_ldm_stm_user_bank_transfer OK\n");
}

static void test_rtc_date_rolls_over_at_midnight(void) {
    /* Confirmed on real hardware: the date advances at midnight. This emulator did not do this before.
       The cascade for each second stopped at the day of the week. Thus a device stayed at one date
       permanently. */
    struct {
        uint32_t date_before, time_before, date_after;
        const char *what;
    } cases[] = {
        {0x00260801u, 0x07235959u, 0x00260802u, "ordinary day"},
        {0x00260831u, 0x07235959u, 0x00260901u, "end of a 31-day month"},
        {0x00260930u, 0x07235959u, 0x00261001u, "end of a 30-day month"},
        {0x00261231u, 0x07235959u, 0x00270101u, "end of the year"},
        {0x00240228u, 0x07235959u, 0x00240229u, "February in a leap year (24 % 4 == 0)"},
        {0x00240229u, 0x07235959u, 0x00240301u, "the leap day itself"},
        {0x00260228u, 0x07235959u, 0x00260301u, "February in a common year"},
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        psemu_t *ps = psemu_create();
        ps->has_bios = 1;
        put32(ps, 0, 0xEAFFFFFEu);
        arm7tdmi_reset(&ps->cpu, 0);
        ps->rtc.date = cases[i].date_before;
        ps->rtc.time = cases[i].time_before; /* 23:59:59, one second short of midnight */
        psemu_run(ps, (uint32_t)PSEMU_ASSUMED_CPU_HZ);
        assert((ps->rtc.time & 0x00FFFFFFu) == 0x000000u); /* 00:00:00 */
        assert(ps->rtc.date == cases[i].date_after);
        (void)cases[i].what;
        psemu_destroy(ps);
    }

    /* A day that does not end must leave the date completely alone. */
    {
        psemu_t *ps = psemu_create();
        ps->has_bios = 1;
        put32(ps, 0, 0xEAFFFFFEu);
        arm7tdmi_reset(&ps->cpu, 0);
        ps->rtc.date = 0x00260815u;
        ps->rtc.time = 0x07105959u; /* 10:59:59 */
        psemu_run(ps, (uint32_t)PSEMU_ASSUMED_CPU_HZ);
        assert(ps->rtc.date == 0x00260815u);
        psemu_destroy(ps);
    }
    printf("test_rtc_date_rolls_over_at_midnight OK\n");
}

static void test_rtc_keeps_real_time(void) {
    /* The RTC operates a wall clock. Thus one emulated second must last one real second. The budget of
       psemu_run is in PSEMU_ASSUMED_CPU_HZ reference cycles, and that unit IS real elapsed time. Thus
       a budget of exactly that number of cycles must advance the clock by exactly one second. This is
       true at each CLK_MODE value, because the RTC uses its own oscillator. This test found
       RTC_TICK_CYCLES_RUN at the value 4000000, which made the device clock almost 4 times too
       slow. */
    static const uint32_t modes[] = {0u, 4u, 7u};
    size_t m;
    for (m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        psemu_t *ps = psemu_create();
        int i;
        ps->has_bios = 1;
        put32(ps, 0, 0xEAFFFFFEu); /* B . so there is something to execute */
        arm7tdmi_reset(&ps->cpu, 0);
        psemu_bus_write32(&ps->bus, PSEMU_CLK_BASE, modes[m]);

        assert((ps->rtc.time & 0xFFu) == 0x00u); /* power-on reset is 00:00:00 */
        for (i = 0; i < 30; i++) {
            psemu_run(ps, (uint32_t)PSEMU_ASSUMED_CPU_HZ);
        }
        /* 30 seconds of real time, so BCD seconds must read exactly 0x30. */
        assert((ps->rtc.time & 0xFFu) == 0x30u);
        psemu_destroy(ps);
    }
    printf("test_rtc_keeps_real_time OK\n");
}

static void test_clk_stop_halts_until_a_button_wakes_it(void) {
    psemu_t *ps = psemu_create();
    uint64_t steps;
    ps->has_bios = 1; /* psemu_run is a no-op without a loaded BIOS */

    /* B . in RAM, so the CPU has something to execute that never leaves. */
    put32(ps, 0, 0xEAFFFFFEu);
    arm7tdmi_reset(&ps->cpu, 0);

    /* Arm Timer0 and enable its interrupt. A running timer is the whole point: it is clocked by the same
       oscillator the stop bit halts, so it must NOT tick while stopped and must NOT wake the CPU. Waking on
       any asserted interrupt would leave a running timer re-asserting within microseconds, and the CPU
       would never actually pause. */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x0, 4u);                   /* period */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x4, 4u);                   /* count */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x8, TIMER_CTRL_ENABLE);    /* run, /2 */
    psemu_bus_write32(&ps->bus, PSEMU_INTC_BASE + 0x8, INT_TIMER0 | INT_BTN_ACTION);

    psemu_run(ps, 10000u);
    assert(ps->cpu.total_steps > 0);

    /* Request the stop. */
    psemu_bus_write32(&ps->bus, PSEMU_CLK_BASE + 0x4, 1u);
    assert(clk_stop_requested(&ps->clk));

    steps = ps->cpu.total_steps;
    psemu_run(ps, 10u * (uint32_t)PSEMU_ASSUMED_CPU_HZ);
    assert(ps->cpu.total_steps == steps); /* nothing executed for ten seconds of budget */
    assert(clk_stop_requested(&ps->clk));  /* and the timer did not wake it */

    /* The RTC has its own oscillator, so it keeps running while stopped - that is what lets a sleeping
       device still know the time. Ten seconds of stopped CPU are still ten seconds on the clock, in BCD.

       This used to assert INT_RTC in STATUS instead, which is not the same claim. STATUS carries the raw
       level of the RTC's interrupt line (see intc.h), and that line is a square wave: asserting it is high
       only ever tests which half of the waveform the run happened to end in. Ten seconds is a whole number
       of periods at both the 1-transition-per-second rate this was written against and the 2Hz rate
       measured later, so it landed low and the assert failed both times. It never passed in any Debug
       build; NDEBUG hid that everywhere else. */
    assert((ps->rtc.time & 0xFFu) == 0x10u);

    /* The interrupt line keeps moving too, which is the part the level check was reaching for. One more
       transition is the phase-independent way to ask, and STATUS must follow it. */
    {
        int line_before = ps->rtc.int_line;
        psemu_run(ps, RTC_TICK_CYCLES_RUN);
        assert(ps->rtc.int_line != line_before);
        assert(((ps->intc.status & INT_RTC) != 0) == (ps->rtc.int_line != 0));
        assert(ps->cpu.total_steps == steps); /* still stopped: an RTC tick is not a wake */
    }

    /* A button is external to this clock, so it wakes the CPU, and the wake clears the bit. */
    psemu_set_buttons(ps, PSEMU_BUTTON_FIRE);
    psemu_run(ps, 10000u);
    assert(!clk_stop_requested(&ps->clk));
    assert(ps->cpu.total_steps > steps);

    psemu_destroy(ps);
    printf("test_clk_stop_halts_until_a_button_wakes_it OK\n");
}

int main(void) {
    test_arm_data_processing();
    test_arm_long_multiply_and_swap();
    test_arm_memory();
    test_arm_ldrh_misaligned_quirks();
    test_arm_control_flow();
    test_arm_exceptions_and_psr();
    test_msr_user_mode_control_byte_is_ignored();
    test_arm_exception_return();
    test_arm_ldm_exception_return();
    test_thumb_basic();
    test_thumb_memory_and_control();
    test_thumb_bl_bx_lr_stays_thumb();
    test_cpu_faulted_flag();
    test_faulted_cpu_stops_advancing();
    test_crash_report_contents();
    test_intc_status_sources_also_latch_hold();
    test_button_hold_pulses_not_sustained();
    test_button_status_survives_acknowledge();
    test_timer_and_irq();
    test_fiq_delivery_and_priority();
    test_fiq_takes_priority_over_irq();
    test_timer_clock_divisor();
    test_timer_registers_are_16_bit();
    test_boot_ready_stub();
    test_clk_mode_scales_run_speed();
    test_timer_scales_with_clk_mode();
    test_clk_mode_keeps_rtc_dac_on_real_time();
    test_rtc_defaults_and_increment();
    test_flash_bank_select();
    test_flash_bank_val_remapping();
    test_flash_ctrl_busy_wait_bits();
    test_flash_serial_number_default_and_override();
    test_hardware_id_string_conversion();
    test_flash_serial_number_register_access();
    test_content_identity_hash_survives_a_save();
    test_flash_load_app_synthesizes_directory();
    test_flash_load_app_rejects_oversized_app();
    test_psemu_load_mcs_validates_and_unwraps();
    test_psemu_load_content_dispatches_by_size();
    test_flash_key_addresses_are_not_data_storage();
    test_flash_header_write_via_unlock_sequence();
    test_flash_frame_write_lands_in_a_ps1_save_block();
    test_flash_header_write_requires_unlock_first();
    test_flash_header_write_disarms_after_unrelated_write();
    test_flash_header_write_requires_correct_key_order();
    test_lcd_mode_dison_and_rotate();
    test_dac_basic();
    test_iop_sound_gate_mutes_dac();
    test_iop_stop_start_take_effect_via_single_byte_writes();
    test_psemu_reset_restores_defaults_and_preserves_content();
    test_volume_override_writes_bios_setting_byte();
    test_volume_override_survives_emulated_writes_and_reset();
    test_set_datetime_writes_rtc_and_both_century_bytes();
    test_set_datetime_refuses_while_rtc_in_program_mode();
    test_set_datetime_rejects_out_of_range_arguments();
    test_settings_offsets_unknown_without_a_known_bios();
    test_app_running_follows_flash1_execution();
    test_fiq_banks_r8_to_r12();
    test_ldm_stm_user_bank_transfer();
    test_rtc_keeps_real_time();
    test_rtc_date_rolls_over_at_midnight();
    test_clk_stop_halts_until_a_button_wakes_it();
    printf("all cpu tests passed\n");
    return 0;
}
