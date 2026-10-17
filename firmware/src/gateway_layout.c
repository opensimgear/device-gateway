#include "opensimgear/gateway_layout.h"
#include "opensimgear/gateway_publication.h"
/* Worst legal collection: button/hat padding plus eight axes, 114 bytes. */
_Static_assert(OSG_GW_DESCRIPTOR_CAPACITY>=114U*OSG_GW_MAX_MODULES,"All accepted profiles fit the HID descriptor");
void osg_gateway_layout_schedule(osg_gateway_layout_t *s,uint32_t now) {
    if(s) { s->pending=true; s->apply_at=now+100U; }
}
osg_gateway_layout_event_t osg_gateway_layout_tick(osg_gateway_layout_t *s,uint32_t now,bool drained) {
    if(!s) return OSG_GATEWAY_LAYOUT_IDLE;
    if(s->detached && (int32_t)(now-s->reattach_at)>=0) {
        s->detached=false; s->reconnects++; return OSG_GATEWAY_LAYOUT_ATTACH;
    }
    if(s->pending && (drained || (int32_t)(now-(s->apply_at+1000U))>=0) && (int32_t)(now-s->apply_at)>=0) {
        s->pending=false; s->detached=true; s->reattach_at=now+250U; return OSG_GATEWAY_LAYOUT_DETACH;
    }
    return OSG_GATEWAY_LAYOUT_IDLE;
}
static bool current(const osg_gateway_layout_t *s,const osg_gateway_t *g) {
    const osg_publication_t *p=&g->publication.lease;
    return !s->publication_guard || (s->publication_ticket.lease_id==p->ticket.lease_id &&
        s->publication_ticket.transition==p->ticket.transition &&
        p->phase==(s->publication_restore?OSG_PUBLICATION_RESTORING:OSG_PUBLICATION_OWNED));
}
static void cancel(osg_gateway_layout_t *s,osg_gateway_t *g) {
    /* A saved unrelated arrival still needs publication if revocation cancels
     * its old overlay-fenced schedule. Rebuild the saved baseline without a
     * second flash write. Never replace unknown or independently newer state. */
    if(!s->publication_apply && g->publication.lease.phase!=OSG_PUBLICATION_ATTENTION &&
       g->publication.lease.outcome!=OSG_PUBLICATION_PRESERVED) s->rederive=true;
    if(s->activation.reserved) {g->reserved_leases-=s->activation.count;s->activation.reserved=false;s->activation.count=0;}
    s->pending=false;s->publication_guard=s->publication_apply=s->publication_restore=false;g->layout_transition=false;
}
bool osg_gateway_layout_activate(osg_gateway_layout_t *s,osg_gateway_t *g,uint32_t now) {
    if(!s || !g) return false;
    osg_gateway_publication_tick(g,now);
    if(!current(s,g) || (s->publication_apply && !osg_gateway_publication_admit(g,&s->publication_ticket,s->publication_restore,now))) {cancel(s,g);return false;}
    if(!osg_gateway_activate_prepared(g,&s->target,true,&s->activation)) {cancel(s,g);return false;}
    if(s->publication_apply) osg_gateway_publication_applied(g,&s->publication_ticket,s->publication_restore,now);
    if(s->publication_guard) {g->publication.expected_reconnect=true;g->publication.reconnect_until=now+1500U;}
    return true;
}
osg_gateway_layout_event_t osg_gateway_layout_service(osg_gateway_layout_t *s,osg_gateway_t *g,uint32_t now,
    bool drained,bool (*persist)(void *,const osg_gateway_registry_t *),void *context) {
    if(!s || !g) return OSG_GATEWAY_LAYOUT_IDLE;
    osg_gateway_publication_tick(g,now);
    if(s->pending && !current(s,g)) cancel(s,g);
    osg_gateway_layout_event_t event=osg_gateway_layout_tick(s,now,drained);
    if(event==OSG_GATEWAY_LAYOUT_DETACH) return event;
    if(event==OSG_GATEWAY_LAYOUT_ATTACH) {g->layout_transition=false;return event;}
    if(s->pending || s->detached) return event;
    bool publication=g->publication.dirty;
    bool volatile_layout=publication || s->rederive;
    if(!volatile_layout && !osg_gateway_needs_apply(g)) {s->draft=false;g->layout_error=NULL;return event;}
    if(!volatile_layout) {
        if(!s->draft) {s->draft=true;s->draft_at=now;s->due_at=now+100U;s->observed_generation=g->pending.generation;}
        else if(s->observed_generation!=g->pending.generation) {s->observed_generation=g->pending.generation;s->due_at=now+100U;}
        if(((int32_t)(now-s->due_at)<0 && (uint32_t)(now-s->draft_at)<500U) ||
           (g->layout_error && (int32_t)(now-s->retry_at)<0)) return event;
    }
    if(!osg_gateway_publication_target(g,volatile_layout?&g->configured:&g->pending,&s->target)) {
        g->layout_error="invalid_layout";s->retry_at=now+1000U;return event;
    }
    if(!osg_gateway_activation_prepare(g,&s->target,&s->activation)) {g->layout_error="lease_exhausted";s->retry_at=now+1000U;return event;}
    /* Only configured intent reaches flash. Effective overlays never do. */
    if(!volatile_layout) {
        if(!persist || !persist(context,&g->pending)) {g->layout_error="flash_save_failed";s->retry_at=now+1000U;return event;}
        g->configured=g->pending;osg_gateway_publication_changed(g);
    }
    if(!osg_gateway_activation_reserve(g,&s->activation)) {g->layout_error="lease_exhausted";s->retry_at=now+1000U;return event;}
    s->publication_guard=g->publication.lease.phase==OSG_PUBLICATION_OWNED || g->publication.lease.phase==OSG_PUBLICATION_RESTORING;
    s->publication_apply=publication;s->publication_restore=g->publication.lease.phase==OSG_PUBLICATION_RESTORING;s->publication_ticket=g->publication.lease.ticket;
    g->layout_error=NULL;s->retry_at=now;s->draft=false;s->rederive=false;
    if(osg_gateway_layout_equal(&g->active,&s->target)) {
        if(s->publication_apply && !osg_gateway_publication_admit(g,&s->publication_ticket,s->publication_restore,now)) {cancel(s,g);return event;}
        (void)osg_gateway_activate_prepared(g,&s->target,false,&s->activation);
        if(s->publication_apply) osg_gateway_publication_applied(g,&s->publication_ticket,s->publication_restore,now);
    } else {g->layout_transition=true;osg_gateway_layout_schedule(s,now);}
    return event;
}
