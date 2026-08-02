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

static void test_arm_ldrh_misaligned_quirks(void) {
    /* A real, confirmed ARM7TDMI hardware quirk (see exec_halfword_transfer
       in arm_exec.c and docs/hardware-notes.md, "CPU"):
       a misaligned halfword load doesn't just silently round down to the
       aligned halfword below it - real silicon rotates the loaded LDRH
       value right by 8 bits (swapping its two bytes), while a misaligned
       LDRSH instead behaves as a sign-extended BYTE load (LDRSB) from that
       same odd address. Found via a real PocketStation ID-editing
       homebrew's font-glyph routine, which reads a byte-packed table
       via `LDRH Rd,[Rn],#1` (post-increment by 1, not 2) masked to the low
       byte - a real, working idiom on real hardware that depends entirely
       on this rotation to recover each individual byte; without it, every
       other byte read came back wrong, corrupting every glyph drawn. */
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

    /* LDRH R2, [R0, #1] - misaligned (address 0x101): rotates the
       aligned-down halfword (0x2211) right by 8 -> 0x1122, putting the
       byte actually at the odd address (0x22) in the low 8 bits, matching
       what the real font routine's byte-wise reader expects. */
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

    /* LDRSH R4, [R0, #3] - misaligned (address 0x103): behaves as a
       sign-extended BYTE load from 0x103 (0x80, sign bit set), NOT a
       rotated halfword read - real hardware's documented divergence from
       LDRH's rotation behavior above. */
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

    /* MRS/MSR round trip on the control byte (mode + I/F/T bits) - done
       here, still in SVC mode (privileged) from the SWI above, since a
       write to CPSR's control byte only succeeds from a privileged mode
       on real hardware (see test_msr_user_mode_control_byte_is_ignored
       for the User-mode-restricted case). */
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
    /* Real ARM7TDMI/ARMv4T silicon: MSR to CPSR from User mode can only
       change the condition flags (top byte) - the control byte (mode, T,
       F, I) is protected and silently ignored, regardless of which of
       those bits the instruction asks for. Found via a real homebrew app
       (pk_timing_bench) that unintentionally relied on the emulator
       previously getting this wrong: it wrote CPSR_I/F from User mode
       expecting a real-hardware no-op, but the write actually took
       effect here, leaving interrupts globally masked for the app's
       entire runtime in a way real hardware never would - see
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

static void test_cpu_faulted_flag(void) {
    /* psemu_cpu_faulted() (added while investigating a real, reproducible
       Chocobo World crash - see docs/hardware-notes.md - where the
       desktop frontend had no way to tell the CPU had run into an
       unrecognized opcode and silently kept stepping it, corrupting
       state forever with zero diagnostic). 0xB800 is a Thumb "format
       12/13/14" pattern (top3=101) that matches none of exec_load_address
       /exec_add_sp_offset/exec_push_pop's checks, falling to the
       unimplemented default. */
    psemu_t *ps = make_thumb_cpu();
    assert(!psemu_cpu_faulted(ps));
    put16(ps, 0, 0xB800u);
    arm7tdmi_step(&ps->cpu);
    assert(psemu_cpu_faulted(ps));
    psemu_destroy(ps);
    printf("test_cpu_faulted_flag OK\n");
}

