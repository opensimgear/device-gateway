#include "board.h"
#include "gateway_usb.h"
#include "opensimgear/gateway.h"
#include "opensimgear/gateway_cockpit.h"
#include "opensimgear/gateway_config.h"
#include "opensimgear/gateway_layout.h"
#include "opensimgear/cyphal.h"
#include "opensimgear/line.h"
#include "tusb.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static osg_gateway_t gateway;
static osg_cyphal_t cyphal;
static uint8_t sent[OSG_GW_MAX_MODULES][OSG_GW_MAX_REPORT_SIZE];
static bool sent_valid[OSG_GW_MAX_MODULES], hid_inflight;
static uint32_t last_report[OSG_GW_MAX_MODULES];
static unsigned next_slot;
static char response[OSG_GW_COCKPIT_RESPONSE_CAPACITY];
static osg_gateway_delivery_t inflight_delivery;
static size_t response_size,response_offset;
static osg_gateway_layout_t layout;
static osg_gateway_config_t configuration;

static bool can_send(void *context,const osg_gateway_frame_t *frame) {
    (void)context;
    board_can_frame_t outgoing={.id=frame->id,.size=frame->length,.fd=frame->fd,.brs=frame->brs,
        .deadline_ms=frame->deadline_ms,.deadline_valid=frame->deadline_valid};
    memcpy(outgoing.data,frame->data,frame->length);
    return board_can_enqueue(&outgoing);
}
static void migrate_can_source(uint32_t now,bool running) {
    /* Already admitted controller frames still contain the old source ID. */
    board_can_reset(now); osg_cyphal_reset(&cyphal); osg_gateway_bus_reset(&gateway);
    memset(sent_valid,0,sizeof sent_valid); inflight_delivery.valid=false;
    osg_gateway_set_running(&gateway,running);
}

