#include "flash.h"

#include <string.h>

#define TITLE_SECTOR_HEADER_SIZE 0x80u
#define TITLE_SECTOR_MAGIC_OFFSET 0x52u

void flash_init(flash_t *flash) {
    memset(flash->data, 0, sizeof(flash->data));
}

psemu_status flash_load_app(flash_t *flash, const uint8_t *data, size_t size) {
    if (size < TITLE_SECTOR_HEADER_SIZE || size > sizeof(flash->data)) {
        return PSEMU_ERR_BAD_SIZE;
    }
    if (memcmp(&data[TITLE_SECTOR_MAGIC_OFFSET], "MCX0", 4) != 0 &&
        memcmp(&data[TITLE_SECTOR_MAGIC_OFFSET], "MCX1", 4) != 0) {
        return PSEMU_ERR_BAD_FORMAT;
    }
    memset(flash->data, 0, sizeof(flash->data));
    memcpy(flash->data, data, size);
    return PSEMU_OK;
}

uint8_t flash_read8(flash_t *flash, uint32_t addr) {
    return flash->data[addr % PSEMU_FLASH_SIZE];
}
