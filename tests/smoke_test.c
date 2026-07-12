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

    size_t state_size = psemu_state_size(ps);
    uint8_t *state = (uint8_t *)malloc(state_size);
    assert(psemu_save_state(ps, state, state_size) == PSEMU_OK);
    assert(psemu_load_state(ps, state, state_size) == PSEMU_OK);
    free(state);

    psemu_destroy(ps);
    return 0;
}
