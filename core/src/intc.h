#ifndef PSEMU_INTC_H
#define PSEMU_INTC_H

#include <stdint.h>

#define INTC_REG_SPAN 0x14u

/* Real PocketStation interrupt controller and sources.
   Independently corrected against a real BIOS disassembly; see docs/hardware-notes.md.

   Registers at 0x0A000000:
   - hold (+0x0): read-only from software's view. Real hardware treats a write here as invalid.
   - status (+0x4): likewise read-only.
   - enable (+0x8): a write ORs bits in.
   - mask (+0xC): a write ANDs matching bits out of enable.
   - acknowledge (+0x10): write-only. Clears matching bits from both hold and status.

   Every asserted source latches into HOLD. HOLD, gated by ENABLE, drives the CPU's IRQ/FIQ lines.
   Button presses and the RTC tick (bits within INT_STATUS_MASK) also latch into STATUS.
   STATUS lets code poll a source directly without disturbing the interrupt-delivery state, for example the
   RTC wait-for-pulse loop.

   History: this codebase's own interrupt-routing logic originally put STATUS_MASK bits into `status` only,
   never into `hold`. A real BIOS disassembly showed this was wrong.
   The BIOS's top-level IRQ handler tests `hold & enable & 0x200` (RTC).
   Its installed periodic callback tests `hold & 1` (Action button).
   Both land on real handlers, confirmed by tracing them, that could never run under the old status-only routing.
   Real hardware asserts these sources into both registers; this emulator now matches that. */
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
/* Sources whose live signal level is visible in STATUS.
   Real documentation calls that register INT_INPUT, "Raw Interrupt Signal Levels", at 0x0A000004.
   See docs/hardware-notes.md, "Buttons".
   Buttons (bits 0-4) and RTC (bit 9) were confirmed here first.
   INT_IRDA (bit 12) came later, from a disassembly of a real app's IR receive handler.
   That handler acknowledges the interrupt. It then reads STATUS, isolates bit 12, and compares that live
   level against the level it expected. It bails out at once on a mismatch.
   While bit 12 was missing from this mask, STATUS bit 12 always read back 0.
   That comparison could therefore never succeed, and no IR transfer could ever decode.
   See core/src/ir.c's apply_rx_level, and docs/hardware-notes.md, "IR / IR Link". */
#define INT_STATUS_MASK 0x0000121Fu

/* Sources whose STATUS bit is a continuously-driven signal level, not a latched request.
   An acknowledge write does NOT clear these bits.
   A disassembly of a real app's INT_IRDA handler confirms this for IR.
   That handler acknowledges INT_IRDA, and only then reads STATUS back to sample the live line level.
   That order makes sense only if an acknowledge leaves the level alone.
   Otherwise the handler always reads the after-effect of its own acknowledge, which is 0, instead of the real
   signal. That is exactly what happened before this mask existed.
   Buttons are in this mask for the same reason. docs/hardware-notes.md, "Buttons", states that `status`
   tracks the live button level for code that polls it directly. An acknowledge used to clear that level, so
   a button that was still physically held read back as released for as long as nothing pressed it again.
   The confirmed real-hardware finding about buttons is about HOLD, not STATUS: HOLD is a momentary edge
   pulse per press, which intc_clear_hold_only and psemu_set_buttons implement and
   test_button_hold_pulses_not_sustained covers. That finding says nothing about STATUS, and this mask does
   not change it.
   A real app depends on the live level. pk_timing_bench holds Action to open its exit prompt, and counts
   75000 consecutive polls of STATUS to detect the hold. Only a level that survives an acknowledge can
   accumulate that. */
#define INT_LEVEL_MASK     (INT_IRDA | INT_BTN_ACTION | INT_BTN_RIGHT | INT_BTN_LEFT | INT_BTN_DOWN | INT_BTN_UP)

typedef struct intc {
    uint32_t hold;
    uint32_t status;
    uint32_t enable;
    uint32_t mask;
    /* Byte-write accumulators.
       Real code always performs a clean 32-bit store.
       This emulator applies the semantic effect (OR into enable, etc.) once the top byte of that store lands. */
    uint32_t enable_write_scratch;
    uint32_t mask_write_scratch;
    uint32_t ack_write_scratch;
} intc_t;

void intc_init(intc_t *intc);
uint8_t intc_read8(intc_t *intc, uint32_t offset);
void intc_write8(intc_t *intc, uint32_t offset, uint8_t value);

/* TEMPORARY diagnostic flag - see intc.c. */
extern int psemu_intc_trace_enabled;

/* Sets or clears an interrupt source (see INT_* above).
   Routes it to STATUS or HOLD per INT_STATUS_MASK, mirroring real hardware's interrupt-routing logic.
   Passing line=0 is a no-op.
   intc_irq_asserted/intc_fiq_asserted always compute the asserted state on demand.
   Unlike real hardware, there is no separate "recompute" step to trigger. */
void intc_set_line(intc_t *intc, uint32_t line, int state);
uint32_t intc_get_line(intc_t *intc, uint32_t line);

/* Clears `line` from HOLD only, leaving STATUS untouched.
   Use this for sources whose HOLD pulse should represent only the initiating edge, not a sustained level.

   See psemu_set_buttons in psemu.c for why buttons need this.
   A real BIOS callback branches on `hold` to decide which source to service.
   A held button whose hold bit never clears would permanently starve every other source checked later in
   that same branch chain.

   This is confirmed via a real-hardware discrepancy: the button-action branch sits before the RTC check in
   the callback. A continuously-set hold bit would block RTC-driven redraws for as long as the button stays held.
   Real hardware instead keeps redrawing normally, and only acts on release. */
void intc_clear_hold_only(intc_t *intc, uint32_t line);

/* Tracks `line`'s live signal level in STATUS.
   It also latches an interrupt request in HOLD on every call, in both level directions.
   IR receive needs exactly this split, and intc_set_line cannot express it.
   The handler must run on both edges of a pulse, because that is how it measures the pulse width.
   STATUS must keep reporting the real current line level, because the handler checks that level itself.
   A call to intc_set_line with state=0 would instead clear HOLD and skip the interrupt. */
void intc_set_level_and_pulse(intc_t *intc, uint32_t line, int level);

int intc_irq_asserted(intc_t *intc);
int intc_fiq_asserted(intc_t *intc);

#endif
