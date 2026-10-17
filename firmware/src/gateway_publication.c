#include "opensimgear/gateway_publication.h"
#include <string.h>

static uint32_t bit(unsigned i) { return UINT32_C(1)<<i; }
static bool occupied(const osg_gateway_slot_t *s) { return s->occupied; }
static bool inputs(const osg_gateway_profile_t *p) { return p->button_count || p->axis_count || p->hat_count; }
static const char *result(osg_publication_result_t r) {
    switch(r) {
        case OSG_PUBLICATION_OK: case OSG_PUBLICATION_NO_CHANGE: case OSG_PUBLICATION_RESTORE: return NULL;
        case OSG_PUBLICATION_PRESERVED: return "publication_preserved";
        case OSG_PUBLICATION_BUSY: return "publication_busy";
        case OSG_PUBLICATION_NOT_OWNED: return "publication_not_owned";
        case OSG_PUBLICATION_EXPIRED: return "publication_expired";
        case OSG_PUBLICATION_STALE_TIME: return "publication_stale_time";
        case OSG_PUBLICATION_EXHAUSTED: return "publication_exhausted";
        case OSG_PUBLICATION_UNVERIFIED: return "publication_unverified";
        default: return "invalid_publication_request";
    }
}
bool osg_gateway_publication_identity(osg_gateway_t *g,const osg_gateway_identity_t *id) {
    if(!g || !osg_gateway_identity_valid(id) || g->publication.lease.phase!=OSG_PUBLICATION_IDLE) return false;
    memcpy(g->publication.lease.scope.publisher,id->bytes,16);
    g->publication.lease.scope.boot=g->epoch;
    g->publication.lease.scope.route_generation=g->publication.route_generation;
    return true;
}
bool osg_gateway_publication_busy(const osg_gateway_t *g,const osg_gateway_identity_t *id) {
    if(!g || g->publication.lease.phase==OSG_PUBLICATION_IDLE) return false;
    if(!id) return true;
    int slot=osg_gateway_find_slot(&g->configured,id);
    return slot>=0 && (g->publication.target_mask&bit((unsigned)slot));
}
bool osg_gateway_publication_deliverable(const osg_gateway_t *g,const osg_gateway_identity_t *id) {
    if(!g || !id) return false;
    const osg_publication_t *s=&g->publication.lease;
    if(s->phase==OSG_PUBLICATION_IDLE) return true;
    for(unsigned n=0;n<s->original.count;n++) if(!memcmp(s->original.targets[n].identity,id->bytes,16))
        return s->phase==OSG_PUBLICATION_OWNED && !g->publication.dirty && s->desired.targets[n].enabled;
    return true;
}
bool osg_gateway_publication_inspect(const osg_gateway_t *g,const uint8_t owner[16],uint32_t lease) {
    if(!g || !owner || !lease) return false;
    const osg_publication_t *s=&g->publication.lease;
    return s->phase!=OSG_PUBLICATION_IDLE && s->scope.boot==g->epoch && s->ticket.lease_id==lease && !memcmp(owner,s->owner,16);
}
/* The descriptor readback uses captured fixed tuples only after comparing the
 * actual full native profile and report ID. A route/boot change revokes the
 * lease separately; it cannot make an admitted descriptor impossible to undo. */
