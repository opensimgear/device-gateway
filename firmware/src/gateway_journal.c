#include "gateway_journal.h"
#include "opensimgear/gateway_protocol.h"
#include <string.h>

#define JOURNAL_MAGIC UINT32_C(0x314a474f)
#define JOURNAL_COMMIT UINT64_C(0x54494d4d4f43474f)
#define HEADER_SIZE 32U
typedef struct {
    uint32_t magic,sequence,length,epoch,crc,reserved;
    uint64_t commit;
} journal_header_t;
_Static_assert(sizeof(journal_header_t)==HEADER_SIZE,"Journal header layout");
_Static_assert(HEADER_SIZE+OSG_JOURNAL_CAPACITY<=OSG_JOURNAL_PAGE_SIZE,"Journal fits page");
static uint32_t checksum(const uint8_t *record,const journal_header_t *header) {
    uint8_t scratch[16+OSG_JOURNAL_CAPACITY];
    memcpy(scratch,record,16); memcpy(scratch+16,record+HEADER_SIZE,header->length);
    return osg_gateway_crc32(scratch,16+header->length);
}
static bool valid(const uint8_t *record,journal_header_t *header) {
    if(!record) return false;
    memcpy(header,record,sizeof *header);
    return header->magic==JOURNAL_MAGIC && header->sequence!=0 && header->epoch!=0 &&
        header->length<=OSG_JOURNAL_CAPACITY && header->reserved==0 &&
        header->commit==JOURNAL_COMMIT && header->crc==checksum(record,header);
}
static int newest(const osg_journal_io_t *io,journal_header_t *selected) {
    journal_header_t a,b;
    bool first=valid(io->read(io->context,0),&a),second=valid(io->read(io->context,1),&b);
    if(!first && !second) return -1;
    int index=!first || (second && (int32_t)(b.sequence-a.sequence)>0) ? 1 : 0;
    *selected=index ? b : a; return index;
}
bool osg_journal_load(const osg_journal_io_t *io,uint8_t *payload,size_t capacity,size_t *length,uint32_t *epoch) {
    if(!io || !io->read || !payload || !length || !epoch) return false;
    journal_header_t header; int index=newest(io,&header);
    if(index<0) { *length=0; *epoch=0; return false; }
    if(header.length>capacity) return false;
    memcpy(payload,io->read(io->context,(unsigned)index)+HEADER_SIZE,header.length);
    *length=header.length; *epoch=header.epoch; return true;
}
bool osg_journal_erased(const osg_journal_io_t *io) {
    if(!io || !io->read) return false;
    for(unsigned index=0;index<2;index++) {
        const uint8_t *record=io->read(io->context,index); if(!record) return false;
        for(unsigned byte=0;byte<OSG_JOURNAL_PAGE_SIZE;byte++) if(record[byte]!=0xFF) return false;
    }
    return true;
}
bool osg_journal_commit(const osg_journal_io_t *io,const uint8_t *payload,size_t length,uint32_t epoch) {
    if(!io || !io->read || !io->erase || !io->program || (!payload && length) ||
       length>OSG_JOURNAL_CAPACITY || !epoch) return false;
    journal_header_t old,header;
    int previous=newest(io,&old); unsigned destination=previous==0 ? 1U : 0U;
    uint32_t sequence=previous<0 ? 1U : old.sequence+1U;
    if(!sequence) sequence=1;
    uint8_t record[OSG_JOURNAL_PAGE_SIZE]; memset(record,0xFF,sizeof record);
    header=(journal_header_t){.magic=JOURNAL_MAGIC,.sequence=sequence,.length=(uint32_t)length,
        .epoch=epoch,.reserved=0,.commit=UINT64_MAX};
    memcpy(record,&header,sizeof header); if(length) memcpy(record+HEADER_SIZE,payload,length);
    header.crc=checksum(record,&header); memcpy(record,&header,sizeof header);
    if(!io->erase(io->context,destination)) return false;
    size_t programmed=(HEADER_SIZE+length+7U)&~(size_t)7U;
    for(size_t offset=0;offset<programmed;offset+=8) {
        if(offset!=24 && !io->program(io->context,destination,(unsigned)offset,record+offset)) return false;
    }
    const uint8_t *readback=io->read(io->context,destination);
    if(!readback || memcmp(readback,record,programmed)) return false;
    uint64_t commit=JOURNAL_COMMIT; uint8_t marker[8]; memcpy(marker,&commit,sizeof marker);
    if(!io->program(io->context,destination,24,marker)) return false;
    return valid(io->read(io->context,destination),&header);
}
