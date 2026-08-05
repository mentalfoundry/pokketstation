#ifndef POKKETSTATION_CONTENT_WRITEBACK_H
#define POKKETSTATION_CONTENT_WRITEBACK_H

#include <stddef.h>
#include <stdint.h>

#include "psemu/psemu.h"

/* Keeps the file on disk in agreement with the changes that an app makes to the loaded content.

   A real PocketStation writes to the card in its slot. Two different kinds of data get to flash by
   this method, and both are important:

   - an app that saves ITS OWN progress into its own blocks, which is what most apps do; and
   - an app that edits the PS1 save of the CONSOLE GAME, in a different block of the same card. One
     trading-card app sends cards into the save of its own game in this manner (see
     docs/app-notes.md).

   This code does not separate the two kinds. An app reaches its own blocks through the FLASH1 banked
   window, and it reaches the PS1 save through FLASH2. But flash1_write8 resolves the bank into the
   same storage (core/src/flash.c). Thus one comparison covers both kinds.

   ALL THREE CONTENT KINDS ROUND-TRIP. Each one goes back into the shape of its source file:

   - A .mcr file for a full card goes back as a full card.
   - A .mcs file is built again: its own directory frame of 0x80 bytes, unchanged from the file, and
     then the body of the app from flash. The frame gives the properties of the file, and not its
     contents, and an app cannot reach the frame. Thus the loaded copy is still correct.
   - A .pss file is the body alone.

   For a .mcs or .pss file, ONLY THE BLOCKS OF THE APP ARE IN THE FILE. A loaded app operates in a
   memory card that this emulator synthesizes around it. The remainder of that card, which is the
   directory and each other block, has no space in the file. Thus the comparison that finds a change
   covers exactly the region that this code writes, and no more. Without this limit, an app that
   writes to the synthesized directory marks the file as changed permanently. This code then writes
   the file again in a loop, always with the same bytes.

   The baseline is flash immediately after the load. Thus "the app changed something" is a real
   comparison against the loaded data, and not an assumption. Two paths replace all of flash with no
   app write: a reset, and a save-state load. Those paths call content_writeback_resync, which accepts
   the new contents and does not treat them as an edit.

   Safety, because this code writes over a file of the user:
   - This code copies the unchanged file to "<path>.bak" before the first write of a change, and only
     if no .bak file is present. One backup is sufficient. To write over an older backup with a newer,
     already-changed file removes the value of the backup.
   - This code writes the new file to "<path>.tmp", and then moves it over the original file. Thus an
     interrupted write or a failed write cannot leave a truncated file.
   - A failed write does not change the file on disk, and this code writes a message to stderr. It does
     not fail without a message. */

/* Approximately 1 second, at the approximately 32Hz rate of the desktop loop. An app that writes a
   save over several frames makes one edit, and not one edit for each frame. Thus a commit waits this
   time after the first observed change, and covers the full group of writes. */
#define CONTENT_WRITEBACK_SETTLE_FRAMES 32

/* The first PS1 directory frame of a .mcs file. This is the same value as MCS_HEADER_SIZE in psemu.c,
   which is private to that file. */
#define CONTENT_WRITEBACK_MCS_FRAME_SIZE 0x80u

typedef struct {
    int enabled;
    psemu_content_kind kind;
    char path[1024];
    /* The region of flash that this file can hold: the full card, or only the body of the app. */
    size_t region_offset;
    size_t region_size;
    uint8_t mcs_frame[CONTENT_WRITEBACK_MCS_FRAME_SIZE]; /* unchanged from the loaded .mcs file */
    uint8_t baseline[PSEMU_FLASH_SIZE];
    uint8_t current[PSEMU_FLASH_SIZE];
    int dirty;
    unsigned long dirty_since_frame;
} content_writeback_t;

/* Arms the write-back function for the file at `path`. `data` and `size` are the bytes of that file,
   exactly as psemu_load_content received them. Call this function after each successful load. This
   function stays disarmed for content that psemu_identify_content does not recognize, and for a NULL
   `data`. */
void content_writeback_arm(
    content_writeback_t *cw, psemu_t *ps, const char *path, const uint8_t *data, size_t size);

/* Uses the current contents of flash as the new baseline. This function writes no file. It is for the
   paths that replace all of flash, and that do not emulate a write to flash. */
void content_writeback_resync(content_writeback_t *cw, psemu_t *ps);

/* Writes the file now, if this function is armed and the content changed. Returns a nonzero value if
   it wrote a file. */
int content_writeback_commit(content_writeback_t *cw, psemu_t *ps);

/* Call this function one time for each frame. It finds a change, and then waits
   CONTENT_WRITEBACK_SETTLE_FRAMES frames before it writes the file.
   `frame` is the frame counter of the caller. That counter only increases. */
void content_writeback_poll(content_writeback_t *cw, psemu_t *ps, unsigned long frame);

#endif
