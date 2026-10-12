#pragma once
#include "opensimgear/gateway_cockpit.h"
#include "opensimgear/cyphal.h"
enum { OSG_GATEWAY_CONFIG_TIMEOUT_MS=4000 };
typedef struct {
    osg_gateway_message_t route;
    char command[OSG_GW_CONFIG_COMMAND_CAPACITY];
    char response[OSG_GW_CONFIG_RESPONSE_CAPACITY];
    uint16_t sent,received,total;
    uint32_t started;
    bool busy,complete;
} osg_gateway_config_t;
const char *osg_gateway_config_start(osg_gateway_config_t *,const osg_gateway_t *,
    const osg_gateway_cockpit_request_t *,uint32_t now);
void osg_gateway_config_tick(osg_gateway_config_t *,const osg_gateway_t *,osg_cyphal_t *,uint32_t now);
bool osg_gateway_config_receive(osg_gateway_config_t *,const osg_gateway_t *,const osg_gateway_message_t *);
/* NULL until complete. Once complete, result is an error token or NULL success. */
bool osg_gateway_config_result(osg_gateway_config_t *,const char **error,const char **json);
void osg_gateway_config_cancel(osg_gateway_config_t *);
