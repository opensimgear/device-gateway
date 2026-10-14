#include "pico_storage.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t memory[2*1024*1024];
static unsigned programs,erases;
static int fail_after=-1;
static bool permitted(void) {
    if(fail_after<0) return true;
    if(!fail_after) return false;
    fail_after--;return true;
}
static bool erase(size_t offset,size_t size) {
    assert(offset%4096==0 && size==4096 && offset+size<=sizeof memory);
    erases++;
    if(!permitted()) { memset(memory+offset,255,size/2);return false; }
    memset(memory+offset,255,size);return true;
}
static bool program(size_t offset,const uint8_t *data,size_t size) {
    assert(offset%256==0 && size==256 && offset+size<=sizeof memory);
    programs++;
    bool ok=permitted();size_t count=ok ? size : size/2;
    for(size_t i=0;i<count;i++) {
        assert((memory[offset+i]&data[i])==data[i]);memory[offset+i]&=data[i];
    }
    return ok;
}
static osg_pico_flash_t backend(void) {
    return (osg_pico_flash_t){.mapped_flash=memory,.size=sizeof memory,.image_size=120000,
        .erase=erase,.program=program};
}
static void reset(void) {
    memset(memory,255,sizeof memory);programs=erases=0;fail_after=-1;
    osg_pico_flash_t flash=backend();assert(osg_pico_storage_init(&flash));
}
static void readback(const osg_journal_io_t *io,const uint8_t *expected,size_t length,uint32_t epoch) {
    uint8_t actual[OSG_JOURNAL_CAPACITY];size_t n;uint32_t restored;
    assert(osg_journal_load(io,actual,sizeof actual,&n,&restored));
    assert(n==length && restored==epoch && !memcmp(actual,expected,n));
}
int main(void) {
    assert(!osg_pico_storage_init(NULL));assert(!osg_pico_storage_io(0).read);
    osg_pico_flash_t bad=backend();bad.image_size=bad.size-OSG_PICO_STORAGE_BYTES+1;
    assert(!osg_pico_storage_init(&bad));bad=backend();bad.size--;
    assert(!osg_pico_storage_init(&bad));bad=backend();bad.erase=NULL;
    assert(!osg_pico_storage_init(&bad));
    reset();assert(!osg_pico_storage_io(OSG_PICO_STORAGE_JOURNALS).read);
    osg_journal_io_t io=osg_pico_storage_io(0);
    assert(osg_journal_erased(&io));assert(!io.read(io.context,2));assert(!io.erase(NULL,0));
    uint8_t word[8]={0};assert(!io.program(io.context,0,1,word));
    assert(!io.program(io.context,0,OSG_JOURNAL_PAGE_SIZE,word));
    assert(!io.program(io.context,0,UINT32_MAX-7U,word));
    assert(!io.program(io.context,0,0,NULL));
    assert(io.program(io.context,0,0,word));memset(word,255,sizeof word);
    assert(!io.program(io.context,0,0,word));assert(io.read(io.context,0));
    reset();io=osg_pico_storage_io(0);
    uint8_t first[OSG_JOURNAL_CAPACITY],second[OSG_JOURNAL_CAPACITY];
    for(unsigned i=0;i<sizeof first;i++) { first[i]=(uint8_t)i;second[i]=(uint8_t)(255-i); }
    assert(osg_journal_commit(&io,first,sizeof first,1));readback(&io,first,sizeof first,1);
    assert(erases==1 && programs==9); /* Eight data pages and a separate final marker. */
    assert(osg_journal_commit(&io,second,sizeof second,2));readback(&io,second,sizeof second,2);
    for(unsigned index=1;index<OSG_PICO_STORAGE_JOURNALS;index++) {
        osg_journal_io_t other=osg_pico_storage_io(index);
        assert(osg_journal_erased(&other));
        assert(osg_journal_commit(&other,first,17,index+1));readback(&other,first,17,index+1);
        readback(&io,second,sizeof second,2);
    }
    /* Every interruption before the commit marker restores the previous record.
       Reinitialization models losing the RAM page cache during power loss. */
    uint8_t saved[sizeof memory];memcpy(saved,memory,sizeof memory);
    for(int failure=0;failure<10;failure++) {
        memcpy(memory,saved,sizeof memory);osg_pico_flash_t flash=backend();
        assert(osg_pico_storage_init(&flash));io=osg_pico_storage_io(0);fail_after=failure;
        assert(!osg_journal_commit(&io,first,sizeof first,3));fail_after=-1;
        assert(osg_pico_storage_init(&flash));io=osg_pico_storage_io(0);
        if(failure<9) readback(&io,second,sizeof second,2);
        else readback(&io,first,sizeof first,3); /* Marker landed before the reported failure. */
    }
    puts("Pico sector storage, staged page writes, journal isolation and interruption recovery passed.");
}
