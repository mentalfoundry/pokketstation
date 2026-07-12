#ifndef PSEMU_INTC_H
#define PSEMU_INTC_H

#include <stdint.h>

#define INTC_REG_SPAN 0x14u

/* Real PocketStation interrupt controller and sources, independently
   corrected against a real BIOS disassembly - see docs/hardware-notes.md. Registers
   at 0x0A000000: hold(+0x0, read-only from software's view - "invalid
   write" per real hardware), status(+0x4, likewise read-only), enable
   (+0x8, write ORs bits in), mask(+0xC, write ANDs matching bits out of
   enable), acknowledge(+0x10, write-only, clears matching bits from both
   hold and status).

   Every asserted source latches into HOLD, which (gated by ENABLE) drives
   the CPU's IRQ/FIQ lines. Button presses and the RTC tick (bits within
   INT_STATUS_MASK) ALSO latch into STATUS, for direct polling without
   disturbing the interrupt-delivery state (e.g. the RTC wait-for-pulse
   loop). This codebase's own interrupt-routing logic originally put
   STATUS_MASK bits into `status` ONLY, never `hold` - that was the
   initial implementation, but a real BIOS disassembly showed its top-level
   IRQ handler testing `hold & enable & 0x200` (RTC) and its installed
   periodic callback testing `hold & 1` (Action button), both landing on
   real handlers (confirmed by tracing them) that could never run under
   that status-only routing. Real hardware evidently
   asserts these sources into both registers. */
#define INT_BTN_ACTION 0x00000001u
#define INT_BTN_RIGHT 0x00000002u
#define INT_BTN_LEFT 0x00000004u
#define INT_BTN_DOWN 0x00000008u
#define INT_BTN_UP 0x00000010u
#define INT_UNKNOWN 0x00000020u
#define INT_COM 0x00000040u
#define INT_TIMER0 0x00000080u
#define INT_TIMER1 0x00000100u
#define INT_RTC 0x00000200u
#define INT_BATTERY 0x00000400u
#define INT_IOP 0x00000800u
#define INT_IRDA 0x00001000u
#define INT_TIMER2 0x00002000u
#define INT_IRQ_MASK 0x00001FBFu
#define INT_FIQ_MASK 0x00002040u
#define INT_STATUS_MASK 0x0000021Fu

typedef struct intc {
    uint32_t hold;
    uint32_t status;
    uint32_t enable;
    uint32_t mask;
    /* Byte-write accumulators: real code always does a clean 32-bit store,
       so the semantic effect (OR into enable, etc.) is applied once the
       top byte of such a store lands. */
    uint32_t enable_write_scratch;
    uint32_t mask_write_scratch;
    uint32_t ack_write_scratch;
} intc_t;

void intc_init(intc_t *intc);
uint8_t intc_read8(intc_t *intc, uint32_t offset);
void intc_write8(intc_t *intc, uint32_t offset, uint8_t value);

/* TEMPORARY diagnostic flag - see intc.c. */
extern int psemu_intc_trace_enabled;

/* Sets or clears an interrupt source (see INT_* above), routing it to
   STATUS or HOLD per INT_STATUS_MASK - mirrors real hardware's
   interrupt-routing logic. Passing line=0 is a no-op (asserted state is always
   computed on demand by intc_irq_asserted/intc_fiq_asserted, so unlike
   real hardware there is no separate "recompute" step to trigger). */
void intc_set_line(intc_t *intc, uint32_t line, int state);
uint32_t intc_get_line(intc_t *intc, uint32_t line);

/* Clears `line` from HOLD only, leaving STATUS untouched - used for
   sources whose HOLD pulse should represent only the initiating edge,
   not a sustained level for as long as the source's live condition
   remains true (see psemu_set_buttons in psemu.c for why buttons need
   this: a real BIOS callback branches on `hold` to decide which source
   to service, and a held button whose hold bit never clears would
   permanently starve every other source checked after it in that same
   branch chain - confirmed via a real-hardware discrepancy: the button-
   action branch sits before the RTC check in the callback, so a
   continuously-set hold bit would block RTC-driven redraws for as long
   as the button is held, when real hardware evidently keeps redrawing
   normally and only acts on release). */
void intc_clear_hold_only(intc_t *intc, uint32_t line);

int intc_irq_asserted(intc_t *intc);
int intc_fiq_asserted(intc_t *intc);

#endif
