#include "board.h"
#include "gateway_usb.h"
#include "opensimgear/gateway.h"
#include "opensimgear/line.h"
#include "tusb.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static osg_gateway_t gateway;
static uint8_t sent[OSG_GW_MAX_MODULES][OSG_GW_MAX_REPORT_SIZE];
static bool sent_valid[OSG_GW_MAX_MODULES], hid_inflight;
static uint32_t last_report[OSG_GW_MAX_MODULES];
static unsigned next_slot;
static char response[4096];
static size_t response_size,response_offset;
static bool apply_pending,layout_detached;
static uint32_t apply_at,reattach_at;

static void invalidate_reports(void) {
    memset(sent_valid,0,sizeof sent_valid); hid_inflight=false;
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
static bool save_registry(void) {
    uint8_t record[OSG_GW_REGISTRY_RECORD_SIZE];
    size_t size=osg_gateway_registry_encode(&gateway.pending,record,sizeof record);
    return size && board_registry_commit(record,size,gateway.epoch);
}
static void identity(void) {
    char uid[25]; board_uid_hex(uid);
    append("gatewayType=stm32-can-bringup protocol=1 board=gateway_g0b1cb_rev_a identity=%s epoch=%lu maxModules=%u\r\n",
        uid,(unsigned long)gateway.epoch,OSG_GW_MAX_MODULES);
}
static void command(const char *line,uint32_t now) {
    if(!strcmp(line,"hello") || !strcmp(line,"identity") || !strcmp(line,"info")) identity();
    else if(!strcmp(line,"help")) append("identity | inventory | registry | register <32hex> | unregister <32hex> | save | apply | diagnostics\r\n");
    else if(!strcmp(line,"inventory")) {
        identity();
        for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) {
            const osg_gateway_module_t *module=&gateway.modules[i]; if(!module->occupied) continue;
            char id[33]; osg_gateway_identity_format(&module->identity,id);
            append("module identity=%s node=%u online=%u directUsb=%u input=%u axes=%u buttons=%u revision=%u registered=%d active=%d lastSeenMs=%lu\r\n",
                id,module->node,module->online,module->direct_usb,module->has_input,module->profile.axis_count,
                module->profile.button_count,module->profile.revision,
                osg_gateway_find_slot(&gateway.pending,&module->identity),
                osg_gateway_find_slot(&gateway.active,&module->identity),(unsigned long)(now-module->last_seen));
        }
        append("OK inventory\r\n");
    } else if(!strcmp(line,"registry")) {
        for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) {
            const osg_gateway_slot_t *pending=&gateway.pending.slots[i],*active=&gateway.active.slots[i];
            char p[33]="-",a[33]="-";
            if(pending->occupied) osg_gateway_identity_format(&pending->identity,p);
            if(active->occupied) osg_gateway_identity_format(&active->identity,a);
            append("slot=%u reportId=%u pending=%s active=%s\r\n",i,i+1,p,a);
        }
        append("OK applyRequired=%u\r\n",osg_gateway_needs_apply(&gateway));
    } else if(!strncmp(line,"register ",9) || !strncmp(line,"unregister ",11)) {
        bool remove=line[0]=='u'; osg_gateway_identity_t id;
        if(!osg_gateway_identity_parse(line+(remove ? 11 : 9),&id)) append("ERR identity must be 32 hexadecimal digits\r\n");
        else {
            bool ok=remove ? osg_gateway_unregister(&gateway,&id) : osg_gateway_register(&gateway,&id);
            append(ok ? "OK pending registry changed; save persists, apply activates USB layout\r\n" :
                "ERR registration rejected (unknown, offline, profile, or capacity)\r\n");
        }
    } else if(!strcmp(line,"save")) append(save_registry() ? "OK registry saved; activation on apply or next boot\r\n" : "ERR flash save failed\r\n");
    else if(!strcmp(line,"apply")) {
        if(!osg_gateway_needs_apply(&gateway)) append("OK layout unchanged\r\n");
        else if(!save_registry()) append("ERR flash save failed; USB layout unchanged\r\n");
        else { append("OK registry saved; applying USB layout\r\n"); apply_pending=true; apply_at=now+100U; }
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
        size_t length=osg_gateway_report(&gateway,slot,report,sizeof report);
        if(!length) continue;
        if(sent_valid[slot] && !memcmp(report,sent[slot],length) && (uint32_t)(now-last_report[slot])<10U) continue;
        if(tud_hid_report((uint8_t)(slot+1),report,(uint16_t)length)) {
            memcpy(sent[slot],report,length); sent_valid[slot]=true; last_report[slot]=now; hid_inflight=true;
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
    if(epoch>UINT32_MAX-0x10000U || !osg_gateway_init(&gateway,epoch+0x10000U,registry) || !save_registry() ||
       !gateway_usb_prepare(&gateway)) board_fault();
}
int main(void) {
    board_init(); initialize_gateway();
    const tusb_rhport_init_t usb={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_FULL};
    if(!tusb_init(0,&usb)) board_fault();
    tud_disconnect();
    bool attached=false,serial_open=false,active=false,was_can_fault=false;
    uint32_t previous_recoveries=0;
    char line_buffer[80]; osg_line_t line; osg_line_init(&line,line_buffer,sizeof line_buffer);
    for(;;) {
        uint32_t now=board_millis(); bool vbus=board_usb_present();
        if(layout_detached && (int32_t)(now-reattach_at)>=0) layout_detached=false;
        bool should_attach=vbus && !layout_detached;
        if(should_attach!=attached) {
            attached=should_attach; invalidate_reports();
            if(attached) tud_connect(); else tud_disconnect();
        }
        tud_task();
        bool running=attached && tud_mounted() && !tud_suspended();
        if(running!=active || running!=gateway.running) {
            active=running; invalidate_reports(); osg_gateway_set_running(&gateway,running);
        }
        osg_gateway_tick(&gateway,now);
        board_can_task(now);
        board_can_diagnostics_t can=board_can_diagnostics();
        bool can_fault=can.bus_off || can.recovering;
        if((can_fault && !was_can_fault) || can.recoveries!=previous_recoveries) {
            osg_gateway_bus_reset(&gateway); memset(sent_valid,0,sizeof sent_valid);
            osg_gateway_set_running(&gateway,running);
        }
        was_can_fault=can_fault; previous_recoveries=can.recoveries;
        for(unsigned budget=0;budget<4;budget++) {
            board_can_frame_t incoming; if(!board_can_receive(&incoming)) break;
            osg_gateway_frame_t frame={.id=incoming.id,.length=incoming.size,.extended=true,.fd=true,.brs=true},reply;
            memcpy(frame.data,incoming.data,incoming.size);
            if(osg_gateway_receive(&gateway,&frame,now,&reply)==OSG_GW_RX_ASSIGNMENT) {
                board_can_frame_t outgoing={.id=reply.id,.size=reply.length};
                memcpy(outgoing.data,reply.data,reply.length); (void)board_can_enqueue(&outgoing);
            }
        }
        bool connected=attached && tud_cdc_connected();
        if(connected!=serial_open) {
            serial_open=connected; osg_line_init(&line,line_buffer,sizeof line_buffer);
            response_size=response_offset=0; tud_cdc_read_flush();
            if(connected) { identity(); append("type help for commands\r\n"); }
        }
        if(connected && response_offset<response_size) {
            response_offset+=tud_cdc_write(response+response_offset,(uint32_t)(response_size-response_offset));
            tud_cdc_write_flush();
        }
        if(response_offset==response_size) response_offset=response_size=0;
        for(unsigned budget=0;connected && !apply_pending && !response_size && budget<32 && tud_cdc_available();budget++) {
            char byte; if(tud_cdc_read(&byte,1)!=1) break;
            osg_line_result_t result=osg_line_feed(&line,byte);
            if(result==OSG_LINE_READY) command(line_buffer,now);
            else if(result==OSG_LINE_TOO_LONG) append("ERR line too long\r\n");
        }
        if(apply_pending && !response_size && (int32_t)(now-apply_at)>=0) {
            tud_disconnect(); attached=false; layout_detached=true; reattach_at=now+250U;
            active=false; invalidate_reports(); osg_gateway_apply_layout(&gateway);
            if(!gateway_usb_prepare(&gateway)) board_fault();
            apply_pending=false;
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
    (void)report; (void)length; if(!instance) hid_inflight=false;
}
void tud_hid_report_failed_cb(uint8_t instance,hid_report_type_t type,const uint8_t *report,uint16_t length) {
    (void)report; (void)length;
    if(!instance && type==HID_REPORT_TYPE_INPUT) { memset(sent_valid,0,sizeof sent_valid); hid_inflight=false; }
}
uint16_t tud_hid_get_report_cb(uint8_t instance,uint8_t id,hid_report_type_t type,uint8_t *buffer,uint16_t length) {
    if(instance || type!=HID_REPORT_TYPE_INPUT || !buffer || !length) return 0;
    if(id==11) {
        for(unsigned slot=0;slot<OSG_GW_MAX_MODULES;slot++) if(gateway.active.slots[slot].occupied) return 0;
        buffer[0]=0; return 1;
    }
    if(id<1 || id>OSG_GW_MAX_MODULES) return 0;
    return (uint16_t)osg_gateway_report(&gateway,id-1,buffer,length);
}
void tud_hid_set_report_cb(uint8_t instance,uint8_t id,hid_report_type_t type,const uint8_t *buffer,uint16_t length) {
    (void)instance; (void)id; (void)type; (void)buffer; (void)length;
}
