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
/* Sources whose live signal level is visible in STATUS (the register real documentation calls INT_INPUT,
   "Raw Interrupt Signal Levels", at 0x0A000004 - see docs/hardware-notes.md, "Buttons").
   Buttons (bits 0-4) and RTC (bit 9) were confirmed here first.
   INT_IRDA (bit 12) was added after disassembling a real app's IR receive handler: its very first action,
   after acknowledging the interrupt, is to read STATUS, isolate bit 12, and compare that live level against
   the level it was expecting - bailing out immediately on a mismatch. With bit 12 missing from this mask,
   STATUS bit 12 always read back 0, so that comparison could never succeed and no IR transfer could ever
   decode. See core/src/ir.c's apply_rx_level and docs/hardware-notes.md, "IR / IR Link". */
#define INT_STATUS_MASK 0x0000121Fu

/* Sources whose STATUS bit is a continuously-driven signal level rather than a latched request, and so is
   NOT cleared by an acknowledge write.
   Confirmed for IR by disassembling a real app's INT_IRDA handler: it acknowledges INT_IRDA and only then
   reads STATUS back to sample the live line level. That order is only meaningful if acknowledging leaves the
   level alone - otherwise the handler would be guaranteed to read its own acknowledge's after-effect (0)
   instead of the real signal, which is exactly what happened before this mask existed.
   Buttons are deliberately NOT included here, even though real INT_INPUT exposes their raw level too: their
   STATUS/HOLD handling is already pinned down by confirmed real-hardware behavior (see intc_clear_hold_only
   and psemu_set_buttons), and nothing observed needs changing there. */
#define INT_LEVEL_MASK INT_IRDA

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

/* Tracks `line`'s live signal level in STATUS, while latching an interrupt request in HOLD on every call
   regardless of which direction the level just moved.
   IR receive needs exactly this split, and intc_set_line cannot express it: the handler must be entered on
   both edges of a pulse (that is how it measures the pulse's width), but STATUS has to keep reporting the
   real current line level for the handler's own level check to work. Passing state=0 to intc_set_line would
   instead clear HOLD and skip the interrupt entirely. */
void intc_set_level_and_pulse(intc_t *intc, uint32_t line, int level);

int intc_irq_asserted(intc_t *intc);
int intc_fiq_asserted(intc_t *intc);

#endif