static void invalidate_reports(void) {
    memset(sent_valid,0,sizeof sent_valid); hid_inflight=false; inflight_delivery.valid=false;
    osg_gateway_set_running(&gateway,false);
}
static void append(const char *format,...) {
    if(response_size>=sizeof response-1) return;
    va_list args; va_start(args,format);
    int written=vsnprintf(response+response_size,sizeof response-response_size,format,args);
    va_end(args);
    if(written>0) {
        size_t available=sizeof response-response_size;
        response_size+=(size_t)written<available ? (size_t)written : available-1;
    }
}
static bool save_registry(const osg_gateway_registry_t *registry) {
    uint8_t record[OSG_GW_REGISTRY_RECORD_SIZE];
    size_t size=osg_gateway_registry_encode(registry,record,sizeof record);
    return size && board_registry_commit(record,size,gateway.epoch);
}
static void identity(void) {
    char uid[25]; board_uid_hex(uid);
    append("gatewayType=stm32-can-gateway protocol=3 canProtocol=3 firmware=0.4.0 board=gateway_g0b1cb_v0_1_0 identity=%s epoch=%lu maxModules=%u\r\n",
        uid,(unsigned long)gateway.epoch,OSG_GW_MAX_MODULES);
}
static bool layout_save(void *context,const osg_gateway_registry_t *registry) { (void)context; return save_registry(registry); }
static void cockpit_command(const char *line,uint32_t now) {
    osg_gateway_cockpit_request_t request={0};
    const char *error=NULL;
    bool apply=false;
    if(!osg_gateway_cockpit_parse(line,&request)) error="invalid_request";
    else if(request.action==OSG_GW_COCKPIT_CONFIG || request.action==OSG_GW_COCKPIT_ACTION || request.action==OSG_GW_COCKPIT_EVENTS) {
        error=osg_gateway_config_start(&configuration,&gateway,&request,now);
        if(!error) return;
    } else error=osg_gateway_cockpit_execute(&gateway,&request,NULL,NULL,&apply);
    char uid[25]; board_uid_hex(uid);
    response_size=osg_gateway_cockpit_response(&gateway,uid,request.id,error,response,sizeof response);
    if(!response_size) append("{\"id\":%lu,\"ok\":false,\"error\":\"response_overflow\",\"state\":null}\r\n",(unsigned long)request.id);
}
static void command(const char *line,uint32_t now) {
    if(!strncmp(line,"cockpit ",8)) cockpit_command(line,now);
    else if(!strcmp(line,"hello") || !strcmp(line,"identity") || !strcmp(line,"info")) identity();
    else if(!strcmp(line,"help")) append("identity | inventory | registry | enable <32hex> | disable <32hex> | forget <32hex> | update-profile <32hex> | diagnostics | cockpit <id> snapshot | cockpit <id> <epoch> enable <32hex>|disable <32hex>|forget <32hex>|update-profile <32hex>|config <32hex> <boot> <child-command>|action <32hex> <boot> <child-command>|events <32hex> <boot> <child-command>\r\n");
    else if(!strcmp(line,"inventory")) {
        identity();
        for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) {
            const osg_gateway_module_t *module=&gateway.modules[i]; if(!module->occupied) continue;
            char id[33]; osg_gateway_identity_format(&module->identity,id);
            int desired=osg_gateway_find_slot(&gateway.pending,&module->identity);
            int published=osg_gateway_find_slot(&gateway.active,&module->identity);
            append("module identity=%s node=%u online=%u directUsb=%u input=%u axes=%u buttons=%u hats=%u revision=%u enabled=%u published=%u descriptorIndex=%d reportId=%u lastSeenMs=%lu\r\n",
                id,module->node,module->online,module->direct_usb,module->has_input,module->profile.axis_count,
                module->profile.button_count,module->profile.hat_count,module->profile.revision,
                desired>=0 && gateway.pending.slots[desired].enabled,
                published>=0 && gateway.active.slots[published].enabled,
                osg_gateway_descriptor_index(&gateway.active,&module->identity),
                published>=0 && gateway.active.slots[published].enabled ? gateway.active.slots[published].report_id : 0,
                (unsigned long)(now-module->last_seen));
        }
        append("OK inventory\r\n");
    } else if(!strcmp(line,"registry")) {
        for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) {
            const osg_gateway_slot_t *pending=&gateway.pending.slots[i],*active=&gateway.active.slots[i];
            char p[33]="-",a[33]="-";
            if(pending->occupied) osg_gateway_identity_format(&pending->identity,p);
            if(active->occupied) osg_gateway_identity_format(&active->identity,a);
            append("record=%u reportId=%u order=%u enabled=%u pending=%s active=%s\r\n",i,pending->report_id,pending->order,pending->enabled,p,a);
        }
        append("OK applyRequired=%u\r\n",osg_gateway_needs_apply(&gateway));
    } else if(!strncmp(line,"enable ",7) || !strncmp(line,"disable ",8) ||
              !strncmp(line,"forget ",7) || !strncmp(line,"update-profile ",15)) {
        char request_line[OSG_GW_CONFIG_COMMAND_CAPACITY+128]; snprintf(request_line,sizeof request_line,"cockpit 0 %lu %s",(unsigned long)gateway.epoch,line);
        cockpit_command(request_line,now);
    } else if(!strcmp(line,"diagnostics")) {
        board_can_diagnostics_t can=board_can_diagnostics();
        append("can rx=%lu tx=%lu rxDrop=%lu txDrop=%lu txFailed=%lu queued=%u busOff=%u recovering=%u recoveries=%lu rxError=%u txError=%u halError=%08lx\r\n",
            (unsigned long)can.rx,(unsigned long)can.tx,(unsigned long)can.rx_dropped,(unsigned long)can.tx_dropped,
            (unsigned long)can.tx_failed,can.tx_queue,can.bus_off,can.recovering,(unsigned long)can.recoveries,can.rx_error,can.tx_error,(unsigned long)can.hal_error);
        append("protocol accepted=%lu rejected=%lu epoch=%lu leases=%lu registryGeneration=%lu running=%u applyRequired=%u\r\n",
            (unsigned long)gateway.accepted_frames,(unsigned long)gateway.rejected_frames,(unsigned long)gateway.epoch,
            (unsigned long)gateway.lease_counter,(unsigned long)gateway.pending.generation,gateway.running,osg_gateway_needs_apply(&gateway));
    } else append("ERR unknown command; type help\r\n");
}
static void hid_task(uint32_t now) {
    if(!gateway.running || hid_inflight || !tud_hid_ready()) return;
    for(unsigned tried=0;tried<OSG_GW_MAX_MODULES;tried++) {
        unsigned slot=next_slot; next_slot=(next_slot+1)%OSG_GW_MAX_MODULES;
        uint8_t report[OSG_GW_MAX_REPORT_SIZE];
        osg_gateway_delivery_t delivery;
        size_t length=osg_gateway_delivery_prepare(&gateway,slot,report,sizeof report,&delivery);
        if(!length) continue;
        if(sent_valid[slot] && !memcmp(report,sent[slot],length) && (uint32_t)(now-last_report[slot])<10U) continue;
        if(tud_hid_report(gateway.active.slots[slot].report_id,report,(uint16_t)length)) {
            memcpy(sent[slot],report,length); sent_valid[slot]=true; last_report[slot]=now; hid_inflight=true;
            inflight_delivery=delivery;
        }
        return;
    }
}
/* Keep boot restore buffers out of the event loop's permanent stack frame. */
static void initialize_gateway(void) __attribute__((noinline));
static void initialize_gateway(void) {
    uint8_t record[OSG_GW_REGISTRY_RECORD_SIZE]; size_t length=0; uint32_t epoch=0;
    osg_gateway_registry_t restored; const osg_gateway_registry_t *registry=NULL;
    if(board_registry_load(record,sizeof record,&length,&epoch)) {
        if(!osg_gateway_registry_decode(record,length,&restored)) board_fault();
        registry=&restored;
    }
    if(epoch>UINT32_MAX-0x10000U || !osg_gateway_init(&gateway,epoch+0x10000U,registry) || !save_registry(&gateway.pending) ||
       !gateway_usb_prepare(&gateway)) board_fault();
    char uid[33]; board_uid_hex(uid); memcpy(uid+24,"47575459",9);
    osg_gateway_identity_t unique_id;
    if(!osg_gateway_identity_parse(uid,&unique_id) ||
       !osg_cyphal_init(&cyphal,&unique_id,gateway.epoch,"org.opensimgear.gateway",can_send,NULL)) board_fault();
}
int main(void) {
    board_init(); initialize_gateway();
    const tusb_rhport_init_t usb={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_FULL};
    if(!tusb_init(0,&usb)) board_fault();
    tud_disconnect();
    bool attached=false,serial_open=false,active=false,was_can_fault=false;
    uint32_t previous_recoveries=0;
    char line_buffer[OSG_GW_CONFIG_COMMAND_CAPACITY+96]; osg_line_t line; osg_line_init(&line,line_buffer,sizeof line_buffer);
    for(;;) {
        uint32_t now=board_millis(); bool vbus=board_usb_present();
        osg_gateway_layout_event_t layout_event=osg_gateway_layout_service(&layout,&gateway,now,response_size==0 && !configuration.busy,layout_save,NULL);
        if(layout_event==OSG_GATEWAY_LAYOUT_DETACH) {
            tud_disconnect(); attached=false; active=false; invalidate_reports();
            osg_gateway_config_cancel(&configuration);
            (void)osg_gateway_activate_prepared(&gateway,&layout.target,true,&layout.activation);
            if(!gateway_usb_prepare(&gateway)) board_fault();
        }
        bool should_attach=vbus && !layout.detached;
        if(should_attach!=attached) {
            attached=should_attach; invalidate_reports();
            if(attached) tud_connect(); else tud_disconnect();
        }
        tud_task();
        /* USB completion callbacks may tick the core at a later millisecond. */
        now=board_millis();
        bool running=attached && tud_mounted() && !tud_suspended();
        if(running!=active || running!=gateway.running) {
            active=running; invalidate_reports(); osg_gateway_set_running(&gateway,running);
        }
        osg_gateway_tick(&gateway,now);
        board_can_task(now);
        board_can_diagnostics_t can=board_can_diagnostics();
        bool can_fault=can.bus_off || can.recovering;
        if((can_fault && !was_can_fault) || can.recoveries!=previous_recoveries) {
            osg_cyphal_reset(&cyphal); osg_gateway_bus_reset(&gateway); memset(sent_valid,0,sizeof sent_valid);
            osg_gateway_set_running(&gateway,running);
        }
        was_can_fault=can_fault; previous_recoveries=can.recoveries;
        for(unsigned budget=0;budget<4;budget++) {
            board_can_frame_t incoming; if(!board_can_receive(&incoming)) break;
            osg_gateway_frame_t frame={.id=incoming.id,.length=incoming.size,.extended=true,
                .fd=incoming.fd,.brs=incoming.brs};
            osg_gateway_message_t message,reply;
            memcpy(frame.data,incoming.data,incoming.size);
            uint8_t source=osg_cyphal_node_id(&cyphal);
            bool received=osg_cyphal_receive(&cyphal,&frame,now,&message);
            if(source!=osg_cyphal_node_id(&cyphal)) { migrate_can_source(now,running); break; }
            if(received && message.type==OSG_GW_CONFIG_RESPONSE) {
                (void)osg_gateway_config_receive(&configuration,&gateway,&message);
            } else if(received &&
               osg_gateway_receive(&gateway,&message,now,&reply)==OSG_GW_RX_ASSIGNMENT)
                (void)osg_cyphal_send(&cyphal,&reply,now);
        }
        if(!can_fault) {
            uint8_t source=osg_cyphal_node_id(&cyphal);
            osg_cyphal_tick(&cyphal,now);
            if(source!=osg_cyphal_node_id(&cyphal)) migrate_can_source(now,running);
        }
        osg_gateway_config_tick(&configuration,&gateway,&cyphal,now);
        const char *config_error,*config_json;
        uint32_t config_id=configuration.route.sequence;
        if(osg_gateway_config_result(&configuration,&config_error,&config_json)) {
            char uid[25]; board_uid_hex(uid);
            response_size=configuration.events ?
                osg_gateway_cockpit_events_response(&gateway,uid,config_id,config_error,config_json,response,sizeof response) : configuration.action ?
                osg_gateway_cockpit_action_response(&gateway,uid,config_id,config_error,config_json,response,sizeof response) :
                osg_gateway_cockpit_config_response(&gateway,uid,config_id,config_error,config_json,response,sizeof response);
            if(!response_size) append("{\"id\":%lu,\"ok\":false,\"error\":\"response_overflow\",\"state\":null}\r\n",(unsigned long)config_id);
        }
        bool connected=attached && tud_cdc_connected();
        if(connected!=serial_open) {
            serial_open=connected; osg_line_init(&line,line_buffer,sizeof line_buffer);
            response_size=response_offset=0;
            if(!connected) osg_gateway_config_cancel(&configuration);
            /* DTR and the first host write may arrive in the same USB task.
             * Preserve that request on open; discard old input only on close. */
            if(!connected) tud_cdc_read_flush();
            if(connected) { identity(); append("type help for commands\r\n"); }
        }
        if(connected && response_offset<response_size) {
            response_offset+=tud_cdc_write(response+response_offset,(uint32_t)(response_size-response_offset));
            tud_cdc_write_flush();
        }
        if(response_offset==response_size) response_offset=response_size=0;
        for(unsigned budget=0;connected && !layout.pending && !configuration.busy && !response_size && budget<32 && tud_cdc_available();budget++) {
            char byte; if(tud_cdc_read(&byte,1)!=1) break;
            osg_line_result_t result=osg_line_feed(&line,byte);
            if(result==OSG_LINE_READY) command(line_buffer,now);
            else if(result==OSG_LINE_TOO_LONG) append("ERR line too long\r\n");
        }
        hid_task(now);
        board_led(can_fault ? ((now/100U)&1U)!=0 : (active ? ((now/500U)&1U)!=0 : ((now/1000U)&1U)!=0));
    }
}
void tud_mount_cb(void) { invalidate_reports(); }
void tud_umount_cb(void) { invalidate_reports(); }
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; invalidate_reports(); }
void tud_resume_cb(void) { invalidate_reports(); }
void tud_hid_report_complete_cb(uint8_t instance,const uint8_t *report,uint16_t length) {
    if(instance) return;
    /* tud_task runs before the event loop's next tick. Enforce freshness at
     * actual completion time, including the exact timeout boundary. */
    osg_gateway_tick(&gateway,board_millis());
    osg_gateway_message_t ack;
    if(hid_inflight && board_usb_present() && tud_mounted() && !tud_suspended() &&
       osg_gateway_delivery_complete(&gateway,&inflight_delivery,report,length,&ack)) {
        (void)osg_cyphal_send(&cyphal,&ack,board_millis());
    }
    hid_inflight=false; inflight_delivery.valid=false;
}
void tud_hid_report_failed_cb(uint8_t instance,hid_report_type_t type,const uint8_t *report,uint16_t length) {
    (void)report; (void)length;
    if(!instance && type==HID_REPORT_TYPE_INPUT) {
        memset(sent_valid,0,sizeof sent_valid); hid_inflight=false; inflight_delivery.valid=false;
    }
}
uint16_t tud_hid_get_report_cb(uint8_t instance,uint8_t id,hid_report_type_t type,uint8_t *buffer,uint16_t length) {
    if(instance || type!=HID_REPORT_TYPE_INPUT || !buffer || !length) return 0;
    for(unsigned index=0;index<OSG_GW_MAX_MODULES;index++)
        if(gateway.active.slots[index].occupied && gateway.active.slots[index].enabled && gateway.active.slots[index].report_id==id)
            return (uint16_t)osg_gateway_report(&gateway,index,buffer,length);
    return 0;
}
void tud_hid_set_report_cb(uint8_t instance,uint8_t id,hid_report_type_t type,const uint8_t *buffer,uint16_t length) {
    (void)instance; (void)id; (void)type; (void)buffer; (void)length;
}
