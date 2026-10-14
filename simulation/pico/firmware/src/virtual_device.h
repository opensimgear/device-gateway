#pragma once
#include "runtime_config.h"
#include "device_actions.h"
#include "opensimgear/events.h"
#include "opensimgear/gateway.h"

enum { OSG_DEBUG_DEVICE_COUNT=8, OSG_DEBUG_JOURNAL_COUNT=9 };
typedef struct {
    char text[64];
} osg_virtual_display_t;
typedef struct {
    bool occupied,online,direct;
    uint8_t preset,node;
    uint32_t boot,lease,sequence,last_announce,last_input,turn_at;
    int8_t turns[OSG_MAX_ENCODERS][64];
    uint8_t turn_head[OSG_MAX_ENCODERS],turn_count[OSG_MAX_ENCODERS],turn_phase[OSG_MAX_ENCODERS];
    osg_gateway_identity_t identity;
    osg_runtime_config_t runtime;
    osg_events_t events;
    osg_profile_state_t acquisition;
    osg_profile_sample_t sample;
    uint8_t report[OSG_REPORT_CAPACITY];
    osg_device_actions_t actions;
    osg_device_display_port_t display_ports[3];
    osg_virtual_display_t displays[3];
} osg_virtual_device_t;
extern const char *const osg_debug_presets[];
extern const unsigned osg_debug_preset_count;
bool osg_virtual_init(osg_virtual_device_t *,unsigned slot,unsigned preset,uint32_t boot,uint32_t now,const osg_journal_io_t *);
void osg_virtual_tick(osg_virtual_device_t *,osg_gateway_t *,uint32_t now);
void osg_virtual_rearm(osg_virtual_device_t *,uint32_t now,bool reset_events);
void osg_virtual_ack(osg_virtual_device_t *,const osg_gateway_message_t *,uint32_t now);
const char *osg_virtual_command(osg_virtual_device_t *,const char *,uint32_t now,char *,size_t);
const char *osg_virtual_debug(osg_virtual_device_t *,const char *,int,int,uint32_t now);
