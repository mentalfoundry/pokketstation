/* See the note at the top of cpu_test.c: a Release build's NDEBUG compiles every assert() away, so this
   suite has to keep them itself. Must precede <assert.h>. */
#undef NDEBUG

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "psemu/psemu.h"

int main(void) {
    psemu_t *ps = psemu_create();
    assert(ps != NULL);

    uint8_t bios[PSEMU_BIOS_SIZE];
    memset(bios, 0, sizeof(bios));
    assert(psemu_load_bios(ps, bios, sizeof(bios)) == PSEMU_OK);
    assert(psemu_load_bios(ps, bios, sizeof(bios) - 1) == PSEMU_ERR_BAD_SIZE);

    uint8_t app[0x90];
    memset(app, 0, sizeof(app));
    memcpy(&app[0x52], "MCX0", 4);
    assert(psemu_load_app(ps, app, sizeof(app)) == PSEMU_OK);
    assert(psemu_load_app(ps, app, 4) == PSEMU_ERR_BAD_SIZE);

    psemu_reset(ps);
    uint32_t ran = psemu_run(ps, 100);
    /* Not "ran >= 100": psemu_run's argument is a time budget at a
       reference clock rate (see clk.h), and CLK_MODE defaults to the
       low-power idle rate until something writes it - a slower real
       clock than the reference means fewer raw cycles fit in the same
       budget. Just check forward progress happened at all. */
    assert(ran >= 1);

    const uint8_t *fb = psemu_get_framebuffer(ps);
    assert(fb != NULL);

    /* psemu_flash_data/psemu_ram_data hand a frontend's host a pointer it keeps and writes to on its
       own schedule (libretro's RETRO_MEMORY_SAVE_RAM works this way), so the addresses have to stay
       put across everything a session does - a state load especially, since that one memcpy's over the
       whole psemu_t. Nothing warns if that stops being true; the host just scribbles on freed memory
       or silently persists a stale region. */
    uint8_t *flash_ptr = psemu_flash_data(ps);
    uint8_t *ram_ptr = psemu_ram_data(ps);
    assert(flash_ptr != NULL);
    assert(ram_ptr != NULL);
    /* The region really is the card: what psemu_save_flash_image copies out, without the copy. */
    uint8_t flash_copy[PSEMU_FLASH_SIZE];
    assert(psemu_save_flash_image(ps, flash_copy, sizeof(flash_copy)) == PSEMU_OK);
    assert(memcmp(flash_copy, flash_ptr, PSEMU_FLASH_SIZE) == 0);

    size_t state_size = psemu_state_size(ps);
    uint8_t *state = (uint8_t *)malloc(state_size);
    assert(psemu_save_state(ps, state, state_size) == PSEMU_OK);
    assert(psemu_load_state(ps, state, state_size) == PSEMU_OK);
    free(state);
    assert(psemu_flash_data(ps) == flash_ptr);
    assert(psemu_ram_data(ps) == ram_ptr);
    psemu_reset(ps);
    assert(psemu_flash_data(ps) == flash_ptr);
    assert(psemu_ram_data(ps) == ram_ptr);

    /* A host dump of this region is a .mcr, and that is a contract users depend on: the saves section
       of docs/libretro_readme.md tells them to open a .srm in external memory-card tools. Narrowing
       the region or prefixing a header would break every file an older build already wrote, so pin the
       property that makes it true. */
    assert(psemu_identify_content(flash_ptr, PSEMU_FLASH_SIZE) == PSEMU_CONTENT_CARD);

    psemu_destroy(ps);
    return 0;
}
