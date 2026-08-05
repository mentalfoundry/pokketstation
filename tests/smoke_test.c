/* See the comment at the top of cpu_test.c. NDEBUG in a Release build removes each assert() call, thus
   this test suite must keep them. This code must come before <assert.h>. */
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
    /* This test does not use "ran >= 100". The argument of psemu_run is a
       time budget at a reference clock rate (see clk.h). CLK_MODE has a
       default of the low-power idle rate, until code writes the register.
       A real clock that is slower than the reference rate fits less raw
       cycles in the same budget. Thus this test only confirms that the
       emulator advanced. */
    assert(ran >= 1);

    const uint8_t *fb = psemu_get_framebuffer(ps);
    assert(fb != NULL);

    /* psemu_flash_data and psemu_ram_data give the host of a frontend a pointer. The host keeps that
       pointer and writes to it on its own schedule. The save-memory interface of libretro operates
       this way. Thus the addresses must stay constant through each operation of a session. A state
       load is the most important operation, because it copies over the full psemu_t structure. No
       code gives a warning if this stops being true: the host then writes to memory that is no
       longer valid, or it keeps an old region and gives no message. */
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

    /* A dump of this region by a host is a .mcr file. This is a contract that users depend on: the
       saves section of docs/libretro_readme.md tells them to open the save file in external
       memory-card tools. A smaller region, or an added header, makes each file from an older build
       invalid. Thus this test protects the property that makes the contract true. */
    assert(psemu_identify_content(flash_ptr, PSEMU_FLASH_SIZE) == PSEMU_CONTENT_CARD);

    psemu_destroy(ps);
    return 0;
}