static void test_faulted_cpu_stops_advancing(void) {
    /* A real, confirmed bug found via a real desktop-app crash report:
       arm7tdmi_step only gated on `halted` (which nothing ever sets),
       not `unimplemented` - so a caller that doesn't check the flag
       after every single instruction (psemu_run's whole-cycle-budget
       loop, unlike tools/inspect.c's own per-instruction loop) kept
       fetching and executing from whatever garbage the CPU landed on,
       potentially thousands of instructions past the real fault site,
       corrupting registers/PC before anyone ever noticed. Confirms both
       that r15 stops moving and that the trace ring buffer stops
       recording once faulted, so a crash report reflects the original
       fault, not wherever unchecked continued execution wandered to. */
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
    /* psemu_write_crash_report() - added so the desktop frontend can
       write a real diagnostic file instead of just a one-line stderr
       message when something goes wrong (see docs/hardware-notes.md,
       the Chocobo World crash investigation this was built for). Checks
       the report names the actual fault opcode/address and includes the
       executed-PC trace, not just that it doesn't crash. */
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

    /* Buttons and the RTC tick (INT_STATUS_MASK bits) must latch into
       BOTH status and hold, not status only. An earlier version of
       intc_set_line only set `status` for these bits - confirmed wrong by
       disassembling the real BIOS: its top-level IRQ handler tests
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
       the INT_INPUT-vs-INT_LATCH naming and the real RTC handler's
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

static void test_button_hold_pulses_not_sustained(void) {
    psemu_t *ps = make_arm_cpu();

    /* A real, confirmed bug found via direct real-hardware testing: on
       the real device, holding Action does nothing (whatever's blinking
       keeps blinking normally) and only releasing it confirms - but this
       emulator's button HOLD bit used to stay latched as a sustained
       level for the entire physical hold duration. A real BIOS callback
       branches on `hold & INT_BTN_ACTION` *before* its RTC-driven redraw
       check, so a continuously-set hold bit permanently skipped that
       redraw path for as long as the button was held, confirmed via a
       runtime watchpoint showing the RTC-driven blink counter frozen
       throughout a sustained hold. Fixed: HOLD only pulses on the press
       edge; a still-held button clears HOLD again (but not STATUS,
       which keeps tracking the live level) on the next
       psemu_set_buttons call with no new edge. */
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

    /* docs/hardware-notes.md, "Buttons": `status` tracks the live button level,
       for code that polls it directly. An acknowledge clears a latched interrupt
       REQUEST, so it must clear HOLD. It must not clear that live level.

       This used to fail. Buttons were missing from INT_LEVEL_MASK, so any
       acknowledge wiped their STATUS bit and a button that was still physically
       held read back as released until something pressed it again. The real BIOS
       acknowledges button interrupts during boot, so an app that polled STATUS
       for a held button saw nothing.

       A real app depends on this: pk_timing_bench opens its exit prompt by
       counting 75000 consecutive polls of STATUS with Action held, and waits on
       the same level to see the button released again before it departs. */
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

    /* Timer0: period=count=10, enabled; also enable its source in the INTC -
       hold alone isn't enough to assert IRQ, matching real hardware.
       Control bits 0-1 are left at 0 -> the slowest selectable divisor is
       actually /2 (0 and 3 both mean /2) - confirmed real hardware
       behavior, not a raw 1:1 cycle-to-count ratio, so the cycle counts below are
       doubled relative to `period` to account for it. */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x0, 10u); /* period */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x4, 10u); /* count */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x8, TIMER_CTRL_ENABLE);
    ps->intc.enable |= INT_TIMER0;
    assert(psemu_bus_read32(&ps->bus, PSEMU_TIMER_BASE + 0x0) == 10u);

    timer_tick(&ps->timer, &ps->intc, 10u);
    assert(!intc_irq_asserted(&ps->intc)); /* fewer than `period` /2-divided ticks: no expiry yet */
    timer_tick(&ps->timer, &ps->intc, 10u);
    /* 10 more /2-divided ticks bring count to exactly 0 - but zero is a state the real counter actually
       occupies, so it reloads and fires on the NEXT tick, giving a real period of P+1 rather than P.
       Confirmed by direct real-hardware measurement; see timer.c's timer_tick and
       pk_timing_bench/VERIFICATION.md. This assertion used to expect the IRQ one tick earlier. */
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
    /* A real, confirmed bug this session (see docs/hardware-notes.md,
       "Interrupt controller"): FIQ was never actually delivered
       by this emulator, for any app, ever - intc_fiq_asserted (intc.c)
       already existed and was already verified correct against real
       hardware's bit-mapping, but arm7tdmi_step only ever checked
       intc_irq_asserted. Found by tracing why a real homebrew's
       Timer2-driven (FIQ, not IRQ) confirmation beep produced silence:
       Timer2's period/enable registers were being written and INT_TIMER2
       latched into the interrupt controller's hold register,
       but the CPU's mode never left USR - the interrupt sat asserted
       forever, unconsumed. */
    psemu_t *ps = make_arm_cpu();

    /* Timer2 (not Timer0/1 - real hardware's FIQ source, per
       docs/hardware-notes.md's confirmed INT_FIQ_MASK bit mapping),
       same setup pattern as test_timer_and_irq's Timer0/IRQ case. */
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
    /* Real ARM7TDMI sets BOTH F and I on FIQ entry (unlike IRQ, which only
       sets I) - prevents a second FIQ from recursively interrupting the
       handler before it saves state. */
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
    /* Real ARM7TDMI checks FIQ before IRQ - FIQ has strictly higher
       priority in the exception scheme. With both pending and
       unmasked simultaneously, the CPU must enter FIQ, not IRQ. */
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

    /* Control bits 0-1 = 1 selects /32 - an earlier
       version of timer_tick ignored this entirely and decremented count
       by raw cycles directly, which would fire this timer 16x too often
       relative to the /2 case. period=count=1, and the real period is P+1
       selected ticks (see timer.c), so an expiry needs two /32 ticks - 64
       raw cycles. The divisor is what this test is actually pinning down:
       were it ignored, the timer would expire within the first couple of
       raw cycles instead. */
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

    /* period and count are 16-bit on real hardware; a wider store keeps only the low half
       (see TIMER_REG_MASK in timer.h, and docs/hardware-notes.md, "Timers").
       This emulator used to model both as full uint32_t. Pop'n Music's FIQ audio timer
       programs Timer2 with a value whose upper half is nonzero; retaining it stretched the
       period from 851 ticks to about 52.6 million, so the audio interrupt never fired and
       the game ran silently. */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x20, 0x03240353u); /* Timer2 period */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x24, 0x0323B1FDu); /* Timer2 count */
    assert(ps->timer.timers[2].period == 0x0353u);
    assert(ps->timer.timers[2].count == 0xB1FDu);
    assert(psemu_bus_read32(&ps->bus, PSEMU_TIMER_BASE + 0x20) == 0x0353u);

    /* Enabling reloads count from the masked period, so a timer armed this way expires on
       the real 851-tick schedule rather than never. */
    psemu_bus_write32(&ps->bus, PSEMU_TIMER_BASE + 0x28, TIMER_CTRL_ENABLE);
    assert(ps->timer.timers[2].count == 0x0353u);

    /* The reload on expiry is masked too, so a state carried over from the old 32-bit model
       converges instead of staying stuck at a huge count. */
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

    /* Real BIOS polls this address (LDR/TST #0x10/BEQ) before flash-control
       init; see docs/hardware-notes.md. Must read back with bit 4 set or
       a real boot sequence hangs forever. */
    assert(psemu_bus_read32(&ps->bus, PSEMU_CLK_BASE) & 0x10u);

    /* Bit 9 of INT_INPUT is the RTC's toggling interrupt line, not a static
       flag - see test_rtc_defaults_and_increment for its actual behavior. */

    psemu_destroy(ps);
    printf("test_boot_ready_stub OK\n");
}

