#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum { OSG_JOURNAL_PAGE_SIZE=2048, OSG_JOURNAL_CAPACITY=1984 };
typedef struct {
    void *context;
    const uint8_t *(*read)(void *,unsigned page);
    bool (*erase)(void *,unsigned page);
    bool (*program)(void *,unsigned page,unsigned offset,const uint8_t doubleword[8]);
} osg_journal_io_t;
bool osg_journal_load(const osg_journal_io_t *,uint8_t *,size_t,size_t *,uint32_t *);
bool osg_journal_commit(const osg_journal_io_t *,const uint8_t *,size_t,uint32_t);
/* Distinguish a virgin device from corrupt nonblank records before using epoch0. */
bool osg_journal_erased(const osg_journal_io_t *);
