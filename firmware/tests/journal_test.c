#include "gateway_journal.h"
#include "opensimgear/gateway_registry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t pages[2][OSG_JOURNAL_PAGE_SIZE];
    unsigned operations,fail_at;
    bool corrupt_program;
} flash_t;
static const uint8_t *read_page(void *context,unsigned page) {
    flash_t *flash=context; assert(page<2); return flash->pages[page];
}
static bool erase(void *context,unsigned page) {
    flash_t *flash=context; assert(page<2); flash->operations++;
    if(flash->operations==flash->fail_at) {
        memset(flash->pages[page],0xFF,OSG_JOURNAL_PAGE_SIZE/2); return false;
    }
    memset(flash->pages[page],0xFF,OSG_JOURNAL_PAGE_SIZE); return true;
}
static bool program(void *context,unsigned page,unsigned offset,const uint8_t data[8]) {
    flash_t *flash=context; assert(page<2 && offset%8==0 && offset<=OSG_JOURNAL_PAGE_SIZE-8);
    flash->operations++; bool failed=flash->operations==flash->fail_at;
    for(unsigned byte=0;byte<(failed ? 4U : 8U);byte++) {
        assert(flash->pages[page][offset+byte]==0xFF);
        flash->pages[page][offset+byte]&=data[byte];
    }
    if(flash->corrupt_program && offset==32) flash->pages[page][offset]&=0xFE;
    return !failed;
}
static osg_journal_io_t io(flash_t *flash) {
    return (osg_journal_io_t){.context=flash,.read=read_page,.erase=erase,.program=program};
}
static void expect(const osg_journal_io_t *backend,const uint8_t *data,size_t size,uint32_t expected_epoch) {
    uint8_t actual[OSG_JOURNAL_CAPACITY]; size_t length=9999; uint32_t epoch=0;
    assert(osg_journal_load(backend,actual,sizeof actual,&length,&epoch));
    assert(length==size && epoch==expected_epoch && !memcmp(actual,data,size));
}
static void power_failure(size_t payload_size) {
    flash_t initial; memset(&initial,0,sizeof initial); memset(initial.pages,0xFF,sizeof initial.pages);
    osg_journal_io_t first=io(&initial);
    uint8_t old[OSG_JOURNAL_CAPACITY],updated[OSG_JOURNAL_CAPACITY];
    memset(old,0xA5,sizeof old); memset(updated,0x5B,sizeof updated);
    assert(osg_journal_commit(&first,old,payload_size,0x10000));
    flash_t successful=initial; successful.operations=0;
    osg_journal_io_t backend=io(&successful);
    assert(osg_journal_commit(&backend,updated,payload_size,0x20000));
    expect(&backend,updated,payload_size,0x20000);
    unsigned operation_count=successful.operations;
    for(unsigned interrupted=1;interrupted<=operation_count;interrupted++) {
        flash_t cut=initial; cut.operations=0; cut.fail_at=interrupted;
        backend=io(&cut);
        assert(!osg_journal_commit(&backend,updated,payload_size,0x20000));
        expect(&backend,old,payload_size,0x10000);
        cut.fail_at=0; cut.operations=0;
        assert(osg_journal_commit(&backend,updated,payload_size,0x20000));
        expect(&backend,updated,payload_size,0x20000);
    }
    /* A damaged newest page must fall back to the previous complete page. */
    if(payload_size) {
        successful.pages[1][32]^=1;
        backend=io(&successful); expect(&backend,old,payload_size,0x10000);
    }
    flash_t damaged=initial; damaged.corrupt_program=true; damaged.operations=0;
    backend=io(&damaged);
    if(payload_size) {
        assert(!osg_journal_commit(&backend,updated,payload_size,0x20000));
        expect(&backend,old,payload_size,0x10000);
    }
}
int main(void) {
    flash_t flash; memset(&flash,0,sizeof flash); memset(flash.pages,0xFF,sizeof flash.pages);
    osg_journal_io_t backend=io(&flash);
    uint8_t out[OSG_JOURNAL_CAPACITY]; size_t length=1; uint32_t epoch=1;
    assert(!osg_journal_load(&backend,out,sizeof out,&length,&epoch));
    assert(length==0 && epoch==0);
    assert(osg_journal_erased(&backend));
    flash.pages[0][OSG_JOURNAL_PAGE_SIZE-1]=0;
    assert(!osg_journal_erased(&backend));
    assert(!osg_journal_load(&backend,out,sizeof out,&length,&epoch));
    flash.pages[0][OSG_JOURNAL_PAGE_SIZE-1]=0xFF;
    assert(!osg_journal_commit(&backend,out,sizeof out+1,0x10000));
    assert(!osg_journal_commit(&backend,out,1,0));
    assert(flash.operations==0);
    power_failure(0); power_failure(1); power_failure(500); power_failure(OSG_GW_REGISTRY_RECORD_SIZE); power_failure(OSG_JOURNAL_CAPACITY);
    puts("gateway flash journal power failure tests passed"); return 0;
}