static void test_clk_mode_scales_run_speed(void) {
    /* Real hardware runs more raw instructions per real frame when
       CLK_MODE is elevated. Confirmed by tracing a real boot+beep
       sequence: the BIOS sets mode 7 (~4MHz, per the CLK_MODE/
       SetCpuSpeed table in clk.c) for the whole HELLO/heart/beep
       window, then drops to a slower mode. psemu_run's cycle budget
       uses the PSEMU_ASSUMED_CPU_HZ reference rate. Raising CLK_MODE
       to max runs more raw cycles in the same budget than the idle
       default (mode 0). Timer scales with CLK_MODE too (see
       test_timer_scales_with_clk_mode). RTC and DAC stay pinned to
       real time (see test_clk_mode_keeps_rtc_dac_on_real_time). */
    psemu_t *ps_idle = make_arm_cpu();
    psemu_t *ps_max = make_arm_cpu();
    ps_idle->has_bios = 1; /* psemu_run is a no-op without a loaded BIOS */
    ps_max->has_bios = 1;

    psemu_bus_write32(&ps_max->bus, PSEMU_CLK_BASE, 7u);

    uint32_t ran_idle = psemu_run(ps_idle, 100000u);
    uint32_t ran_max = psemu_run(ps_max, 100000u);

    /* Mode 7 (~4MHz) is ~122x mode 0's ~32.768kHz - assert a conservative
       lower bound (10x) to avoid coupling this test to the exact table
       values while still catching a completely unscaled psemu_run. */
    assert(ran_max > ran_idle * 10u);

    psemu_destroy(ps_idle);
    psemu_destroy(ps_max);
    printf("test_clk_mode_scales_run_speed OK\n");
}

