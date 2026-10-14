#include "pico_storage.h"
#include <string.h>

_Static_assert((int)OSG_JOURNAL_PAGE_SIZE<=(int)OSG_PICO_FLASH_SECTOR_SIZE,"Journal fits its sector");
static osg_pico_flash_t flash;
static struct { size_t offset; uint8_t data[OSG_PICO_FLASH_PROGRAM_SIZE]; bool dirty; } pending;
static unsigned journals[OSG_PICO_STORAGE_JOURNALS];

static bool flush(void) {
    if(!pending.dirty) return true;
    if(!flash.program(pending.offset,pending.data,sizeof pending.data) ||
       memcmp(flash.mapped_flash+pending.offset,pending.data,sizeof pending.data)) return false;
    pending.dirty=false;return true;
}
static bool location(void *context,unsigned page,size_t *offset) {
    if(!flash.mapped_flash || !context || page>=2) return false;
    const unsigned *journal=context;
    uintptr_t address=(uintptr_t)journal,first=(uintptr_t)journals,last=(uintptr_t)(journals+OSG_PICO_STORAGE_JOURNALS);
    if(address<first || address>=last || (address-first)%sizeof journals[0] ||
       *journal>=OSG_PICO_STORAGE_JOURNALS) return false;
    *offset=flash.size-OSG_PICO_STORAGE_BYTES+(*journal*2+page)*OSG_PICO_FLASH_SECTOR_SIZE;
    return true;
}
static const uint8_t *read_page(void *context,unsigned page) {
    size_t offset;
    if(!location(context,page,&offset) || !flush()) return NULL;
    return flash.mapped_flash+offset;
}
static bool erase_page(void *context,unsigned page) {
    size_t offset;
    if(!location(context,page,&offset) || !flush() ||
       !flash.erase(offset,OSG_PICO_FLASH_SECTOR_SIZE)) return false;
    for(unsigned i=0;i<OSG_PICO_FLASH_SECTOR_SIZE;i++)
        if(flash.mapped_flash[offset+i]!=UINT8_MAX) return false;
    return true;
}
static bool program_word(void *context,unsigned page,unsigned offset,const uint8_t word[8]) {
    size_t sector;
    if(!word || offset%8 || offset>OSG_JOURNAL_PAGE_SIZE-8 ||
       !location(context,page,&sector)) return false;
    size_t address=sector+offset,block=address&~(size_t)(OSG_PICO_FLASH_PROGRAM_SIZE-1);
    if(!pending.dirty || pending.offset!=block) {
        if(!flush()) return false;
        pending.offset=block;
        memcpy(pending.data,flash.mapped_flash+block,sizeof pending.data);
    }
    unsigned within=(unsigned)(address-block);
    for(unsigned i=0;i<8;i++) if((pending.data[within+i]&word[i])!=word[i]) return false;
    memcpy(pending.data+within,word,8);pending.dirty=true;
    /* The journal writes its commit marker only after read() durably flushes
       the complete body. Make that final write durable before returning. */
    return offset==24 ? flush() : true;
}
bool osg_pico_storage_init(const osg_pico_flash_t *backend) {
    memset(&flash,0,sizeof flash);memset(&pending,0,sizeof pending);
    if(!backend || !backend->mapped_flash || !backend->erase || !backend->program ||
       backend->size<OSG_PICO_STORAGE_BYTES || backend->size%OSG_PICO_FLASH_SECTOR_SIZE ||
       backend->image_size>backend->size-OSG_PICO_STORAGE_BYTES) return false;
    flash=*backend;
    for(unsigned i=0;i<OSG_PICO_STORAGE_JOURNALS;i++) journals[i]=i;
    return true;
}
osg_journal_io_t osg_pico_storage_io(unsigned index) {
    if(!flash.mapped_flash || index>=OSG_PICO_STORAGE_JOURNALS) return (osg_journal_io_t){0};
    return (osg_journal_io_t){.context=&journals[index],.read=read_page,
        .erase=erase_page,.program=program_word};
}