static bool actual(const osg_gateway_t *g,osg_publication_view_t *v) {
    const osg_gateway_publication_t *p=&g->publication;
    *v=p->lease.original;
    for(unsigned n=0;n<v->count;n++) {
        osg_gateway_identity_t id; memcpy(id.bytes,v->targets[n].identity,16);
        int index=osg_gateway_find_slot(&g->active,&id);
        if(index<0) return false;
        const osg_gateway_slot_t *s=&g->active.slots[index];
        if(s->report_id!=v->targets[n].report_id || !osg_gateway_profile_equal(&s->profile,&p->profiles[n])) return false;
        v->targets[n].enabled=s->enabled;v->targets[n].order=s->order;
    }
    return osg_publication_view_valid(v);
}
static bool configured_sealed(const osg_gateway_t *g) {
    const osg_gateway_publication_t *p=&g->publication;
    unsigned n=0;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(p->target_mask&bit(i)) {
        if(n>=p->lease.original.count) return false;
        const osg_publication_target_t *t=&p->lease.original.targets[n];
        const osg_gateway_slot_t *s=&g->configured.slots[i];
        osg_gateway_identity_t id;memcpy(id.bytes,t->identity,16);
        if(!s->occupied || !osg_gateway_identity_equal(&s->identity,&id) || s->report_id!=t->report_id ||
           s->enabled!=t->enabled || s->order!=t->order || !osg_gateway_profile_equal(&s->profile,&p->profiles[n])) return false;
        n++;
    }
    return n==p->lease.original.count;
}
static bool sealed(const osg_gateway_t *g) {
    const osg_gateway_publication_t *p=&g->publication;
    if(!configured_sealed(g)) return false;
    for(unsigned n=0;n<p->lease.original.count;n++) {
        const osg_publication_target_t *t=&p->lease.original.targets[n];
        osg_gateway_identity_t id;memcpy(id.bytes,t->identity,16);
        int index=osg_gateway_find_module(g,&id);if(index<0) return false;
        const osg_gateway_module_t *m=&g->modules[index];
        if(!m->online || m->direct_usb || m->boot_session!=t->boot || m->configuration_generation!=t->configuration_generation ||
           !osg_gateway_profile_equal(&m->profile,&p->profiles[n]) || m->node!=p->nodes[n] || m->source_node!=p->sources[n] || m->lease!=p->module_leases[n]) return false;
    }
    return true;
}
static bool derive(const osg_gateway_registry_t *base,const osg_publication_view_t *overlay,osg_gateway_registry_t *out) {
    *out=*base;
    bool used[OSG_GW_MAX_MODULES]={false};
    unsigned count=0;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(out->slots[i].occupied) {
        osg_gateway_slot_t *s=&out->slots[i];
        for(unsigned n=0;overlay && n<overlay->count;n++) if(!memcmp(s->identity.bytes,overlay->targets[n].identity,16)) {
            s->enabled=overlay->targets[n].enabled;s->order=overlay->targets[n].order;
            if(s->enabled) {if(s->order>=OSG_GW_MAX_MODULES || used[s->order]) return false;used[s->order]=true;}
            break;
        }
        count+=s->enabled;
    }
    /* Fixed owned orders are preserved; unrelated arrivals fill the remaining
     * configured order, so their persistence never recaptures our baseline. */
    unsigned next=0;
    for(unsigned order=0;order<OSG_GW_MAX_MODULES;order++) for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) {
        osg_gateway_slot_t *s=&out->slots[i];
        if(!s->occupied || !s->enabled || base->slots[i].order!=order) continue;
        bool owned=false;
        for(unsigned n=0;overlay && n<overlay->count;n++) if(!memcmp(s->identity.bytes,overlay->targets[n].identity,16)) owned=true;
        if(owned) continue;
        while(next<OSG_GW_MAX_MODULES && used[next]) next++;
        if(next>=count) return false;
        s->order=(uint8_t)next;used[next++]=true;
    }
    return osg_gateway_registry_valid(out);
}
bool osg_gateway_publication_effective(const osg_gateway_t *g,osg_gateway_registry_t *out) {
    return g?osg_gateway_publication_target(g,&g->configured,out):false;
}
bool osg_gateway_publication_target(const osg_gateway_t *g,const osg_gateway_registry_t *base,osg_gateway_registry_t *out) {
    if(!g || !out) return false;
    const osg_publication_t *s=&g->publication.lease;
    return base && derive(base,s->phase==OSG_PUBLICATION_OWNED?&s->desired:NULL,out);
}
const char *osg_gateway_publication_claim(osg_gateway_t *g,const uint8_t owner[16],uint32_t revision,
    uint32_t targets,uint32_t enabled,uint32_t ttl,uint32_t now) {
    if(!g || !owner || !targets || (enabled&~targets)) return "invalid_publication_request";
    osg_gateway_publication_tick(g,now);
    osg_gateway_publication_t *p=&g->publication;
    osg_publication_view_t v,desired;
    osg_publication_ticket_t ticket;
    if(p->lease.phase!=OSG_PUBLICATION_IDLE) {
        if(revision!=p->claim_revision || targets!=p->target_mask || enabled!=p->enabled_mask || !actual(g,&v)) return "publication_busy";
        return result(osg_publication_claim_current(&p->lease,&p->lease.scope,owner,&v,&p->lease.desired,now,ttl,&ticket));
    }
    if(!g->publication_revision || revision!=g->publication_revision) return "stale_publication_revision";
    if(g->layout_transition || osg_gateway_needs_apply(g) || p->dirty || !osg_gateway_layout_equal(&g->active,&g->configured)) return "publication_not_settled";
    memset(&v,0,sizeof v);
    osg_gateway_registry_t target=g->active;
    /* Remove selected collections first, then append newly enabled targets. */
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(targets&bit(i)) {
        const osg_gateway_slot_t *s=&g->configured.slots[i];
        int module=occupied(s)?osg_gateway_find_module(g,&s->identity):-1;
        if(module<0 || !g->modules[module].online || g->modules[module].direct_usb || !inputs(&s->profile) ||
           !s->report_id || s->report_id>32 || !g->modules[module].boot_session || !g->modules[module].configuration_generation ||
           !osg_gateway_profile_equal(&s->profile,&g->modules[module].profile)) return "publication_target_not_ready";
        const osg_gateway_module_t *m=&g->modules[module];
        osg_publication_target_t *t=&v.targets[v.count];
        memcpy(t->identity,s->identity.bytes,16);t->boot=m->boot_session;t->configuration_generation=m->configuration_generation;
        t->profile_revision=(uint64_t)s->profile.revision+1U;t->input_capable=true;t->report_id=s->report_id;t->enabled=s->enabled;t->order=s->order;
        p->profiles[v.count]=s->profile;p->nodes[v.count]=m->node;p->sources[v.count]=m->source_node;p->module_leases[v.count]=m->lease;v.count++;
        if(!(enabled&bit(i)) && target.slots[i].enabled) {
            unsigned old=target.slots[i].order;
            for(unsigned j=0;j<OSG_GW_MAX_MODULES;j++) if(target.slots[j].occupied && target.slots[j].enabled && target.slots[j].order>old) target.slots[j].order--;
            target.slots[i].enabled=false;target.slots[i].order=255;
        }
    }
    unsigned count=0;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(target.slots[i].occupied && target.slots[i].enabled) count++;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if((enabled&bit(i)) && !target.slots[i].enabled) {target.slots[i].enabled=true;target.slots[i].order=(uint8_t)count++;}
    desired=v;unsigned n=0;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(targets&bit(i)) {desired.targets[n].enabled=target.slots[i].enabled;desired.targets[n++].order=target.slots[i].order;}
    osg_publication_scope_t scope=p->lease.scope;scope.boot=g->epoch;scope.route_generation=p->route_generation;
    osg_publication_result_t r=osg_publication_claim_current(&p->lease,&scope,owner,&v,&desired,now,ttl,&ticket);
    if(r==OSG_PUBLICATION_OK) {p->claim_revision=revision;p->target_mask=targets;p->enabled_mask=enabled;p->dirty=true;osg_gateway_publication_changed(g);}
    return result(r);
}
const char *osg_gateway_publication_renew(osg_gateway_t *g,const uint8_t owner[16],uint32_t lease,uint32_t ttl,uint32_t now) {
    if(!g) return "invalid_publication_request";
    osg_gateway_publication_tick(g,now);osg_publication_ticket_t ticket;
    return result(osg_publication_renew(&g->publication.lease,&g->publication.lease.scope,owner,lease,now,ttl,&ticket));
}
static osg_publication_result_t revoked(osg_gateway_t *g,osg_publication_result_t r) {
    if(r==OSG_PUBLICATION_RESTORE && !configured_sealed(g)) {
        /* The primitive granted only captured-original restoration. A newer
         * configured tuple narrows that permission to preservation; it must
         * never be published under an old owner's restoration authority. */
        g->publication.lease.phase=OSG_PUBLICATION_IDLE;
        g->publication.lease.outcome=r=OSG_PUBLICATION_PRESERVED;
    }
    if(r==OSG_PUBLICATION_RESTORE) g->publication.dirty=true;
    else if(r==OSG_PUBLICATION_NO_CHANGE || r==OSG_PUBLICATION_PRESERVED) g->publication.dirty=false;
    osg_gateway_set_running(g,false);osg_gateway_publication_changed(g);
    return r;
}
const char *osg_gateway_publication_release(osg_gateway_t *g,const uint8_t owner[16],uint32_t lease,uint32_t now) {
    if(!g) return "invalid_publication_request";
    osg_publication_view_t v;bool valid=actual(g,&v);
    osg_publication_phase_t before=g->publication.lease.phase;
    osg_publication_result_t r=osg_publication_release(&g->publication.lease,&g->publication.lease.scope,owner,lease,valid?&v:NULL,now);
    if(before!=g->publication.lease.phase) r=revoked(g,r);
    return result(r);
}
void osg_gateway_publication_tick(osg_gateway_t *g,uint32_t now) {
    if(!g) return;
    osg_publication_t *s=&g->publication.lease;
    if(s->phase!=OSG_PUBLICATION_OWNED) return;
    osg_publication_view_t v;bool valid=actual(g,&v);
    osg_publication_result_t r;
    if(!sealed(g)) r=osg_publication_invalidate(s,&s->scope,valid?&v:NULL,now);
    else r=osg_publication_tick(s,valid?&v:NULL,now);
    if(s->phase!=OSG_PUBLICATION_OWNED) revoked(g,r);
}
void osg_gateway_publication_transport_lost(osg_gateway_t *g,uint32_t now) {
    if(!g) return;
    osg_publication_t *s=&g->publication.lease;osg_publication_view_t v;bool valid=actual(g,&v);
    if(s->phase==OSG_PUBLICATION_OWNED) revoked(g,osg_publication_invalidate(s,&s->scope,valid?&v:NULL,now));
    if(g->publication.route_generation!=UINT64_MAX) g->publication.route_generation++;
    else g->publication_revision=0;
    g->publication.expected_reconnect=false;osg_gateway_publication_changed(g);
}
void osg_gateway_publication_reconnect(osg_gateway_t *g,uint32_t now,bool connected) {
    if(!g || !g->publication.expected_reconnect) return;
    if(connected) g->publication.expected_reconnect=false;
    else if((int32_t)(now-g->publication.reconnect_until)>=0) osg_gateway_publication_transport_lost(g,now);
}
bool osg_gateway_publication_admit(osg_gateway_t *g,const osg_publication_ticket_t *ticket,bool restore,uint32_t now) {
    if(!g || !ticket) return false;
    osg_gateway_publication_tick(g,now);osg_publication_view_t v;
    if(restore && g->publication.lease.phase==OSG_PUBLICATION_RESTORING && !configured_sealed(g)) {
        (void)revoked(g,OSG_PUBLICATION_RESTORE);return false;
    }
    if(!actual(g,&v)) return false;
    osg_publication_t *s=&g->publication.lease;
    return restore?osg_publication_restore_allowed(s,ticket,&v):osg_publication_begin_apply(s,&s->scope,s->owner,ticket,&v,now);
}
void osg_gateway_publication_applied(osg_gateway_t *g,const osg_publication_ticket_t *ticket,bool restore,uint32_t now) {
    if(!g || !ticket) return;
    osg_gateway_publication_t *p=&g->publication;osg_publication_view_t v;
    bool ok=actual(g,&v) && (restore?osg_publication_confirm_restored(&p->lease,ticket,&v):
        osg_publication_confirm_applied(&p->lease,&p->lease.scope,p->lease.owner,ticket,&v,now));
    if(ok) {
        unsigned n=0;
        for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(p->target_mask&bit(i)) {
            int module=osg_gateway_find_module(g,&g->configured.slots[i].identity);
            if(module>=0) p->module_leases[n]=g->modules[module].lease;
            n++;
        }
        p->dirty=false;
    }
}
