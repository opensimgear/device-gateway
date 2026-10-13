#include "opensimgear/gateway_layout.h"
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
    /* A non-reading serial client cannot block automatic enrollment forever. */
    if(s->pending && (drained || (int32_t)(now-(s->apply_at+1000U))>=0) && (int32_t)(now-s->apply_at)>=0) {
        s->pending=false; s->detached=true; s->reattach_at=now+250U; return OSG_GATEWAY_LAYOUT_DETACH;
    }
    return OSG_GATEWAY_LAYOUT_IDLE;
}
osg_gateway_layout_event_t osg_gateway_layout_service(osg_gateway_layout_t *s,osg_gateway_t *g,uint32_t now,
    bool drained,bool (*persist)(void *,const osg_gateway_registry_t *),void *context) {
    if(!s || !g) return OSG_GATEWAY_LAYOUT_IDLE;
    osg_gateway_layout_event_t event=osg_gateway_layout_tick(s,now,drained);
    if(event==OSG_GATEWAY_LAYOUT_DETACH) {
        return event;
    }
    if(event==OSG_GATEWAY_LAYOUT_ATTACH) { g->layout_transition=false; return event; }
    if(s->pending || s->detached) return event;
    if(!osg_gateway_needs_apply(g)) { s->draft=false; g->layout_error=NULL; return event; }
    if(!s->draft) { s->draft=true; s->draft_at=now; s->due_at=now+100U; s->observed_generation=g->pending.generation; }
    else if(s->observed_generation!=g->pending.generation) {
        s->observed_generation=g->pending.generation; s->due_at=now+100U;
    }
    if(((int32_t)(now-s->due_at)<0 && (uint32_t)(now-s->draft_at)<500U) ||
       (g->layout_error && (int32_t)(now-s->retry_at)<0)) return event;
    s->target=g->pending;
    if(!osg_gateway_registry_valid(&s->target)) { g->layout_error="invalid_layout"; s->retry_at=now+1000U; return event; }
    if(!osg_gateway_activation_prepare(g,&s->target,&s->activation)) {
        g->layout_error="lease_exhausted"; s->retry_at=now+1000U; return event;
    }
    if(!persist || !persist(context,&s->target)) {
        g->layout_error="flash_save_failed"; s->retry_at=now+1000U; return event;
    }
    if(!osg_gateway_activation_reserve(g,&s->activation)) {
        g->layout_error="lease_exhausted"; s->retry_at=now+1000U; return event;
    }
    g->layout_error=NULL; s->retry_at=now; s->draft=false;
    if(osg_gateway_layout_equal(&g->active,&s->target)) (void)osg_gateway_activate_prepared(g,&s->target,false,&s->activation);
    else { g->layout_transition=true; osg_gateway_layout_schedule(s,now); }
    return event;
}
