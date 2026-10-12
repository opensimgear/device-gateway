#ifndef OSG_GATEWAY_REGISTRY_H
#define OSG_GATEWAY_REGISTRY_H
#include "opensimgear/gateway_protocol.h"
enum { OSG_GW_REGISTRY_VERSION=1, OSG_GW_REGISTRY_RECORD_SIZE=500 };
typedef struct {
    bool occupied;
    osg_gateway_identity_t identity;
    osg_gateway_profile_t profile;
} osg_gateway_slot_t;
typedef struct {
    uint32_t generation;
    osg_gateway_slot_t slots[OSG_GW_MAX_MODULES];
} osg_gateway_registry_t;
bool osg_gateway_registry_valid(const osg_gateway_registry_t *);
size_t osg_gateway_registry_encode(const osg_gateway_registry_t *, uint8_t *, size_t);
bool osg_gateway_registry_decode(const uint8_t *, size_t, osg_gateway_registry_t *);
/* Returns0/1 for newest valid record, -1 if neither validates. Wrap safe. */
int osg_gateway_registry_select(const uint8_t *, size_t, const uint8_t *, size_t,
                                osg_gateway_registry_t *);
#endif
