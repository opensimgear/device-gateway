#pragma once
#include "opensimgear/gateway.h"
typedef struct {
    bool pending,detached,draft;
    uint32_t apply_at,reattach_at,reconnects,observed_generation,draft_at,due_at,retry_at;
    osg_gateway_registry_t target;
    osg_gateway_activation_t activation;
} osg_gateway_layout_t;
typedef enum { OSG_GATEWAY_LAYOUT_IDLE,OSG_GATEWAY_LAYOUT_DETACH,OSG_GATEWAY_LAYOUT_ATTACH } osg_gateway_layout_event_t;
void osg_gateway_layout_schedule(osg_gateway_layout_t *,uint32_t now);
osg_gateway_layout_event_t osg_gateway_layout_tick(osg_gateway_layout_t *,uint32_t now,bool response_drained);
/* Save one immutable generation before DETACH. New announcements queue another
 * transaction. On DETACH, disconnect then activate_prepared with s->target and
 * s->activation, then rebuild. Reserved capacity cannot be spent by arrivals.
 * Background work is independent of Cockpit and serial presence. */
osg_gateway_layout_event_t osg_gateway_layout_service(osg_gateway_layout_t *,osg_gateway_t *,uint32_t now,
    bool response_drained,bool (*persist)(void *,const osg_gateway_registry_t *),void *context);
