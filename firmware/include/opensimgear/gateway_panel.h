#ifndef OSG_GATEWAY_PANEL_H
#define OSG_GATEWAY_PANEL_H
#include "opensimgear/gateway.h"

enum { OSG_GW_PANEL_FRESHNESS_MS=250 };

typedef struct {
    osg_gateway_identity_t identity;
    uint32_t boot_session, lease, gateway_epoch, configuration_generation;
    uint32_t sequence, cursor;
    /* Most recent witnessed input sample and the gateway time when it advanced. */
    uint32_t sampled_at_ms, sample_received_at_ms;
    /* Latest accepted input packet and the capture time of the axes in state. */
    uint32_t received_at_ms, state_sampled_at_ms;
    uint32_t occurrence_at_ms, fresh_at_ms;
    osg_panel_layout_t layout;
    osg_panel_state_t state;
    uint8_t node, source_node;
    bool valid, has_layout, has_input_sample, loss_notified, exhausted;
} osg_gateway_panel_slot_t;

typedef struct {
    osg_gateway_panel_slot_t slots[OSG_GW_MAX_MODULES];
} osg_gateway_panel_t;

/* Packets are borrowed for the callback. NULL withdraws continuity while the
 * slot still identifies the withdrawn scope. State is usable only when valid. */
typedef void (*osg_gateway_panel_notify_fn)(void *, unsigned slot, const osg_gateway_message_t *packet);

/* Zero-initialize the receiver. Pass decoded panel transfers on the gateway owner. */
bool osg_gateway_panel_receive(osg_gateway_panel_t *, const osg_gateway_t *,
    const osg_gateway_message_t *, uint32_t now, osg_gateway_panel_notify_fn, void *);
/* Call after gateway route maintenance, including bus resets. */
void osg_gateway_panel_tick(osg_gateway_panel_t *, const osg_gateway_t *, uint32_t now,
    osg_gateway_panel_notify_fn, void *);
#endif