static void test_timer_scales_with_clk_mode(void) {
    /* This function's history (see docs/hardware-notes.md, "CLK_MODE"
       for the current, settled behavior): Timer was
       twice made to stay pinned to real time like RTC/DAC, reasoning
       that it drives the app's Timer1-IRQ audio-generation loop
       and shouldn't race ahead of real time. That fixed a real "beep plays far too fast"
       complaint, but direct measurement later showed it broke something
       else: with Timer decoupled, the HELLO animation (driven by the
       SAME Timer1 heartbeat - both audio and general GUI ticking are
       confirmed real uses of the same IRQ) ran ~4x too slow during
       CLK_MODE=7, and a date-setting screen's blink ran ~2x too fast
       during CLK_MODE=4 - both errors matching the ratio between those
       CLK_MODEs' real Hz and the fixed reference rate almost exactly.
       Real timers are clocked by the
       System Clock, which varies with CLK_MODE. Timer now
       tracks CLK_MODE like the outer loop's own throughput - the
       earlier "beep too fast" complaint is inferred to be the
       separate DAC output-pacing bug fixed by
       test_clk_mode_keeps_rtc_dac_on_real_time below, not a result
       of Timer following CLK_MODE. */
    psemu_t *ps_idle = make_arm_cpu();
    psemu_t *ps_max = make_arm_cpu();
    ps_idle->has_bios = 1;
    ps_max->has_bios = 1;

    psemu_bus_write32(&ps_max->bus, PSEMU_CLK_BASE, 7u);

    /* period/count large enough that it never expires (and hence never
       reloads/wraps) within this test's budget, at either CLK_MODE - so
       (initial count - final count), in ticks, cumulatively reflects
       total raw cycles fed, unlike cycle_accumulator (which is only the
       sub-divisor remainder, not cumulative).
       That means the largest period the hardware can actually hold: these
       registers are 16-bit (see TIMER_REG_MASK in timer.h). This test used
       to load 100000000, which no real timer can store; once the emulator
       started masking to the real width that value wrapped almost
       immediately and the tick arithmetic below became meaningless.
       The budget is halved to match: at 0xFFFF and the /2 divisor, mode 7
       accumulates ~50k of the 65535 available ticks over ~0.05s, so it
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
    /* Unlike Timer (see test_timer_scales_with_clk_mode), RTC is a
       separate, CPU-clock-independent real 1Hz oscillator (confirmed
       via real hardware: the RTC ticks flat regardless of CPU_FREQ), and this
       emulator's DAC resampling needs the same real-time independence
       for its fixed PSEMU_DAC_SAMPLE_RATE_HZ output rate - regardless of
       CLK_MODE, RTC/DAC progress must come out the same for the same
       real-time budget. */
    psemu_t *ps_idle = make_arm_cpu();
    psemu_t *ps_max = make_arm_cpu();
    ps_idle->has_bios = 1;
    ps_max->has_bios = 1;

    psemu_bus_write32(&ps_max->bus, PSEMU_CLK_BASE, 7u);

    uint32_t budget = PSEMU_ASSUMED_CPU_HZ / 10u; /* ~0.1 real second */
    psemu_run(ps_idle, budget);
    psemu_run(ps_max, budget);

    long rtc_diff = (long)ps_idle->rtc.tick_accumulator - (long)ps_max->rtc.tick_accumulator;
    /* Small final-step overshoot only - but the bound is wider than it
       used to be now that arm7tdmi_step returns a real, region-dependent
       cycle cost (see docs/hardware-notes.md, "Memory access timing")
       instead of a flat 1: this test's own zero-filled instruction stream
       runs off the end of the 2KB WRAM region partway through the
       budget, after which every step costs 2 raw cycles instead of 1, up
       to doubling the worst-case single-step overshoot this assert has
       to tolerate (observed ~38 vs the old flat-cost model's headroom
       under 20). */
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

    /* Real silicon power-on-reset values:
       date 1998-01-01, time 00:00:00 with day-of-week BCD 4 - see
       rtc.h for why this isn't the previously-assumed arbitrary 1999-01-01. */
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

static void test_flash_bank_val_remapping(void) {
    /* F_BANK_VAL is indexed by
       PHYSICAL bank (table[p]=v, deliberately the "backwards" direction
       from a typical page table, see docs/hardware-notes.md, "Flash
       memory") - FLASH1 windowing is a real, potentially-reordering
       remapping table, not just a simple linear offset. This test exercises a non-contiguous
       mapping: physical blocks 2 and 5 enabled, with block 5 explicitly
       assigned to virtual bank 0 and block 2 to virtual bank 1 - the
       reverse of what a linear-offset model would produce. */
    psemu_t *ps = make_arm_cpu();

    psemu_bus_write32(&ps->bus, PSEMU_FLASH2_BASE + 2 * 8192, 0x22222222u);
    psemu_bus_write32(&ps->bus, PSEMU_FLASH2_BASE + 5 * 8192, 0x55555555u);

    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 8, (1u << 2) | (1u << 5)); /* F_BANK_FLG */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x100 + 5 * 4, 0u);        /* F_BANK_VAL[5] = virtual 0 */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x100 + 2 * 4, 1u);        /* F_BANK_VAL[2] = virtual 1 */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0, 2u);                    /* commit */

    /* Virtual bank 0 (FLASH1 offset 0) -> physical block 5, not the
       lowest-numbered enabled block (2) a linear-offset model would
       have picked. */
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

    /* Two real, confirmed bugs, both busy-wait loops in real BIOS/app code
       that this emulator silently hung forever - found only once real app
       execution got far enough to reach them (see
       docs/app-notes.md's app-dispatch investigation). */

    /* Bug 1: +0 (command/commit trigger) is write-command/read-status on
       real hardware, not a plain mirror. A real routine writes 2 here
       then busy-waits on this same address's bit 0 reading back 1
       ("ready"). Echoing back the raw command value (bit 0 of 2 is 0)
       left that loop spinning forever. */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0, 2u);
    assert((psemu_bus_read32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0) & 1u) != 0u);

    /* Bug 2: +0x10 (F_WAIT2, waitstates and FLASH-Write-
       Control-and-Status) wasn't modeled at all (span stopped at +0xC) -
       a real app's own flash-write routine polls bit 2 here, expecting
       it to read back set once the write completes. A default/unmapped
       read of 0 left that loop spinning too. Since writes complete
       instantly here, it should always report "not busy". */
    assert((psemu_bus_read32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x10) & 0x04u) != 0u);

    psemu_destroy(ps);
    printf("test_flash_ctrl_busy_wait_bits OK\n");
}

