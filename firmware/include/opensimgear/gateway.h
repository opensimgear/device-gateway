#ifndef OSG_GATEWAY_H
#define OSG_GATEWAY_H
#include "opensimgear/gateway_registry.h"
enum { OSG_GW_INPUT_TIMEOUT_MS=250, OSG_GW_OFFLINE_TIMEOUT_MS=1500,
       OSG_GW_DESCRIPTOR_CAPACITY=1536 };
typedef struct {
    bool occupied, online, direct_usb, has_input, has_sequence;
    osg_gateway_identity_t identity;
    osg_gateway_profile_t profile;
    uint8_t node,source_node; /* application route and independent Cyphal/CAN source */
    uint32_t boot_session, lease, sequence, input_sequence, last_seen, last_input,configuration_generation;
    uint64_t delivery_generation;
    uint8_t report[OSG_GW_MAX_REPORT_SIZE];
} osg_gateway_module_t;
typedef struct {
    osg_gateway_module_t modules[OSG_GW_MAX_MODULES];
    osg_gateway_registry_t pending, active;
    uint32_t epoch, lease_counter;
    bool running;
    uint64_t run_generation;
    uint32_t accepted_frames, rejected_frames;
} osg_gateway_t;
/* Immutable snapshot of an eligible input submitted to the USB interrupt endpoint. */
typedef struct {
    bool valid;
    uint8_t slot,node,length;
    osg_gateway_identity_t identity;
    uint32_t boot_session,lease,sequence;
    uint64_t run_generation,delivery_generation;
    uint8_t report[OSG_GW_MAX_REPORT_SIZE];
} osg_gateway_delivery_t;
typedef enum { OSG_GW_RX_REJECTED, OSG_GW_RX_ACCEPTED, OSG_GW_RX_ASSIGNMENT,
               OSG_GW_RX_FULL } osg_gateway_receive_result_t;
/* Nonzero epoch is a persisted lease block base, advanced by65536 per boot.
 * At most65535 leases per boot; do not reuse a block within retained traffic. */
bool osg_gateway_init(osg_gateway_t *, uint32_t epoch, const osg_gateway_registry_t *);
void osg_gateway_tick(osg_gateway_t *, uint32_t now);
void osg_gateway_set_running(osg_gateway_t *, bool);
void osg_gateway_bus_reset(osg_gateway_t *); /* neutral + revoke every lease */
/* Only complete, decoded Cyphal transfers are passed to the application core. */
osg_gateway_receive_result_t osg_gateway_receive(osg_gateway_t *, const osg_gateway_message_t *,
                                                uint32_t now, osg_gateway_message_t *reply);
int osg_gateway_find_module(const osg_gateway_t *, const osg_gateway_identity_t *);
int osg_gateway_find_slot(const osg_gateway_registry_t *, const osg_gateway_identity_t *);
bool osg_gateway_register(osg_gateway_t *, const osg_gateway_identity_t *);
bool osg_gateway_unregister(osg_gateway_t *, const osg_gateway_identity_t *);
bool osg_gateway_needs_apply(const osg_gateway_t *);
/* Call only after persistent save succeeds and USB has disconnected. */
void osg_gateway_apply_layout(osg_gateway_t *);
/* Active slot indexes are stable; HID report ID is slot+1. Returns0 for empty. */
size_t osg_gateway_report(const osg_gateway_t *, unsigned slot, uint8_t *, size_t);
size_t osg_gateway_descriptor(const osg_gateway_t *, uint8_t *, size_t);
/* Neutral fallback reports remain deliverable, but never qualify for a CAN ACK. */
size_t osg_gateway_delivery_prepare(const osg_gateway_t *, unsigned slot, uint8_t *, size_t, osg_gateway_delivery_t *);
/* Invoke only from a successful USB interrupt completion. delivered includes its report ID. */
bool osg_gateway_delivery_complete(const osg_gateway_t *, const osg_gateway_delivery_t *,
                                   const uint8_t *delivered, size_t length, osg_gateway_message_t *ack);
#endif
