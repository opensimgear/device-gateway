#include "opensimgear/gateway_layout.h"
void osg_gateway_layout_schedule(osg_gateway_layout_t *s,uint32_t now) { if(s) { s->pending=true; s->apply_at=now+100U; } }
osg_gateway_layout_event_t osg_gateway_layout_tick(osg_gateway_layout_t *s,uint32_t now,bool drained) {
    if(!s) return OSG_GATEWAY_LAYOUT_IDLE;
    if(s->detached && (int32_t)(now-s->reattach_at)>=0) {
        s->detached=false; s->reconnects++; return OSG_GATEWAY_LAYOUT_ATTACH;
    }
    if(s->pending && drained && (int32_t)(now-s->apply_at)>=0) {
        s->pending=false; s->detached=true; s->reattach_at=now+250U; return OSG_GATEWAY_LAYOUT_DETACH;
    }
    return OSG_GATEWAY_LAYOUT_IDLE;
}
