@ The four (plus a diagnostic) memory-timing experiments. See ../README.md
@ for what each screen shows and what its numbers mean.
    .syntax unified
    .arm

    .include "constants.inc"

    .section .text, "ax"
    .global run_experiment_1_sanity
    .global run_experiment_2_flashctrl
    .global run_experiment_3_bios_arm
    .global run_experiment_4_bios_thumb

@ Experiment 1: sanity check - ARM vs Thumb opcode-fetch cost (~2:1 expected)
run_experiment_1_sanity:
    push {lr}
    ldr r0, =WRAM_DATA_SCRATCH
    ldr r1, =LOOP_N
    bl measure_loop_ptr
    ldr r1, =(WRAM_RESULTS_BASE + 0)
    str r0, [r1]

    ldr r0, =WRAM_DATA_SCRATCH
    ldr r1, =LOOP_N
    adr lr, exp1_thumb_ret
    ldr r2, =measure_loop_ptr_thumb    @ .thumb_func-tagged - bit0 set automatically
    bx r2
exp1_thumb_ret:
    ldr r1, =(WRAM_RESULTS_BASE + 4)
    str r0, [r1]
    pop {lr}
    bx lr
    .ltorg

@ Experiment 2: FLASH_CTRL vs WRAM data-access cost
run_experiment_2_flashctrl:
    push {lr}
    ldr r0, =FLASH_CTRL_TEST_ADDR
    ldr r1, =LOOP_N
    bl measure_loop_ptr
    ldr r1, =(WRAM_RESULTS_BASE + 8)
    str r0, [r1]

    ldr r0, =WRAM_DATA_SCRATCH
    ldr r1, =LOOP_N
    bl measure_loop_ptr
    ldr r1, =(WRAM_RESULTS_BASE + 12)
    str r0, [r1]
    pop {lr}
    bx lr
    .ltorg

@ Experiment 3: real BIOS ARM helper (0x04001BC8) vs WRAM copy
run_experiment_3_bios_arm:
    push {lr}
    ldr r1, =LOOP_N
    bl measure_bios_call_loop_real
    ldr r1, =(WRAM_RESULTS_BASE + 16)
    str r0, [r1]

    ldr r1, =LOOP_N
    bl measure_bios_call_loop_wram
    ldr r1, =(WRAM_RESULTS_BASE + 20)
    str r0, [r1]
    pop {lr}
    bx lr
    .ltorg

@ Experiment 4: real BIOS Thumb helper (0x04001320|1) vs WRAM copy
run_experiment_4_bios_thumb:
    push {lr}
    ldr r1, =LOOP_N
    bl measure_bios_thumb_call_loop_real
    ldr r1, =(WRAM_RESULTS_BASE + 24)
    str r0, [r1]

    ldr r1, =LOOP_N
    bl measure_bios_thumb_call_loop_wram
    ldr r1, =(WRAM_RESULTS_BASE + 28)
    str r0, [r1]
    pop {lr}
    bx lr
    .ltorg
