#include "opensimgear/gateway_registry.h"
#include <string.h>
static uint16_t read16(const uint8_t *p) { return (uint16_t)(p[0]|((uint16_t)p[1]<<8)); }
static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void write16(uint8_t *p,uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void write32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static bool zero(const uint8_t *p,size_t n) { for(size_t i=0;i<n;i++) if(p[i]) return false; return true; }
bool osg_gateway_registry_valid(const osg_gateway_registry_t *r) {
    if(!r) return false;
    unsigned enabled=0;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(r->slots[i].occupied) enabled+=r->slots[i].enabled;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(r->slots[i].occupied) {
        const osg_gateway_slot_t *s=&r->slots[i];
        bool inputs=s->profile.button_count || s->profile.axis_count || s->profile.hat_count;
        if(!osg_gateway_identity_valid(&s->identity) || !osg_gateway_profile_valid(&s->profile) ||
           (inputs ? !s->report_id : (s->report_id || s->enabled)) || (s->enabled ? s->order>=enabled : s->order!=255)) return false;
        for(unsigned j=0;j<i;j++) if(r->slots[j].occupied &&
            (osg_gateway_identity_equal(&s->identity,&r->slots[j].identity) || (s->report_id && s->report_id==r->slots[j].report_id) ||
             (s->enabled && r->slots[j].enabled && s->order==r->slots[j].order))) return false;
    }
    return true;
}
size_t osg_gateway_registry_encode(const osg_gateway_registry_t *r,uint8_t *out,size_t capacity) {
    if(!out || capacity<OSG_GW_REGISTRY_RECORD_SIZE || !osg_gateway_registry_valid(r)) return 0;
    uint8_t record[OSG_GW_REGISTRY_RECORD_SIZE]={0};
    memcpy(record,"OGR1",4); write16(record+4,OSG_GW_REGISTRY_VERSION); record[6]=OSG_GW_MAX_MODULES;
    write32(record+8,r->generation); write32(record+12,OSG_GW_REGISTRY_RECORD_SIZE);
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(r->slots[i].occupied) {
        const osg_gateway_slot_t *s=&r->slots[i]; uint8_t *p=record+16+48*i;
        p[0]=(uint8_t)(1U|(s->enabled?2U:0U)); p[1]=s->profile.button_count; p[2]=s->profile.axis_count; p[3]=s->profile.hat_count;
        memcpy(p+4,s->identity.bytes,16); write16(p+20,s->profile.revision);
        memcpy(p+22,s->profile.axis_usage,8);
        for(unsigned a=0;a<8;a++) write16(p+30+2*a,s->profile.axis_neutral[a]);
        p[46]=s->report_id; p[47]=s->order;
    }
    write32(record+sizeof record-4,osg_gateway_crc32(record,sizeof record-4)); memcpy(out,record,sizeof record);
    return sizeof record;
}
bool osg_gateway_registry_decode(const uint8_t *record,size_t length,osg_gateway_registry_t *out) {
    if(!record || !out || length<20 || memcmp(record,"OGR1",4) || record[7] ||
       read32(record+12)!=length || read32(record+length-4)!=osg_gateway_crc32(record,length-4)) return false;
    uint16_t version=read16(record+4); bool legacy=version==1 || version==2;
    if(legacy ? (length!=OSG_GW_LEGACY_REGISTRY_RECORD_SIZE || record[6]!=10) :
       (version!=OSG_GW_REGISTRY_VERSION || length!=OSG_GW_REGISTRY_RECORD_SIZE || record[6]!=OSG_GW_MAX_MODULES)) return false;
    osg_gateway_registry_t r={.generation=read32(record+8)}; unsigned migrated=0;
    for(unsigned i=0;i<record[6];i++) {
        const uint8_t *p=record+16+48*i;
        if(!p[0]) { if(!zero(p,48)) return false; continue; }
        if((legacy && (p[0]!=1 || p[46] || p[47])) || (!legacy && p[0]!=1 && p[0]!=3) ||
           (version==1 && (p[3] || p[1]>64))) return false;
        osg_gateway_slot_t *s=&r.slots[legacy?migrated:i];
        s->occupied=true; s->enabled=legacy || (p[0]&2)!=0;
        s->report_id=legacy?(uint8_t)(i+1):p[46]; s->order=legacy?(uint8_t)migrated:p[47];
        s->profile.button_count=p[1]; s->profile.axis_count=p[2]; s->profile.hat_count=p[3];
        memcpy(s->identity.bytes,p+4,16); s->profile.revision=read16(p+20);
        memcpy(s->profile.axis_usage,p+22,8);
        for(unsigned a=0;a<8;a++) s->profile.axis_neutral[a]=read16(p+30+2*a);
        migrated++;
    }
    if(!osg_gateway_registry_valid(&r)) return false;
    *out=r; return true;
}
int osg_gateway_registry_select(const uint8_t *a,size_t an,const uint8_t *b,size_t bn,
                                osg_gateway_registry_t *out) {
    if(!out) return -1;
    osg_gateway_registry_t ra,rb;
    bool va=osg_gateway_registry_decode(a,an,&ra),vb=osg_gateway_registry_decode(b,bn,&rb);
    if(!va && !vb) return -1;
    uint32_t distance=vb && va ? rb.generation-ra.generation : 0;
    if(vb && (!va || (distance && distance<UINT32_C(0x80000000)))) { *out=rb; return 1; }
    *out=ra; return 0;
}
