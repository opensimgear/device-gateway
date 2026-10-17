#pragma once
#include "opensimgear/gateway.h"

/* Board initializes the exact physical composite publisher identity once. */
bool osg_gateway_publication_identity(osg_gateway_t *,const osg_gateway_identity_t *);
bool osg_gateway_publication_busy(const osg_gateway_t *,const osg_gateway_identity_t *);
bool osg_gateway_publication_deliverable(const osg_gateway_t *,const osg_gateway_identity_t *);
bool osg_gateway_publication_inspect(const osg_gateway_t *,const uint8_t owner[16],uint32_t lease);
const char *osg_gateway_publication_claim(osg_gateway_t *,const uint8_t owner[16],
    uint32_t revision,uint32_t targets,uint32_t enabled,uint32_t ttl,uint32_t now);
const char *osg_gateway_publication_renew(osg_gateway_t *,const uint8_t owner[16],uint32_t lease,uint32_t ttl,uint32_t now);
const char *osg_gateway_publication_release(osg_gateway_t *,const uint8_t owner[16],uint32_t lease,uint32_t now);
void osg_gateway_publication_tick(osg_gateway_t *,uint32_t now);
/* Real transport loss always revokes. A bounded, explicitly scheduled USB
 * detach is exempt only until its exact reconnect deadline. */
void osg_gateway_publication_transport_lost(osg_gateway_t *,uint32_t now);
void osg_gateway_publication_reconnect(osg_gateway_t *,uint32_t now,bool connected);
bool osg_gateway_publication_effective(const osg_gateway_t *,osg_gateway_registry_t *);
bool osg_gateway_publication_target(const osg_gateway_t *,const osg_gateway_registry_t *,osg_gateway_registry_t *);
bool osg_gateway_publication_admit(osg_gateway_t *,const osg_publication_ticket_t *,bool restore,uint32_t now);
void osg_gateway_publication_applied(osg_gateway_t *,const osg_publication_ticket_t *,bool restore,uint32_t now);
