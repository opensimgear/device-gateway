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
    m->has_input=false; m->delivery_generation++; memset(m->report,0,sizeof m->report);
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
    if(g->running!=running || !running) {
        g->run_generation++;
        for(unsigned i=0;i<10;i++) neutral(&g->modules[i]);
    }
    g->running=running;
}
void osg_gateway_bus_reset(osg_gateway_t *g) {
    if(!g) return;
    g->running=false; g->run_generation++;
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
osg_gateway_receive_result_t osg_gateway_receive(osg_gateway_t *g,const osg_gateway_message_t *message,
                                                uint32_t now,osg_gateway_message_t *reply) {
    if(!g) return OSG_GW_RX_REJECTED;
    if(!message || message->source_node>127 || !message->boot_session ||
       (message->type!=OSG_GW_ANNOUNCE && message->type!=OSG_GW_INPUT && message->type!=OSG_GW_HEARTBEAT)) goto rejected;
    osg_gateway_message_t msg=*message;
    if(msg.type==OSG_GW_ANNOUNCE) {
        if(msg.node || !osg_gateway_identity_valid(&msg.identity) || !osg_gateway_profile_valid(&msg.profile)) goto rejected;
    } else if(!msg.node || msg.node>OSG_GW_MAX_MODULES || !msg.lease ||
              (msg.type==OSG_GW_HEARTBEAT && !osg_gateway_identity_valid(&msg.identity)) ||
              (msg.type==OSG_GW_INPUT && msg.direct_usb)) goto rejected;
    osg_gateway_tick(g,now);
    int index=-1;
    if(msg.type==OSG_GW_INPUT) {
        for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) {
            const osg_gateway_module_t *m=&g->modules[i];
            if(m->occupied && m->online && m->node==msg.node && m->source_node==msg.source_node &&
               m->boot_session==msg.boot_session && m->lease==msg.lease) {
                if(index>=0) goto rejected; /* A compact route must resolve unambiguously. */
                index=(int)i;
            }
        }
    } else index=osg_gateway_find_module(g,&msg.identity);
    if(msg.type==OSG_GW_ANNOUNCE) {
        if(!reply) goto rejected;
        /* Do not let two identities share a transport address while a CAN
         * node-ID collision is being resolved. Invalidate the previous route. */
        for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) {
            osg_gateway_module_t *other=&g->modules[i];
            if(other->occupied && other->online && other->source_node==msg.source_node &&
               !osg_gateway_identity_equal(&other->identity,&msg.identity)) {
                revoke(other); goto rejected;
            }
        }
        if(index<0) index=available_inventory(g,now);
        if(index<0) { g->rejected_frames++; return OSG_GW_RX_FULL; }
        osg_gateway_module_t *m=&g->modules[index];
        bool same=m->occupied && osg_gateway_identity_equal(&m->identity,&msg.identity);
        bool renew=!same || !m->online || m->source_node!=msg.source_node || m->boot_session!=msg.boot_session ||
                   m->configuration_generation!=msg.configuration_generation || !osg_gateway_profile_equal(&m->profile,&msg.profile);
        if(renew) {
            uint32_t lease=allocate_lease(g);
            if(!lease) { g->rejected_frames++; return OSG_GW_RX_FULL; }
            if(!same) memset(m,0,sizeof *m);
            else revoke(m);
            uint8_t node=available_node(g);
            if(!node) { g->rejected_frames++; return OSG_GW_RX_FULL; }
            m->occupied=true; m->identity=msg.identity; m->profile=msg.profile;
            m->boot_session=msg.boot_session; m->node=node; m->source_node=msg.source_node; m->lease=lease;
            m->configuration_generation=msg.configuration_generation;
        }
        m->online=true; m->last_seen=now; m->direct_usb=msg.direct_usb;
        /* Refresh an already registered draft slot after module configuration.
         * The active descriptor stays immutable until persistent gateway Apply. */
        int registered=osg_gateway_find_slot(&g->pending,&m->identity);
        if(registered>=0 && !osg_gateway_profile_equal(&g->pending.slots[registered].profile,&m->profile)) {
            g->pending.slots[registered].profile=m->profile; g->pending.generation++;
        }
        if(m->direct_usb) neutral(m);
        osg_gateway_message_t assignment={.type=OSG_GW_ASSIGN,.node=m->node,
            .identity=m->identity,.boot_session=m->boot_session,.lease=m->lease,.gateway_epoch=g->epoch};
        *reply=assignment;
        g->accepted_frames++; return OSG_GW_RX_ASSIGNMENT;
    }
    if(index<0) goto rejected;
    osg_gateway_module_t *m=&g->modules[index];
    if(!m->online || m->node!=msg.node || m->source_node!=msg.source_node ||
       m->boot_session!=msg.boot_session || m->lease!=msg.lease ||
       (m->has_sequence && !newer(msg.sequence,m->sequence)) ||
       (msg.type==OSG_GW_INPUT && !osg_gateway_profile_report_valid(&m->profile,msg.report))) goto rejected;
    m->sequence=msg.sequence; m->has_sequence=true; m->last_seen=now;
    if(msg.type==OSG_GW_HEARTBEAT) {
        m->direct_usb=msg.direct_usb;
        if(m->direct_usb) neutral(m);
    } else {
        /* INPUT cannot overrule direct-USB ownership. A heartbeat/announce releases it. */
        if(g->running && !m->direct_usb) {
            memcpy(m->report,msg.report,sizeof m->report); m->has_input=true;
            m->last_input=now; m->input_sequence=msg.sequence;
        }
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
    uint8_t neutral_report[OSG_GW_MAX_REPORT_SIZE];
    osg_gateway_profile_neutral(&s->profile,neutral_report); memcpy(out,neutral_report,length);
    int module=osg_gateway_find_module(g,&s->identity);
    if(module>=0) {
        const osg_gateway_module_t *m=&g->modules[module];
        if(g->running && m->online && !m->direct_usb && m->has_input &&
           osg_gateway_profile_equal(&m->profile,&s->profile)) memcpy(out,m->report,length);
    }
    return length;
}
size_t osg_gateway_delivery_prepare(const osg_gateway_t *g,unsigned slot,uint8_t *out,size_t capacity,osg_gateway_delivery_t *delivery) {
    if(!delivery) return 0;
    memset(delivery,0,sizeof *delivery);
    size_t length=osg_gateway_report(g,slot,out,capacity);
    if(!length) return 0;
    const osg_gateway_slot_t *s=&g->active.slots[slot];
    int index=osg_gateway_find_module(g,&s->identity);
    if(index<0) return length;
    const osg_gateway_module_t *m=&g->modules[index];
    if(!g->running || !m->online || m->direct_usb || !m->has_input || !osg_gateway_profile_equal(&m->profile,&s->profile)) return length;
    *delivery=(osg_gateway_delivery_t){.valid=true,.slot=(uint8_t)slot,.node=m->node,.length=(uint8_t)length,
        .identity=m->identity,.boot_session=m->boot_session,.lease=m->lease,.sequence=m->input_sequence,
        .run_generation=g->run_generation,.delivery_generation=m->delivery_generation};
    memcpy(delivery->report,out,length); return length;
}
bool osg_gateway_delivery_complete(const osg_gateway_t *g,const osg_gateway_delivery_t *d,
                                   const uint8_t *delivered,size_t length,osg_gateway_message_t *ack) {
    if(!g || !d || !d->valid || !delivered || !ack || !g->running || d->run_generation!=g->run_generation ||
       d->slot>=OSG_GW_MAX_MODULES || length!=(size_t)d->length+1 || delivered[0]!=d->slot+1 ||
       memcmp(delivered+1,d->report,d->length)) return false;
    const osg_gateway_slot_t *s=&g->active.slots[d->slot];
    if(!s->occupied || !osg_gateway_identity_equal(&s->identity,&d->identity)) return false;
    int index=osg_gateway_find_module(g,&d->identity);
    if(index<0) return false;
    const osg_gateway_module_t *m=&g->modules[index];
    if(!m->online || m->direct_usb || !m->has_input || m->node!=d->node || m->boot_session!=d->boot_session ||
       m->lease!=d->lease || m->delivery_generation!=d->delivery_generation ||
       !osg_gateway_profile_equal(&m->profile,&s->profile)) return false;
    osg_gateway_message_t message={.type=OSG_GW_INPUT_ACK,.node=d->node,
        .boot_session=d->boot_session,.lease=d->lease,.sequence=d->sequence};
    memcpy(message.report,d->report,sizeof message.report);
    *ack=message; return true;
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
        if(p->hat_count) {
            EMIT(0x05,1,0x09,0x39,0x15,0,0x25,7,0x35,0,0x46,0x3b,1,0x65,0x14,
                 0x75,4,0x95,p->hat_count,0x81,0x42);
            if(p->hat_count%2) EMIT(0x75,4,0x95,1,0x81,3);
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