static void test_flash_serial_number_default_and_override(void) {
    /* F_SN (see docs/hardware-notes.md, "Hardware ID (F_SN)") defaults to
       0x410000D3 ("410000D3" in hex form) - low 24 bits 211, Chocobo World (FF8) masks off the
       high byte, reads the rest via SWI 0Ah, and uses its last 3 decimal
       digits as an "ID" stat that alone determines rank; 211 is the
       community-documented best rank. The high byte ('A') is the ASCII
       letter real hardware prints as part of its serial sticker - see
       psemu_parse_hardware_id/psemu_format_hardware_id. */
    psemu_t *ps = make_arm_cpu();
    assert(psemu_get_hardware_id(ps) == (((uint32_t)'A' << 24) | 211u));

    psemu_set_hardware_id(ps, 0x12345678u);
    assert(psemu_get_hardware_id(ps) == 0x12345678u);

    psemu_destroy(ps);
    printf("test_flash_serial_number_default_and_override OK\n");
}

static void test_hardware_id_string_conversion(void) {
    /* The only accepted form: exactly 8 plain hex digits, matching
       exactly what a real "ID rewriter" homebrew displays/edits on real
       hardware - confirmed via real-hardware testing that there's no
       "first digit must be a letter" restriction at all: a real unit
       happily accepts and persists "EEEEEEEE". Deliberately NOT also
       accepting the letter+8-decimal-digit "sticker" form real units
       print under their front cover (e.g. "A02374684") - what's in a
       persisted hardware-ID string is exactly the raw value, nothing
       hidden or silently translated; a sticker-to-raw-value converter, if
       ever wanted, belongs as a separate desktop-app feature. See
       docs/hardware-notes.md, "Hardware ID (F_SN)". */
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
    /* F_SN_LO/F_SN_HI live at FLASH_CTRL+0x300/+0x302 (F_EXTRA, see
       flash.h) - real hardware requires these read via two separate 16-bit
       halfword loads, not a single 32-bit one, but the underlying bytes
       are addressable the same way regardless; this exercises the bus at
       the halfword granularity real code actually uses. An ID-editing
       homebrew pokes these registers directly rather than going through
       the SWI, so the bus-level path needs to work independent of
       psemu_set_hardware_id. */
    psemu_t *ps = make_arm_cpu();

    /* Default is "A00000211" (see FLASH_DEFAULT_SERIAL) - F_SN_LO carries
       the low 16 bits (211, unaffected by the high byte), F_SN_HI carries
       the high 16 bits, which includes the 'A' letter byte (0x41) in its
       own top half. */
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x300) == 211u);
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x302) == 0x4100u);

    psemu_bus_write16(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x300, 0xBEEFu);
    psemu_bus_write16(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x302, 0xCAFEu);
    assert(psemu_get_hardware_id(ps) == 0xCAFEBEEFu);

    /* F_CAL (+0x308) is a real, separate register in the same F_EXTRA
       region - defaults to the documented reset value and is
       independently read/writable. */
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x308) == 0x001Au);
    psemu_bus_write16(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x308, 0x0099u);
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x308) == 0x0099u);

    /* The gap between F_BANK_VAL's end (+0x140) and F_EXTRA's start
       (+0x300) is unmapped and must stay 0, not mirror
       last_command now that FLASH_CTRL_SPAN reaches all the way to
       F_EXTRA. */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0, 2u); /* nonzero last_command */
    assert(psemu_bus_read32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x200) == 0u);

    /* Same for unknown bytes inside F_EXTRA itself, e.g. the reserved
       halfword between F_SN_HI and F_CAL. */
    assert(psemu_bus_read32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0x304) == 0u);

    psemu_destroy(ps);
    printf("test_flash_serial_number_register_access OK\n");
}

