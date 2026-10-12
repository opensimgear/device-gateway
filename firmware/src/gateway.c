#include "opensimgear/gateway.h"
#include <string.h>

int osg_gateway_find_module(const osg_gateway_t *g,const osg_gateway_identity_t *id) {
    if(!g || !id) return -1;
    for(unsigned i=0;i<10;i++) if(g->modules[i].occupied && osg_gateway_identity_equal(&g->modules[i].identity,id)) return (int)i;
    return -1;
}
int osg_gateway_find_slot(const osg_gateway_registry_t *r,const osg_gateway_identity_t *id) {
    if(!r || !id) return -1;
    for(unsigned i=0;i<10;i++) if(r->slots[i].occupied && osg_gateway_identity_equal(&r->slots[i].identity,id)) return (int)i;
    return -1;
}
static void neutral(osg_gateway_module_t *m) {
    m->has_input=false; memset(m->report,0,sizeof m->report);
}
static void revoke(osg_gateway_module_t *m) {
    neutral(m); m->online=false; m->node=0; m->lease=0; m->has_sequence=false;
}
bool osg_gateway_init(osg_gateway_t *g,uint32_t epoch,const osg_gateway_registry_t *r) {
    if(!g || !epoch || (r && !osg_gateway_registry_valid(r))) return false;
    memset(g,0,sizeof *g); g->epoch=epoch;
    if(r) g->pending=g->active=*r;
    for(unsigned i=0;i<10;i++) if(g->active.slots[i].occupied) {
        g->modules[i].occupied=true; g->modules[i].identity=g->active.slots[i].identity;
        g->modules[i].profile=g->active.slots[i].profile;
    }
    return true;
}
void osg_gateway_tick(osg_gateway_t *g,uint32_t now) {
    if(!g) return;
    for(unsigned i=0;i<10;i++) {
        osg_gateway_module_t *m=&g->modules[i];
        if(!m->occupied || !m->online) continue;
        if((uint32_t)(now-m->last_seen)>=OSG_GW_OFFLINE_TIMEOUT_MS) revoke(m);
        else if(m->has_input && (uint32_t)(now-m->last_input)>=OSG_GW_INPUT_TIMEOUT_MS) neutral(m);
    }
}
void osg_gateway_set_running(osg_gateway_t *g,bool running) {
    if(!g) return;
    /* Every transition requires an input newer than the last accepted sequence. */
    if(g->running!=running || !running) for(unsigned i=0;i<10;i++) neutral(&g->modules[i]);
    g->running=running;
}
void osg_gateway_bus_reset(osg_gateway_t *g) {
    if(!g) return;
    g->running=false;
    for(unsigned i=0;i<10;i++) revoke(&g->modules[i]);
}
static int available_inventory(const osg_gateway_t *g,uint32_t now) {
    int candidate=-1; uint32_t oldest=0;
    for(unsigned i=0;i<10;i++) {
        const osg_gateway_module_t *m=&g->modules[i];
        if(!m->occupied) return (int)i;
        if(!m->online && osg_gateway_find_slot(&g->pending,&m->identity)<0 &&
           osg_gateway_find_slot(&g->active,&m->identity)<0) {
            uint32_t age=now-m->last_seen;
            if(candidate<0 || age>oldest) { candidate=(int)i; oldest=age; }
        }
    }
    return candidate;
}
static uint8_t available_node(const osg_gateway_t *g) {
    for(uint8_t node=1;node<=10;node++) {
        bool used=false;
        for(unsigned i=0;i<10;i++) if(g->modules[i].online && g->modules[i].node==node) used=true;
        if(!used) return node;
    }
    return 0;
}
static uint32_t allocate_lease(osg_gateway_t *g) {
    /* Epoch/counter guard against old frames; this is not authentication. */
    if(g->lease_counter>=65535) return 0;
    g->lease_counter++;
    return g->epoch+g->lease_counter;
}
static bool newer(uint32_t value,uint32_t previous) {
    uint32_t delta=value-previous;
    return delta!=0 && delta<UINT32_C(0x80000000);
}
static bool input_valid(const osg_gateway_message_t *msg,const osg_gateway_profile_t *profile) {
    size_t length=osg_gateway_profile_report_size(profile);
    if(!length) return false;
    if(profile->button_count%8) {
        uint8_t forbidden=(uint8_t)(0xffU<<(profile->button_count%8));
        if(msg->report[profile->button_count/8]&forbidden) return false;
    }
    for(size_t i=length;i<24;i++) if(msg->report[i]) return false;
    return true;
}
osg_gateway_receive_result_t osg_gateway_receive(osg_gateway_t *g,const osg_gateway_frame_t *frame,
                                                uint32_t now,osg_gateway_frame_t *reply) {
    if(!g) return OSG_GW_RX_REJECTED;
    osg_gateway_message_t msg;
    if(!osg_gateway_frame_decode(frame,&msg) || msg.type==OSG_GW_ASSIGN) goto rejected;
    osg_gateway_tick(g,now);
    int index=osg_gateway_find_module(g,&msg.identity);
    if(msg.type==OSG_GW_ANNOUNCE) {
        if(!reply) goto rejected;
        if(index<0) index=available_inventory(g,now);
        if(index<0) { g->rejected_frames++; return OSG_GW_RX_FULL; }
        osg_gateway_module_t *m=&g->modules[index];
        bool same=m->occupied && osg_gateway_identity_equal(&m->identity,&msg.identity);
        bool renew=!same || !m->online || m->boot_session!=msg.boot_session ||
                   !osg_gateway_profile_equal(&m->profile,&msg.profile);
        if(renew) {
            uint32_t lease=allocate_lease(g);
            if(!lease) { g->rejected_frames++; return OSG_GW_RX_FULL; }
            if(!same) memset(m,0,sizeof *m);
            else revoke(m);
            uint8_t node=available_node(g);
            if(!node) { g->rejected_frames++; return OSG_GW_RX_FULL; }
            m->occupied=true; m->identity=msg.identity; m->profile=msg.profile;
            m->boot_session=msg.boot_session; m->node=node; m->lease=lease;
        }
        m->online=true; m->last_seen=now; m->direct_usb=msg.direct_usb;
        if(m->direct_usb) neutral(m);
        osg_gateway_message_t assignment={.type=OSG_GW_ASSIGN,.node=m->node,
            .identity=m->identity,.boot_session=m->boot_session,.lease=m->lease,.gateway_epoch=g->epoch};
        if(!osg_gateway_frame_encode(&assignment,reply)) goto rejected;
        g->accepted_frames++; return OSG_GW_RX_ASSIGNMENT;
    }
    if(index<0) goto rejected;
    osg_gateway_module_t *m=&g->modules[index];
    if(!m->online || m->node!=msg.node || m->boot_session!=msg.boot_session || m->lease!=msg.lease ||
       (m->has_sequence && !newer(msg.sequence,m->sequence)) ||
       (msg.type==OSG_GW_INPUT && !input_valid(&msg,&m->profile))) goto rejected;
    m->sequence=msg.sequence; m->has_sequence=true; m->last_seen=now;
    if(msg.type==OSG_GW_HEARTBEAT) {
        m->direct_usb=msg.direct_usb;
        if(m->direct_usb) neutral(m);
    } else {
        /* INPUT cannot overrule direct-USB ownership. A heartbeat/announce releases it. */
        if(g->running && !m->direct_usb) { memcpy(m->report,msg.report,sizeof m->report); m->has_input=true; m->last_input=now; }
    }
    g->accepted_frames++; return OSG_GW_RX_ACCEPTED;
rejected:
    g->rejected_frames++; return OSG_GW_RX_REJECTED;
}
bool osg_gateway_register(osg_gateway_t *g,const osg_gateway_identity_t *id) {
    if(!g || !id) return false;
    int module=osg_gateway_find_module(g,id);
    if(module<0 || !g->modules[module].online) return false;
    int slot=osg_gateway_find_slot(&g->pending,id);
    if(slot>=0 && osg_gateway_profile_equal(&g->pending.slots[slot].profile,&g->modules[module].profile)) return true;
    if(slot<0) for(unsigned i=0;i<10;i++) if(!g->pending.slots[i].occupied) { slot=(int)i; break; }
    if(slot<0) return false;
    g->pending.slots[slot]=(osg_gateway_slot_t){.occupied=true,.identity=*id,.profile=g->modules[module].profile};
    g->pending.generation++; return true;
}
bool osg_gateway_unregister(osg_gateway_t *g,const osg_gateway_identity_t *id) {
    if(!g || !id) return false;
    int slot=osg_gateway_find_slot(&g->pending,id);
    if(slot<0) return false;
    memset(&g->pending.slots[slot],0,sizeof g->pending.slots[slot]); g->pending.generation++; return true;
}
bool osg_gateway_needs_apply(const osg_gateway_t *g) {
    if(!g) return false;
    for(unsigned i=0;i<10;i++) {
        const osg_gateway_slot_t *a=&g->active.slots[i],*b=&g->pending.slots[i];
        if(a->occupied!=b->occupied || (a->occupied &&
           (!osg_gateway_identity_equal(&a->identity,&b->identity) || !osg_gateway_profile_equal(&a->profile,&b->profile)))) return true;
    }
    return false;
}
void osg_gateway_apply_layout(osg_gateway_t *g) {
    if(!g) return;
    osg_gateway_set_running(g,false); g->active=g->pending;
}
size_t osg_gateway_report(const osg_gateway_t *g,unsigned slot,uint8_t *out,size_t capacity) {
    if(!g || !out || slot>=10 || !g->active.slots[slot].occupied) return 0;
    const osg_gateway_slot_t *s=&g->active.slots[slot];
    size_t length=osg_gateway_profile_report_size(&s->profile);
    if(!length || capacity<length) return 0;
    memset(out,0,length);
    unsigned offset=(s->profile.button_count+7)/8;
    for(unsigned a=0;a<s->profile.axis_count;a++) {
        uint16_t value=s->profile.axis_neutral[a];
        out[offset+2*a]=(uint8_t)value; out[offset+2*a+1]=(uint8_t)(value>>8);
    }
    int module=osg_gateway_find_module(g,&s->identity);
    if(module>=0) {
        const osg_gateway_module_t *m=&g->modules[module];
        if(g->running && m->online && !m->direct_usb && m->has_input &&
           osg_gateway_profile_equal(&m->profile,&s->profile)) memcpy(out,m->report,length);
    }
    return length;
}
size_t osg_gateway_descriptor(const osg_gateway_t *g,uint8_t *out,size_t capacity) {
    if(!g || !out || !osg_gateway_registry_valid(&g->active)) return 0;
    uint8_t descriptor[OSG_GW_DESCRIPTOR_CAPACITY]; size_t n=0; bool any=false;
#define EMIT(...) do { const uint8_t items[]={__VA_ARGS__}; \
    if(n+sizeof items>sizeof descriptor) return 0; \
    memcpy(descriptor+n,items,sizeof items); n+=sizeof items; } while(0)
    for(unsigned i=0;i<10;i++) if(g->active.slots[i].occupied) {
        any=true; const osg_gateway_profile_t *p=&g->active.slots[i].profile;
        EMIT(0x05,1,0x09,4,0xa1,1,0x85,(uint8_t)(i+1));
        if(p->button_count) {
            EMIT(0x05,9,0x19,1,0x29,p->button_count,0x15,0,0x25,1,0x75,1,0x95,p->button_count,0x81,2);
            if(p->button_count%8) EMIT(0x75,1,0x95,(uint8_t)(8-p->button_count%8),0x81,3);
        }
        if(p->axis_count) {
            EMIT(0x05,1,0x15,0,0x27,0xff,0xff,0,0,0x35,0,0x45,0,0x65,0,0x75,16,0x95,1);
            for(unsigned a=0;a<p->axis_count;a++) EMIT(0x09,p->axis_usage[a],0x81,2);
        }
        EMIT(0xc0);
    }
    /* Empty registry exposes only a vendor management collection, no joystick. */
    if(!any) EMIT(0x06,0,0xff,0x09,1,0xa1,1,0x85,11,0x15,0,0x26,0xff,0,0x75,8,0x95,1,0x81,2,0xc0);
#undef EMIT
    if(capacity<n) return 0;
    memcpy(out,descriptor,n); return n;
}
