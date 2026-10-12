#ifndef OSG_GATEWAY_COCKPIT_H
#define OSG_GATEWAY_COCKPIT_H
#include "opensimgear/gateway.h"
enum { OSG_GW_COCKPIT_RESPONSE_CAPACITY=8192 };
typedef enum { OSG_GW_COCKPIT_SNAPSHOT, OSG_GW_COCKPIT_REGISTER, OSG_GW_COCKPIT_UNREGISTER,
               OSG_GW_COCKPIT_SAVE, OSG_GW_COCKPIT_APPLY } osg_gateway_cockpit_action_t;
typedef struct {
    uint32_t id,epoch;
    osg_gateway_cockpit_action_t action;
    osg_gateway_identity_t identity;
} osg_gateway_cockpit_request_t;
/* Exact ASCII decimal uint32 tokens; mutations include the expected boot epoch. */
bool osg_gateway_cockpit_parse(const char *,osg_gateway_cockpit_request_t *);
/* Stage registry mutations. apply=true only after a successful persistent save. */
const char *osg_gateway_cockpit_execute(osg_gateway_t *,const osg_gateway_cockpit_request_t *,
                                      bool (*persist)(void *),void *context,bool *apply);
/* One correlated JSON line. A failure returns0 and an empty string, never partial JSON. */
size_t osg_gateway_cockpit_response(const osg_gateway_t *,const char uid[25],uint32_t id,
                                   const char *error,char *out,size_t capacity);
#endif