static void test_flash_load_app_synthesizes_directory(void) {
    /* A real, confirmed bug (see docs/app-notes.md, "App-selection and
       dispatch"): the real BIOS's app-selection routine requires FLASH2 to
       carry a real memory-card directory, not just the app's own bytes at
       offset 0 - flash_load_app used to write the raw Title Sector straight
       to offset 0, so a loaded single app could never actually be reached
       through the real menu. This test locks down the fix: a synthesized
       one-entry directory at slot 1, with the specific byte (frame offset
       0x10 = 'P') the real BIOS's menu-browsing code was empirically found
       to require - see the comment on DIRECTORY_POCKETSTATION_FLAG_OFFSET
       in flash.c for how that byte was isolated against a real card dump. */
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

    /* Slot 1 (first directory frame, at FLASH2 + 1*128): in-use/first
       marker, real file size, the PocketStation flag byte, and a link to
       slot 2 (0-based data-block index 1, since this is a 2-block chain). */
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
    /* flash_load_app now reserves physical block 0 for the synthesized
       directory (see test_flash_load_app_synthesizes_directory above), so
       the largest an app can be is 15 blocks, not the full 16-block flash -
       one less than it used to accept before that fix. Locks in the exact
       boundary so a future change can't silently widen or narrow it without
       a test noticing. */
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
    /* psemu_load_mcs had no direct test coverage at all - only ever
       exercised manually while adding .mcs support. Locks in: a
       well-formed single-save .mcs (real PS1 directory frame + data
       blocks) unwraps into the same synthesized-directory layout
       psemu_load_app produces, and the three ways a malformed .mcs is
       rejected (too short to contain a directory frame at all, a payload
       that isn't a whole number of blocks, and a directory frame whose
       stored file size doesn't match the actual payload). */
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
    /* psemu_load_content centralizes the .mcr/.mcs/.pss priority dispatch
       that used to be hand-duplicated in both frontends (and drifted out
       of sync between them once already, when the priority order changed).
       Locks in all four outcomes: full card, .mcs, bare .pss, and total
       garbage. */

    /* Full memory-card image: passed straight to psemu_load_flash_image,
       not reinterpreted as a Title Sector - a synthesized directory would
       force 'M','C' at offset 0, so a byte pattern that survives unchanged
       there proves the raw path was taken. */
    {
        psemu_t *ps = make_arm_cpu();
        static uint8_t card[PSEMU_FLASH_SIZE];
        memset(card, 0, sizeof(card));
        card[0] = 0xABu;
        assert(psemu_load_content(ps, card, sizeof(card)) == PSEMU_OK);
        assert(psemu_bus_read8(&ps->bus, PSEMU_FLASH2_BASE + 0) == 0xABu);
        psemu_destroy(ps);
    }

    /* .mcs-shaped input (directory frame + block-aligned payload): unwrapped
       and synthesized into a directory the same way psemu_load_mcs alone
       does. */
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

    /* Bare Title Sector, not shaped like a valid .mcs at all (8192 - 0x80
       isn't a multiple of FLASH_BLOCK_SIZE, so psemu_load_mcs rejects it
       on size alone before falling through to psemu_load_app). Still gets
       the same synthesized-directory treatment .pss always does. */
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
    /* A real, confirmed bug found via a real crash report (see
       docs/hardware-notes.md, "Flash memory"):
       F_KEY1 (0x08002A54) and F_KEY2 (0x080055AA) are real hardware's
       flash unlock-sequence trigger addresses, not data storage - real
       flash chips intercept writes there as unlock commands rather than
       actually storing them. This emulator used to just store them as
       plain data, so any real flash-write's unlock sequence permanently
       corrupted whatever byte happened to physically sit at those two
       fixed addresses - in Chocobo World's case, live app code. */
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

    /* Same protection applies through the FLASH1 virtual window (which
       indexes the same underlying data array directly, bypassing
       flash_write8) - with the default untouched bank state, FLASH1
       offset 0x2A54 resolves to the same physical offset. */
    psemu_bus_write16(&ps->bus, PSEMU_FLASH1_BASE + 0x2A54, 0xFF55u);
    assert(psemu_bus_read16(&ps->bus, PSEMU_FLASH1_BASE + 0x2A54) == 0x0000u);

    psemu_destroy(ps);
    printf("test_flash_key_addresses_are_not_data_storage OK\n");
}

