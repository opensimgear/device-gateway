#pragma once
#include "gateway_journal.h"

/* Each logical 2 KiB journal page has its own independently erasable sector. */
enum { OSG_PICO_STORAGE_JOURNALS=9, OSG_PICO_FLASH_SECTOR_SIZE=4096,
       OSG_PICO_FLASH_PROGRAM_SIZE=256,
       OSG_PICO_STORAGE_BYTES=OSG_PICO_STORAGE_JOURNALS*2*OSG_PICO_FLASH_SECTOR_SIZE };
typedef struct {
    const uint8_t *mapped_flash;
    size_t size,image_size;
    bool (*erase)(size_t offset,size_t size);
    bool (*program)(size_t offset,const uint8_t *data,size_t size);
} osg_pico_flash_t;
bool osg_pico_storage_init(const osg_pico_flash_t *);
/* Index 0 is the gateway registry; indices 1..8 are virtual module settings. */
osg_journal_io_t osg_pico_storage_io(unsigned index);
