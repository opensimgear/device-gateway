#pragma once
#include "virtual_device.h"
#include "opensimgear/gateway_layout.h"
#include "opensimgear/gateway_cockpit.h"

enum { OSG_DEBUG_LINE_CAPACITY=1024, OSG_DEBUG_RESPONSE_CAPACITY=32768, OSG_DEBUG_WATCHDOG_MS=10000 };
typedef struct {
    osg_gateway_t core;
    osg_gateway_layout_t layout;
    osg_virtual_device_t devices[OSG_DEBUG_DEVICE_COUNT];
    osg_journal_io_t journals[OSG_DEBUG_JOURNAL_COUNT];
    char uid[25],serial[40];
    uint32_t now,last_command;
    bool connected,watchdog;
    char child[OSG_GW_CONFIG_RESPONSE_CAPACITY];
} osg_debug_gateway_t;
/* journals may be NULL for volatile host tests. Platform uses nine independent
 * two-page journals (gateway plus device slots), retained across UF2 boots. */
bool osg_debug_init(osg_debug_gateway_t *,const char uid[25],const char *serial,const osg_journal_io_t *journals);
osg_gateway_layout_event_t osg_debug_tick(osg_debug_gateway_t *,uint32_t now,bool response_drained);
bool osg_debug_layout_activate(osg_debug_gateway_t *);
void osg_debug_connected(osg_debug_gateway_t *,bool connected);
/* Returns one bounded correlated JSON reply or an ASCII identity line. */
size_t osg_debug_command(osg_debug_gateway_t *,const char *line,char *out,size_t capacity);
size_t osg_debug_report_prepare(osg_debug_gateway_t *,unsigned slot,uint8_t *out,size_t capacity,osg_gateway_delivery_t *);
void osg_debug_report_complete(osg_debug_gateway_t *,const osg_gateway_delivery_t *,const uint8_t *report,size_t size);