/* Real, confirmed sequence (disassembled from a real ID-editing homebrew
   and matching official register documentation: "[8000000h]=new F_SN_LO
   value [8000002h]=new F_SN_HI value") - confirmed working on a real
   retail-BIOS unit this session. See docs/hardware-notes.md,
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

/* The path a PocketStation app uses to write the console game's PS1 save on the same card.

   An app can read that save directly - FLASH2 is the whole card, memory-mapped and unwindowed - but it
   cannot write it that way. Writes go through kernel SWI 0x10, one 128-byte frame at a time. Reverse
   engineered from the J110 BIOS handler at 0x0400126C, which: sets F_WAIT2 (FLASH_CTRL+0x10) to 0x21, runs
   the three-step unlock above, copies 0x40 halfwords to FLASH2 + frame*128, waits for F_WAIT2 bit 2 to read
   back set, and then *verifies* by comparing 32 words and returning 0 for success or 1 for mismatch. Yu-Gi-Oh
   Forbidden Memories drives it from 0x020028FA, deriving the frame number as (save_pointer >> 7) and
   retrying up to 10 times per frame.

   Every step of that has to work or the write fails silently: a retry loop that never succeeds simply gives
   up, exactly like the mode-bit test that used to discard IR transmissions. This replays the sequence
   directly rather than through the BIOS, so it needs no BIOS or app image and runs in CI. The readback
   assertion is the same comparison the real handler makes to decide success. */
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
    /* The unlock must not have redirected save data into F_SN/F_CAL: those cover FLASH2 offsets 0/2/8, and
       a frame this far into the card has to be stored as ordinary data instead. */
    assert(psemu_get_hardware_id(ps) == PSEMU_DEFAULT_HARDWARE_ID);

    psemu_destroy(ps);
    printf("test_flash_frame_write_lands_in_a_ps1_save_block OK\n");
}

static void test_flash_header_write_requires_unlock_first(void) {
    /* The core safety property motivating the gated (not unconditional)
       design: physical offset 0/2/8 is ALSO ordinary card-data storage
       (block 0's directory header, in the normal case, and - confirmed
       via the F_KEY1/F_KEY2 corruption bug above - real save-write
       mechanisms, like Chocobo World's own, write elsewhere in
       this same physical range). Without the real unlock
       sequence immediately before, a write to these offsets must behave
       as plain data, not silently redirect to F_SN/F_CAL. */
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
    /* Armed state covers exactly one real write session (a real header
       update is 3 separate halfword writes after a single unlock) and
       must not leak into unrelated later writes just because they
       happen to also land on offset 0/2/8. */
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
    /* LCD_MODE (0x0D000000) was previously entirely unmodeled: bit6 is DISON
       (display on/off) and bit7 is ROT (rotate 180 degrees, set for
       docked mode). psemu_get_framebuffer() now returns VRAM as filtered
       through these bits rather than raw VRAM unconditionally. */
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

    /* ROT set (with DISON re-set): 180-degree rotation - row order
       reversed, and each row's 32 bits reversed left-right. Row 0
       (0x000000FF, little-endian bytes 0xFF,00,00,00) ends up as the
       last row (offset 124-127) with bits reversed (0xFF000000, bytes
       00,00,00,0xFF); row 1 (0x0000FF00, bytes 00,0xFF,00,00) ends up
       second-to-last (offset 120-123), reversed to 0x00FF0000 (bytes
       00,00,0xFF,00). */
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

    /* Real hardware has no fixed sample rate of its own (software bit-
       bangs DAC_DATA) - dac_tick resamples at a fixed internal rate, so
       N cycles must produce N/DAC_CYCLES_PER_SAMPLE output samples. */
    dac_tick(&ps->dac, DAC_CYCLES_PER_SAMPLE * 3u);
    n = psemu_get_audio_samples(ps, samples, 4);
    assert(n == 3u);

    psemu_destroy(ps);
    printf("test_dac_basic OK\n");
}

