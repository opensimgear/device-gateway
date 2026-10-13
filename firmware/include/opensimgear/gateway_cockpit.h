#ifndef OSG_GATEWAY_COCKPIT_H
#define OSG_GATEWAY_COCKPIT_H
#include "opensimgear/gateway.h"
enum { OSG_GW_COCKPIT_RESPONSE_CAPACITY=32768 };
typedef enum { OSG_GW_COCKPIT_SNAPSHOT, OSG_GW_COCKPIT_REGISTER, OSG_GW_COCKPIT_UNREGISTER,
               OSG_GW_COCKPIT_SAVE, OSG_GW_COCKPIT_APPLY, OSG_GW_COCKPIT_CONFIG,
               OSG_GW_COCKPIT_ENABLE, OSG_GW_COCKPIT_DISABLE, OSG_GW_COCKPIT_FORGET, OSG_GW_COCKPIT_UPDATE_PROFILE } osg_gateway_cockpit_action_t;
typedef struct {
    uint32_t id,epoch;
    osg_gateway_cockpit_action_t action;
    osg_gateway_identity_t identity;
    uint32_t boot;
    char command[OSG_GW_CONFIG_COMMAND_CAPACITY];
} osg_gateway_cockpit_request_t;
/* Exact ASCII decimal uint32 tokens; mutations include the expected boot epoch. */
bool osg_gateway_cockpit_parse(const char *,osg_gateway_cockpit_request_t *);
/* Stage CDC3 lifecycle intent; the background layout service persists it.
 * Legacy callback/apply arguments remain for linked portable callers. */
const char *osg_gateway_cockpit_execute(osg_gateway_t *,const osg_gateway_cockpit_request_t *,
                                      bool (*persist)(void *),void *context,bool *apply);
/* One correlated JSON line. A failure returns0 and an empty string, never partial JSON. */
size_t osg_gateway_cockpit_response(const osg_gateway_t *,const char uid[25],uint32_t id,
                                   const char *error,char *out,size_t capacity);
/* The target JSON is emitted only after a complete, route-guarded module response. */
size_t osg_gateway_cockpit_config_response(const osg_gateway_t *,const char uid[25],uint32_t id,
    const char *error,const char *target_config,char *out,size_t capacity);
#endif
