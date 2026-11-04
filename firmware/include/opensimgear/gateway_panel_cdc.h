#ifndef OSG_GATEWAY_PANEL_CDC_H
#define OSG_GATEWAY_PANEL_CDC_H
#include "opensimgear/gateway_panel.h"

enum { OSG_GW_PANEL_CDC_DEPTH=8, OSG_GW_PANEL_CDC_LINE=232 };
typedef struct {
    struct {
        char line[OSG_GW_PANEL_CDC_LINE];
        size_t size;
        uint32_t queued_at;
    } entries[OSG_GW_PANEL_CDC_DEPTH];
    const osg_gateway_panel_t *receiver;
    uint32_t now;
    unsigned head, count;
    bool enabled, reset_pending;
} osg_gateway_panel_cdc_t;

void osg_gateway_panel_cdc_enable(osg_gateway_panel_cdc_t *, bool enabled);
void osg_gateway_panel_cdc_notify(void *, unsigned, const osg_gateway_message_t *);
const char *osg_gateway_panel_cdc_peek(osg_gateway_panel_cdc_t *, size_t *size);
void osg_gateway_panel_cdc_pop(osg_gateway_panel_cdc_t *);
#endif