static void test_iop_sound_gate_mutes_dac(void) {
    /* Audio must be enabled via
       BOTH DAC_CTRL bit0 AND IOP_STOP/IOP_START bit5 ("Sound Enable") -
       an earlier version of this emulator didn't model IOP_STOP/START
       at all, silently discarding writes to that address range. */
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
    /* A real, confirmed bug found while investigating a "Chocobo World
       plays no sound" report: direct BIOS/app tracing (see
       docs/hardware-notes.md) showed real code writes IOP_STOP/IOP_START
       via single-byte stores, not always full 32-bit ones. An earlier
       version of iop_write8 only committed a STOP/START write's effect
       once a full 32-bit store's top byte arrived (shift==24) -
       single-byte writes to the low byte (shift==0, where bit5 "Sound
       Enable" actually lives) never reached that gate and were silently
       discarded, leaving IOP_STOP/START permanently inert whenever real
       code uses single-byte stores instead of 32-bit ones. */
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
    /* psemu_reset must be a true hardware-level reset: every peripheral
       register returns to its power-on default, but loaded content
       (flash data, the hardware ID, RAM aside) is not content at all and
       must survive. See psemu_reset's comment in psemu.c. Without this,
       reloading a different BIOS/app mid-session left stale peripheral
       state from the previous session in place, which is what caused
       reload glitches. */
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

/* The BIOS-owned settings that live in RAM rather than in a register.
   See docs/hardware-notes.md, "System sound volume setting" and "Where the
   date/time settings actually live", for how these addresses were traced. */
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

/* psemu_set_volume alone is not enough to hold the setting, because emulated
   code overwrites the byte - the BIOS clears RAM early in its own boot, before
   it reads the volume during sound init, so a frontend that only writes
   between frames is always too late and the boot chime plays at full volume.
   psemu_set_volume_override closes that window by making the byte read-only to
   emulated code and re-seeding it across resets. */
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

/* The BIOS sets the clock by issuing RTC_ADJUST increments in a loop until
   each field reaches a target. Overwriting the registers underneath that
   loop can stop it converging, so the setter has to stay out of the way
   while PRGSEL is set and let the caller retry later. */
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

/* The override addresses are only meaningful on BIOS revisions this project
   has actually traced. Anything else has to fail closed, so a frontend can
   disable the UI instead of corrupting unrelated kernel RAM. */
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

/* The per-frame settings overrides above are only safe while the BIOS shell
   owns its RAM. psemu_app_running is what tells a frontend when it no longer
   does. See its comment in psemu/psemu.h for the bug that made it necessary. */
static void test_app_running_follows_flash1_execution(void) {
    psemu_t *ps = psemu_create();
    ps->has_bios = 1; /* psemu_run is a no-op without a loaded BIOS */

    /* Nothing has run yet, so the BIOS shell is presumed to own the machine. */
    assert(psemu_app_running(ps) == 0);

    /* Park an app in FLASH1 that just spins: B . at the window's base, plus
       the same instruction one bank in, so bank resolution is not what this
       test depends on. */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH2_BASE + 0, 0xEAFFFFFEu);
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 8, 1u); /* F_BANK_FLG: enable block 0 */
    psemu_bus_write32(&ps->bus, PSEMU_FLASH_CTRL_BASE + 0, 2u); /* commit */
    assert(psemu_bus_read32(&ps->bus, PSEMU_FLASH1_BASE) == 0xEAFFFFFEu);

    /* Executing from the FLASH1 window is what "an app is running" means. */
    arm7tdmi_reset(&ps->cpu, PSEMU_FLASH1_BASE);
    psemu_run(ps, 1000u);
    assert(psemu_app_running(ps) == 1);

    /* An app sits in the BIOS for the length of every SWI it issues. A short
       excursion must not read as "the app exited", or the frontend resumes
       stamping its overrides into the app's RAM mid-call. */
    put32(ps, 0, 0xEAFFFFFEu); /* B . in RAM, standing in for a BIOS routine */
    arm7tdmi_reset(&ps->cpu, 0);
    psemu_run(ps, 1000u);
    assert(psemu_app_running(ps) == 1);

    /* Staying away long enough is a real handover back to the BIOS shell. The
       grace period is a real-time duration at the reference rate, so a budget
       in the same unit clears it at any CLK_MODE - which is the property that
       makes this tractable to assert at all. */
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
    printf("all cpu tests passed\n");
    return 0;
}
