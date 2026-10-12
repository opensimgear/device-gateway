#pragma once
#include "opensimgear/gateway.h"
typedef struct { bool pending,detached; uint32_t apply_at,reattach_at; uint32_t reconnects; } osg_gateway_layout_t;
typedef enum { OSG_GATEWAY_LAYOUT_IDLE,OSG_GATEWAY_LAYOUT_DETACH,OSG_GATEWAY_LAYOUT_ATTACH } osg_gateway_layout_event_t;
void osg_gateway_layout_schedule(osg_gateway_layout_t *,uint32_t now);
/* DETACH is returned before apply: disconnect USB, apply layout, rebuild descriptors.
 * ATTACH follows at least250ms later. pending stays true while the response drains. */
osg_gateway_layout_event_t osg_gateway_layout_tick(osg_gateway_layout_t *,uint32_t now,bool response_drained);
