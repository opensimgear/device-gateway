#include "opensimgear/gateway.h"
#include <string.h>

int osg_gateway_find_module(const osg_gateway_t *g,const osg_gateway_identity_t *id) {
    if(!g || !id) return -1;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(g->modules[i].occupied && osg_gateway_identity_equal(&g->modules[i].identity,id)) return (int)i;
    return -1;
}
int osg_gateway_find_slot(const osg_gateway_registry_t *r,const osg_gateway_identity_t *id) {
    if(!r || !id) return -1;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(r->slots[i].occupied && osg_gateway_identity_equal(&r->slots[i].identity,id)) return (int)i;
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
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(g->active.slots[i].occupied) {
        g->modules[i].occupied=true; g->modules[i].identity=g->active.slots[i].identity;
        g->modules[i].profile=g->active.slots[i].profile;
    }
    return true;
}
void osg_gateway_tick(osg_gateway_t *g,uint32_t now) {
    if(!g) return;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) {
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
        for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) neutral(&g->modules[i]);
    }
    g->running=running;
}
void osg_gateway_bus_reset(osg_gateway_t *g) {
    if(!g) return;
    g->running=false; g->run_generation++;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) revoke(&g->modules[i]);
}
static int available_inventory(const osg_gateway_t *g,uint32_t now) {
    int candidate=-1; uint32_t oldest=0;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) {
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
    for(uint8_t node=1;node<=OSG_GW_MAX_MODULES;node++) {
        bool used=false;
        for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(g->modules[i].online && g->modules[i].node==node) used=true;
        if(!used) return node;
    }
    return 0;
}
static uint32_t allocate_lease(osg_gateway_t *g) {
    /* Epoch/counter guard against old frames; this is not authentication. */
    if(g->lease_counter+g->reserved_leases>=65535) return 0;
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
        if(index<0) {
            g->rejected_frames++; g->rejected_announcements++; g->rejected_identity=msg.identity;
            g->admission_error="capacity_exceeded"; return OSG_GW_RX_FULL;
        }
        osg_gateway_module_t *m=&g->modules[index];
        bool same=m->occupied && osg_gateway_identity_equal(&m->identity,&msg.identity);
        bool renew=!same || !m->online || m->source_node!=msg.source_node || m->boot_session!=msg.boot_session ||
                   m->configuration_generation!=msg.configuration_generation || !osg_gateway_profile_equal(&m->profile,&msg.profile);
        if(renew) {
            uint32_t lease=allocate_lease(g);
            if(!lease) {
                g->rejected_frames++; g->rejected_announcements++; g->rejected_identity=msg.identity;
                g->admission_error="lease_exhausted"; return OSG_GW_RX_FULL;
            }
            if(!same) memset(m,0,sizeof *m);
            else revoke(m);
            uint8_t node=available_node(g);
            if(!node) { g->rejected_frames++; return OSG_GW_RX_FULL; }
            m->occupied=true; m->identity=msg.identity; m->profile=msg.profile;
            m->boot_session=msg.boot_session; m->node=node; m->source_node=msg.source_node; m->lease=lease;
            m->configuration_generation=msg.configuration_generation;
        }
        m->online=true; m->last_seen=now; m->direct_usb=msg.direct_usb;
        /* First-seen identities enroll automatically. Explicitly disabled identities
         * remain disabled; live enabled profiles stay frozen until update-profile. */
        int registered=osg_gateway_find_slot(&g->pending,&m->identity);
        if(registered<0) {
            if(!osg_gateway_enable(g,&m->identity)) {
                g->rejected_frames++; g->rejected_announcements++; g->rejected_identity=msg.identity;
                g->admission_error="capacity_exceeded"; return OSG_GW_RX_FULL;
            }
        } else if(!g->pending.slots[registered].enabled &&
                  !osg_gateway_profile_equal(&g->pending.slots[registered].profile,&m->profile)) {
            g->pending.slots[registered].profile=m->profile; g->pending.generation++;
        }
        if(g->admission_error && osg_gateway_identity_equal(&g->rejected_identity,&m->identity)) {
            g->admission_error=NULL; memset(&g->rejected_identity,0,sizeof g->rejected_identity);
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
int osg_gateway_descriptor_index(const osg_gateway_registry_t *r,const osg_gateway_identity_t *id) {
    int index=osg_gateway_find_slot(r,id);
    return index>=0 && r->slots[index].enabled ? r->slots[index].order : -1;
}
static uint8_t enabled_count(const osg_gateway_registry_t *r) {
    uint8_t count=0;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(r->slots[i].occupied && r->slots[i].enabled) count++;
    return count;
}
static uint8_t free_report_id(const osg_gateway_t *g) {
    for(unsigned id=1;id<=255;id++) {
        bool used=false;
        for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++)
            if((g->pending.slots[i].occupied && g->pending.slots[i].report_id==id) ||
               (g->active.slots[i].occupied && g->active.slots[i].report_id==id)) used=true;
        if(!used) return (uint8_t)id;
    }
    return 0;
}
bool osg_gateway_enable(osg_gateway_t *g,const osg_gateway_identity_t *id) {
    if(!g || !id) return false;
    int index=osg_gateway_find_slot(&g->pending,id),module=osg_gateway_find_module(g,id);
    if(index>=0 && g->pending.slots[index].enabled) return true;
    if(index<0) {
        if(module<0 || !g->modules[module].online) return false;
        for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(!g->pending.slots[i].occupied) { index=(int)i; break; }
        uint8_t report_id=free_report_id(g);
        if(index<0 || !report_id) return false;
        g->pending.slots[index]=(osg_gateway_slot_t){.occupied=true,.identity=*id,
            .profile=g->modules[module].profile,.report_id=report_id,.order=255};
    }
    osg_gateway_slot_t *entry=&g->pending.slots[index];
    if(module>=0 && g->modules[module].online) entry->profile=g->modules[module].profile;
    entry->order=enabled_count(&g->pending); entry->enabled=true; g->pending.generation++;
    return true;
}
bool osg_gateway_disable(osg_gateway_t *g,const osg_gateway_identity_t *id) {
    if(!g || !id) return false;
    int index=osg_gateway_find_slot(&g->pending,id); if(index<0) return false;
    osg_gateway_slot_t *entry=&g->pending.slots[index];
    if(!entry->enabled) return true;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++)
        if(g->pending.slots[i].occupied && g->pending.slots[i].enabled && g->pending.slots[i].order>entry->order)
            g->pending.slots[i].order--;
    entry->enabled=false; entry->order=255; g->pending.generation++; return true;
}
bool osg_gateway_forget(osg_gateway_t *g,const osg_gateway_identity_t *id) {
    if(!g || !id) return false;
    int index=osg_gateway_find_slot(&g->pending,id),module=osg_gateway_find_module(g,id);
    if(index<0 || (module>=0 && g->modules[module].online)) return false;
    (void)osg_gateway_disable(g,id);
    memset(&g->pending.slots[index],0,sizeof g->pending.slots[index]); g->pending.generation++; return true;
}
bool osg_gateway_update_profile(osg_gateway_t *g,const osg_gateway_identity_t *id) {
    if(!g || !id) return false;
    int index=osg_gateway_find_slot(&g->pending,id),module=osg_gateway_find_module(g,id);
    if(index<0 || module<0 || !g->modules[module].online) return false;
    if(!osg_gateway_profile_equal(&g->pending.slots[index].profile,&g->modules[module].profile)) {
        g->pending.slots[index].profile=g->modules[module].profile; g->pending.generation++;
    }
    return true;
}
bool osg_gateway_register(osg_gateway_t *g,const osg_gateway_identity_t *id) {
    return osg_gateway_enable(g,id) && osg_gateway_update_profile(g,id);
}
bool osg_gateway_unregister(osg_gateway_t *g,const osg_gateway_identity_t *id) { return osg_gateway_forget(g,id); }
bool osg_gateway_needs_apply(const osg_gateway_t *g) {
    if(!g) return false;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) {
        const osg_gateway_slot_t *a=&g->active.slots[i],*b=&g->pending.slots[i];
        if(a->occupied!=b->occupied || (a->occupied &&
           (a->enabled!=b->enabled || a->order!=b->order || a->report_id!=b->report_id ||
            !osg_gateway_identity_equal(&a->identity,&b->identity) || !osg_gateway_profile_equal(&a->profile,&b->profile)))) return true;
    }
    return false;
}
bool osg_gateway_layout_equal(const osg_gateway_registry_t *a,const osg_gateway_registry_t *b) {
    if(!a || !b || enabled_count(a)!=enabled_count(b)) return false;
    for(unsigned order=0;order<enabled_count(a);order++) {
        const osg_gateway_slot_t *left=NULL,*right=NULL;
        for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) {
            if(a->slots[i].occupied && a->slots[i].enabled && a->slots[i].order==order) left=&a->slots[i];
            if(b->slots[i].occupied && b->slots[i].enabled && b->slots[i].order==order) right=&b->slots[i];
        }
        if(!left || !right || left->report_id!=right->report_id ||
           !osg_gateway_identity_equal(&left->identity,&right->identity) || !osg_gateway_profile_equal(&left->profile,&right->profile)) return false;
    }
    return true;
}
bool osg_gateway_activation_prepare(const osg_gateway_t *g,const osg_gateway_registry_t *r,osg_gateway_activation_t *plan) {
    if(!g || !r || !plan || !osg_gateway_registry_valid(r)) return false;
    memset(plan,0,sizeof *plan);
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(g->modules[i].occupied) {
        const osg_gateway_identity_t *id=&g->modules[i].identity;
        if((osg_gateway_descriptor_index(&g->active,id)>=0)!=(osg_gateway_descriptor_index(r,id)>=0))
            plan->identities[plan->count++]=*id;
    }
    return g->lease_counter+g->reserved_leases+plan->count<=65535;
}
bool osg_gateway_activation_reserve(osg_gateway_t *g,osg_gateway_activation_t *plan) {
    if(!g || !plan || plan->count>OSG_GW_MAX_MODULES || plan->reserved ||
       g->lease_counter+g->reserved_leases+plan->count>65535) return false;
    g->reserved_leases+=plan->count; plan->reserved=true; return true;
}
bool osg_gateway_activate_prepared(osg_gateway_t *g,const osg_gateway_registry_t *r,bool stop_input,osg_gateway_activation_t *plan) {
    if(!g || !r || !plan || plan->count>OSG_GW_MAX_MODULES || !plan->reserved || g->reserved_leases<plan->count) return false;
    if(stop_input) osg_gateway_set_running(g,false);
    for(unsigned i=0;i<plan->count;i++) {
        /* Spend reserved capacity at publication so a late Announce cannot
         * make the renewed lease older than a client's current assignment. */
        g->reserved_leases--; uint32_t lease=g->epoch+(++g->lease_counter);
        int index=osg_gateway_find_module(g,&plan->identities[i]);
        if(index<0) continue;
        osg_gateway_module_t *m=&g->modules[index];
        neutral(m); m->has_sequence=false; m->sequence=0; m->input_sequence=0; m->last_input=0;
        if(m->online) m->lease=lease;
    }
    plan->reserved=false; plan->count=0;
    g->active=*r;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(g->modules[i].occupied && !g->modules[i].online &&
        osg_gateway_find_slot(&g->active,&g->modules[i].identity)<0 && osg_gateway_find_slot(&g->pending,&g->modules[i].identity)<0)
        memset(&g->modules[i],0,sizeof g->modules[i]);
    return true;
}
bool osg_gateway_activate(osg_gateway_t *g,const osg_gateway_registry_t *r,bool stop_input) {
    osg_gateway_activation_t plan;
    if(!osg_gateway_activation_prepare(g,r,&plan) || !osg_gateway_activation_reserve(g,&plan)) {
        if(g) g->layout_error="lease_exhausted";
        return false;
    }
    return osg_gateway_activate_prepared(g,r,stop_input,&plan);
}
void osg_gateway_apply_layout(osg_gateway_t *g) {
    if(g) osg_gateway_activate(g,&g->pending,true);
}
size_t osg_gateway_report(const osg_gateway_t *g,unsigned slot,uint8_t *out,size_t capacity) {
    if(!g || !out || slot>=OSG_GW_MAX_MODULES || (!g->active.slots[slot].occupied || !g->active.slots[slot].enabled)) return 0;
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
       d->slot>=OSG_GW_MAX_MODULES || length!=(size_t)d->length+1 || delivered[0]!=g->active.slots[d->slot].report_id ||
       memcmp(delivered+1,d->report,d->length)) return false;
    const osg_gateway_slot_t *s=&g->active.slots[d->slot];
    if(!s->occupied || !s->enabled || !osg_gateway_identity_equal(&s->identity,&d->identity)) return false;
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
size_t osg_gateway_registry_descriptor(const osg_gateway_registry_t *registry,uint8_t *out,size_t capacity) {
    if(!registry || !out || !osg_gateway_registry_valid(registry)) return 0;
    uint8_t descriptor[OSG_GW_DESCRIPTOR_CAPACITY]; size_t n=0;
#define EMIT(...) do { const uint8_t items[]={__VA_ARGS__}; \
    if(n+sizeof items>sizeof descriptor) return 0; \
    memcpy(descriptor+n,items,sizeof items); n+=sizeof items; } while(0)
    for(unsigned order=0;order<enabled_count(registry);order++) {
        const osg_gateway_slot_t *entry=NULL;
        for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(registry->slots[i].occupied && registry->slots[i].enabled && registry->slots[i].order==order) entry=&registry->slots[i];
        if(!entry) return 0;
        const osg_gateway_profile_t *p=&entry->profile;
        EMIT(0x05,1,0x09,4,0xa1,1,0x85,entry->report_id);
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
    /* Zero enabled entries means CDC-only USB, with no HID placeholder. */
#undef EMIT
    if(capacity<n) return 0;
    memcpy(out,descriptor,n); return n;
}

size_t osg_gateway_descriptor(const osg_gateway_t *g,uint8_t *out,size_t capacity) {
    return g?osg_gateway_registry_descriptor(&g->active,out,capacity):0;
}
