#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "tusb.h"
#include "debug_gateway.h"
#include "debug_usb.h"
#include "pico_storage.h"
#include <stdio.h>
#include <string.h>

static osg_debug_gateway_t gateway;
static char input[OSG_DEBUG_LINE_CAPACITY],output[OSG_DEBUG_RESPONSE_CAPACITY];
static size_t input_used,output_used,output_sent;
static bool dropping_line,response_complete,hid_inflight;
static osg_gateway_delivery_t inflight_delivery;
static uint8_t sent[OSG_GW_MAX_MODULES][OSG_GW_MAX_REPORT_SIZE+1];
static bool sent_valid[OSG_GW_MAX_MODULES];
static uint32_t last_report[OSG_GW_MAX_MODULES];
static unsigned next_slot;
extern uint8_t __flash_binary_end;

static uint32_t milliseconds(void) { return to_ms_since_boot(get_absolute_time()); }
static void invalidate_reports(void) {
    memset(sent_valid,0,sizeof sent_valid);hid_inflight=false;inflight_delivery.valid=false;
}
typedef struct { size_t offset,size;const uint8_t *data; } flash_operation_t;
static void flash_erase(void *context) {
    const flash_operation_t *operation=context;
    flash_range_erase((uint32_t)operation->offset,operation->size);
}
static void flash_program(void *context) {
    const flash_operation_t *operation=context;
    flash_range_program((uint32_t)operation->offset,operation->data,operation->size);
}
static bool storage_erase(size_t offset,size_t size) {
    flash_operation_t operation={.offset=offset,.size=size};
    return flash_safe_execute(flash_erase,&operation,1000)==PICO_OK;
}
static bool storage_program(size_t offset,const uint8_t *data,size_t size) {
    flash_operation_t operation={.offset=offset,.size=size,.data=data};
    return flash_safe_execute(flash_program,&operation,1000)==PICO_OK;
}
static void initialize(void) __attribute__((noinline));
static void initialize(void) {
    char id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES*2+1],uid[25],serial[40];
    pico_get_unique_board_id_string(id,sizeof id);
    snprintf(uid,sizeof uid,"00000000%s",id);
    for(unsigned i=8;uid[i];i++) if(uid[i]>='A' && uid[i]<='F') uid[i]=(char)(uid[i]-'A'+'a');
    snprintf(serial,sizeof serial,"OSG-PICO-%s",id);
    osg_pico_flash_t flash={.mapped_flash=(const uint8_t *)XIP_BASE,.size=PICO_FLASH_SIZE_BYTES,
        .image_size=(uintptr_t)&__flash_binary_end-XIP_BASE,
        .erase=storage_erase,.program=storage_program};
    if(!osg_pico_storage_init(&flash)) panic("Debug journals overlap firmware");
    osg_journal_io_t journals[OSG_DEBUG_JOURNAL_COUNT];
    _Static_assert((int)OSG_DEBUG_JOURNAL_COUNT==(int)OSG_PICO_STORAGE_JOURNALS,"Storage count matches virtual devices");
    for(unsigned i=0;i<OSG_DEBUG_JOURNAL_COUNT;i++) journals[i]=osg_pico_storage_io(i);
    if(!osg_debug_init(&gateway,uid,serial,journals) ||
       !osg_debug_usb_prepare(&gateway.core,serial)) panic("Debug gateway initialization failed");
}
static void serial_task(void) {
    if(!tud_cdc_connected()) return;
    if(output_used) {
        if(output_sent<output_used) {
            output_sent+=tud_cdc_write(output+output_sent,(uint32_t)(output_used-output_sent));
            tud_cdc_write_flush();
        }
        if(output_sent!=output_used || !response_complete) return;
        output_used=output_sent=0;response_complete=false;
    }
    if(gateway.layout.detached) return;
    for(unsigned budget=0;budget<64 && tud_cdc_available();budget++) {
        uint8_t c;if(tud_cdc_read(&c,1)!=1) break;
        if(!c) { input_used=0;dropping_line=true;continue; }
        if(c=='\r') continue;
        if(c=='\n') {
            if(dropping_line) {
                const char error[]="ERR line too long\r\n";
                memcpy(output,error,sizeof error);output_used=sizeof error-1;
            } else if(input_used) {
                input[input_used]=0;
                output_used=osg_debug_command(&gateway,input,output,sizeof output);
            }
            input_used=0;dropping_line=false;output_sent=0;response_complete=false;
            if(output_used) return;
        } else if(!dropping_line) {
            if(input_used>=sizeof input-1) { input_used=0;dropping_line=true; }
            else input[input_used++]=(char)c;
        }
    }
}
static void hid_task(uint32_t now) {
    if(!tud_mounted() || tud_suspended() || gateway.layout.detached || hid_inflight || !tud_hid_ready()) return;
    for(unsigned tried=0;tried<OSG_GW_MAX_MODULES;tried++) {
        unsigned slot=next_slot;next_slot=(next_slot+1)%OSG_GW_MAX_MODULES;
        uint8_t report[OSG_GW_MAX_REPORT_SIZE+1];osg_gateway_delivery_t delivery;
        size_t size=osg_debug_report_prepare(&gateway,slot,report,sizeof report,&delivery);
        if(!size) continue;
        if(sent_valid[slot] && !memcmp(sent[slot],report,size) && (uint32_t)(now-last_report[slot])<10) continue;
        /* The shared debug core returns the complete immutable packet, ID first. */
        if(tud_hid_report(0,report,(uint16_t)size)) {
            memcpy(sent[slot],report,size);sent_valid[slot]=true;last_report[slot]=now;
            inflight_delivery=delivery;hid_inflight=true;
        }
        return;
    }
}
int main(void) {
    initialize();
    const tusb_rhport_init_t usb={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_FULL};
    if(!tusb_init(0,&usb)) panic("USB initialization failed");
    for(;;) {
        tud_task();uint32_t now=milliseconds();
        bool connected=tud_mounted() && !tud_suspended() && tud_cdc_connected();
        if(connected!=gateway.connected) {
            osg_debug_connected(&gateway,connected);invalidate_reports();
            if(!connected) { input_used=output_used=output_sent=0;dropping_line=false;response_complete=false;tud_cdc_read_flush(); }
        }
        osg_gateway_layout_event_t event=osg_debug_tick(&gateway,now,output_used==0);
        if(event==OSG_GATEWAY_LAYOUT_DETACH) {
            tud_disconnect();invalidate_reports();
            if(!osg_debug_layout_activate(&gateway) ||
               !osg_debug_usb_prepare(&gateway.core,gateway.serial)) panic("Debug layout publication failed");
        } else if(event==OSG_GATEWAY_LAYOUT_ATTACH) tud_connect();
        if(!tud_mounted() || tud_suspended() || gateway.layout.detached || !gateway.connected)
            osg_gateway_set_running(&gateway.core,false);
        serial_task();hid_task(now);tight_loop_contents();
    }
}
void tud_cdc_tx_complete_cb(uint8_t itf) {
    (void)itf;
    if(output_used && output_sent==output_used && tud_cdc_write_available()==CFG_TUD_CDC_TX_BUFSIZE)
        response_complete=true;
}
void tud_cdc_line_state_cb(uint8_t itf,bool dtr,bool rts) {
    (void)itf;(void)rts;
    if(!dtr) {
        osg_debug_connected(&gateway,false);invalidate_reports();
        input_used=output_used=output_sent=0;dropping_line=false;response_complete=false;
    }
}
void tud_mount_cb(void) { invalidate_reports(); }
void tud_umount_cb(void) { osg_debug_connected(&gateway,false);invalidate_reports(); }
void tud_suspend_cb(bool remote_wakeup_enabled) {
    (void)remote_wakeup_enabled;osg_debug_connected(&gateway,false);invalidate_reports();
}
void tud_resume_cb(void) { invalidate_reports(); }
void tud_hid_report_complete_cb(uint8_t instance,const uint8_t *report,uint16_t size) {
    if(instance) return;
    gateway.now=milliseconds();osg_gateway_tick(&gateway.core,gateway.now);
    if(hid_inflight && tud_mounted() && !tud_suspended() && gateway.connected)
        osg_debug_report_complete(&gateway,&inflight_delivery,report,size);
    hid_inflight=false;inflight_delivery.valid=false;
}
void tud_hid_report_failed_cb(uint8_t instance,hid_report_type_t type,const uint8_t *report,uint16_t size) {
    (void)report;(void)size;
    if(!instance && type==HID_REPORT_TYPE_INPUT) invalidate_reports();
}
uint16_t tud_hid_get_report_cb(uint8_t instance,uint8_t id,hid_report_type_t type,uint8_t *buffer,uint16_t size) {
    if(instance || type!=HID_REPORT_TYPE_INPUT || !buffer) return 0;
    for(unsigned slot=0;slot<OSG_GW_MAX_MODULES;slot++) {
        const osg_gateway_slot_t *entry=&gateway.core.active.slots[slot];
        if(entry->occupied && entry->enabled && entry->report_id==id)
            return (uint16_t)osg_gateway_report(&gateway.core,slot,buffer,size);
    }
    return 0;
}
void tud_hid_set_report_cb(uint8_t instance,uint8_t id,hid_report_type_t type,const uint8_t *buffer,uint16_t size) {
    (void)instance;(void)id;(void)type;(void)buffer;(void)size;
}
