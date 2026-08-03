#ifndef POKKETSTATION_CONTENT_WRITEBACK_H
#define POKKETSTATION_CONTENT_WRITEBACK_H

#include <stddef.h>
#include <stdint.h>

#include "psemu/psemu.h"

/* Keeps the file on disk in step with what an app has done to what was loaded from it.

   A real PocketStation writes the card it is plugged into. Two different things reach flash that way,
   and both matter:

   - an app saving ITS OWN progress into its own blocks, which is what most apps do; and
   - an app editing the CONSOLE GAME'S PS1 save, in another block of the same card - Yu-Gi-Oh Forbidden
     Memories trades cards into the game's own save this way (see docs/app-notes.md).

   Nothing here distinguishes the two. An app's own blocks are reached through the FLASH1 banked window
   and the PS1 save through FLASH2, but flash1_write8 resolves the bank into the same storage
   (core/src/flash.c), so one comparison covers both.

   ALL THREE CONTENT KINDS ROUND-TRIP, each back into the shape of the file it came from:

   - A whole-card .mcr is written back whole.
   - A .mcs is rebuilt: its own 0x80-byte directory frame, kept verbatim from the file, followed by the
     app's body copied back out of flash. The frame describes the file rather than its contents and an
     app cannot reach it, so the loaded copy is still correct.
   - A .pss is the body alone.

   For a .mcs or .pss, ONLY THE APP'S OWN BLOCKS EXIST IN THE FILE. A loaded app runs inside a memory
   card this emulator synthesizes around it, and the rest of that card - the directory, every other
   block - has nowhere to go in the file. So the comparison that decides "changed" covers exactly the
   region that will be written and nothing else. Otherwise an app scribbling on the synthesized
   directory would mark the file dirty forever and rewrite it on a loop, always with the same bytes.

   The baseline is flash as it stood right after the load, so "the app changed something" is a real
   comparison against what was loaded rather than an assumption. Two paths replace flash wholesale
   without any app writing to it - a reset and a save-state load - and those call
   content_writeback_resync, which adopts the new contents rather than treating them as an edit.

   Safety, because this overwrites a user's own file:
   - A pristine copy goes to "<path>.bak" the first time a change is about to be written, and only if no
     .bak is already there. A backup is worth having exactly once; overwriting an older one with a
     newer, already-modified file would defeat the point of keeping it.
   - The new file is written to "<path>.tmp" and then moved over the original, so an interrupted or
     failed write cannot leave a truncated file behind.
   - A failed write leaves the file on disk untouched and says so on stderr, rather than failing
     silently. */

/* ~1s at the desktop loop's ~32Hz. An app rewriting several frames of a save is one edit, not one per
   frame, so a commit waits this long after the first observed change and covers the whole burst. */
#define CONTENT_WRITEBACK_SETTLE_FRAMES 32

/* A .mcs's leading PS1 directory frame. Same value as psemu.c's MCS_HEADER_SIZE, which is private. */
#define CONTENT_WRITEBACK_MCS_FRAME_SIZE 0x80u

typedef struct {
    int enabled;
    psemu_content_kind kind;
    char path[1024];
    /* The region of flash this file can represent: the whole card, or just the app's own body. */
    size_t region_offset;
    size_t region_size;
    uint8_t mcs_frame[CONTENT_WRITEBACK_MCS_FRAME_SIZE]; /* verbatim from the loaded .mcs */
    uint8_t baseline[PSEMU_FLASH_SIZE];
    uint8_t current[PSEMU_FLASH_SIZE];
    int dirty;
    unsigned long dirty_since_frame;
} content_writeback_t;

/* Arms write-back for the file at `path`, whose bytes are `data`/`size` exactly as they were handed to
   psemu_load_content. Call after every successful load. Anything psemu_identify_content does not
   recognise leaves this disarmed, and so does a NULL `data`. */
void content_writeback_arm(
    content_writeback_t *cw, psemu_t *ps, const char *path, const uint8_t *data, size_t size);

/* Adopts flash as it currently stands as the new baseline, writing nothing. For the paths that replace
   flash wholesale rather than emulate a write to it. */
void content_writeback_resync(content_writeback_t *cw, psemu_t *ps);

/* Writes the file out now, if armed and changed. Returns nonzero if a file was written. */
int content_writeback_commit(content_writeback_t *cw, psemu_t *ps);

/* Once per frame. Notices a change, then waits CONTENT_WRITEBACK_SETTLE_FRAMES before committing.
   `frame` is the caller's own monotonically increasing frame counter. */
void content_writeback_poll(content_writeback_t *cw, psemu_t *ps, unsigned long frame);

#endif
