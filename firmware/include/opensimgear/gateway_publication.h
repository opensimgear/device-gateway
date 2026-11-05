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

enum { OSG_GW_PUBLICATION_DECIMAL_CAPACITY=21 };
/* Return the start of the NUL-terminated decimal string within out. */
const char *osg_gateway_publication_decimal(uint64_t,char out[OSG_GW_PUBLICATION_DECIMAL_CAPACITY]);

enum { OSG_GW_PUBLICATION_STREAM_LINE=96, OSG_GW_PUBLICATION_STREAM_FIELDS=17 };
typedef struct {
    uint64_t observed[OSG_GW_PUBLICATION_STREAM_FIELDS],sequence;
    const char *layout_error,*admission_error;
    osg_gateway_identity_t rejected_identity;
    bool enabled,pending,inventory_pending,lost;
} osg_gateway_publication_stream_t;

/* Connection-local and opt-in. Observe while the writer is blocked so a
 * change cannot be erased by a return to the previous state. Peek and pop
 * hand one complete line to the transport's ordered output buffer. */
void osg_gateway_publication_stream_enable(osg_gateway_publication_stream_t *,bool enabled);
void osg_gateway_publication_stream_observe(osg_gateway_publication_stream_t *,const osg_gateway_t *);
size_t osg_gateway_publication_stream_peek(const osg_gateway_publication_stream_t *,char out[OSG_GW_PUBLICATION_STREAM_LINE]);
void osg_gateway_publication_stream_pop(osg_gateway_publication_stream_t *);
