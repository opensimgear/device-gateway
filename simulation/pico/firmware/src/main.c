#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "tusb.h"
#include "gateway.h"
#include "usb_layout.h"

static osg_gateway_t gateway;
static osg_usb_layout_t usb;
static char input[OSG_LINE_CAPACITY+1], output[OSG_RESPONSE_CAPACITY];
static size_t input_used, output_used, output_sent;
static bool dropping_line, response_tx_complete;
static unsigned reconnect_phase;
static uint32_t reconnect_time, report_time;
static uint8_t previous[OSG_MODULE_COUNT][OSG_MAX_REPORT_SIZE];
static bool previous_valid[OSG_MODULE_COUNT];
static uint16_t idle_ms[OSG_MODULE_COUNT];
static uint32_t last_report[OSG_MODULE_COUNT];
static const tusb_desc_device_t device={.bLength=sizeof(tusb_desc_device_t),.bDescriptorType=TUSB_DESC_DEVICE,.bcdUSB=0x0200,.bDeviceClass=TUSB_CLASS_MISC,.bDeviceSubClass=MISC_SUBCLASS_COMMON,.bDeviceProtocol=MISC_PROTOCOL_IAD,.bMaxPacketSize0=64,.idVendor=OSG_USB_VID,.idProduct=OSG_USB_PID,.bcdDevice=0x0100,.iManufacturer=1,.iProduct=2,.iSerialNumber=3,.bNumConfigurations=1};
uint8_t const *tud_descriptor_device_cb(void){return (const uint8_t *)&device;}
uint8_t const *tud_descriptor_configuration_cb(uint8_t index){(void)index;return usb.configuration;}
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance){return instance<gateway.layout_count?usb.reports[instance]:NULL;}
uint16_t const *tud_descriptor_string_cb(uint8_t index,uint16_t langid){
  (void)langid;static uint16_t text[OSG_USB_STRING_CAPACITY];
  return osg_usb_string_descriptor(&gateway,index,text,OSG_USB_STRING_CAPACITY)?text:NULL;
}
uint16_t tud_hid_get_report_cb(uint8_t instance,uint8_t report_id,hid_report_type_t report_type,uint8_t *buffer,uint16_t requested){
  (void)report_id;if(report_type!=HID_REPORT_TYPE_INPUT)return 0;uint8_t report[OSG_MAX_REPORT_SIZE];size_t n=osg_gateway_report(&gateway,instance,report);if(n>requested)n=requested;memcpy(buffer,report,n);return (uint16_t)n;
}
void tud_hid_set_report_cb(uint8_t instance,uint8_t report_id,hid_report_type_t type,uint8_t const *buffer,uint16_t size){(void)instance;(void)report_id;(void)type;(void)buffer;(void)size;}
bool tud_hid_set_idle_cb(uint8_t instance,uint8_t rate){if(instance>=gateway.layout_count)return false;idle_ms[instance]=(uint16_t)rate*4;return true;}
void tud_mount_cb(void){memset(previous_valid,0,sizeof previous_valid);}
void tud_suspend_cb(bool remote_wakeup_enabled){(void)remote_wakeup_enabled;osg_gateway_disconnect(&gateway);}
void tud_umount_cb(void){osg_gateway_disconnect(&gateway);input_used=0;dropping_line=false;memset(previous_valid,0,sizeof previous_valid);}
void tud_cdc_tx_complete_cb(uint8_t itf){(void)itf;if(output_used&&output_sent==output_used&&tud_cdc_write_available()==CFG_TUD_CDC_TX_BUFSIZE)response_tx_complete=true;}
void tud_cdc_line_state_cb(uint8_t itf,bool dtr,bool rts){(void)itf;(void)rts;if(!dtr){if(reconnect_phase==1&&!response_tx_complete){reconnect_phase=0;gateway.reconnecting=false;}osg_gateway_disconnect(&gateway);input_used=0;dropping_line=false;output_used=output_sent=0;}}
static uint32_t milliseconds(void){return to_ms_since_boot(get_absolute_time());}
static void serial_task(uint32_t now){
  if(!tud_cdc_connected())return;
  if(output_sent<output_used){output_sent+=tud_cdc_write(output+output_sent,(uint32_t)(output_used-output_sent));tud_cdc_write_flush();return;}
  if(reconnect_phase)return;
  if(output_used&&!response_tx_complete)return;
  output_used=output_sent=0;
  while(tud_cdc_available()){
    uint8_t c;if(tud_cdc_read(&c,1)!=1)break;
    if(c=='\n'){
      output[0]=0;
      if(dropping_line){osg_gateway_error(&gateway,"Request exceeds 4096 bytes.",output,sizeof output);}
      else if(input_used){input[input_used]=0;osg_gateway_command(&gateway,input,input_used,now,output,sizeof output);}
      input_used=0;dropping_line=false;output_used=strlen(output);response_tx_complete=false;
      if(gateway.reconnecting){reconnect_phase=1;reconnect_time=now;}
      if(output_used)return;
    }else if(!dropping_line){if(input_used>=OSG_LINE_CAPACITY){dropping_line=true;input_used=0;}else input[input_used++]=(char)c;}
  }
}
static void hid_task(uint32_t now){
  if(!tud_mounted()||(uint32_t)(now-report_time)<8)return;
  report_time=now;
  for(unsigned i=0;i<gateway.layout_count;i++){
    uint8_t report[OSG_MAX_REPORT_SIZE];size_t n=osg_gateway_report(&gateway,i,report);
    if(tud_hid_n_ready((uint8_t)i)&&(!previous_valid[i]||memcmp(report,previous[i],n)||(idle_ms[i]&&(uint32_t)(now-last_report[i])>=idle_ms[i]))){if(tud_hid_n_report((uint8_t)i,0,report,(uint16_t)n)){memcpy(previous[i],report,n);previous_valid[i]=true;last_report[i]=now;}}
  }
}
int main(void){
  char board_id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES*2+1],serial[40];pico_get_unique_board_id_string(board_id,sizeof board_id);snprintf(serial,sizeof serial,"OSG-PICO-%s",board_id);
  osg_gateway_init(&gateway,serial);osg_usb_layout_build(&usb,&gateway);
  tusb_rhport_init_t init={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_FULL};tusb_init(0,&init);
  for(;;){
    tud_task();uint32_t now=milliseconds();osg_gateway_tick(&gateway,now,tud_mounted(),tud_cdc_connected());serial_task(now);hid_task(now);
    /* The ACK travels over the current CDC connection before descriptor changes.
       Detached descriptors remain immutable until the host sees a new connection. */
    if(reconnect_phase==1&&response_tx_complete&&(uint32_t)(now-reconnect_time)>=150){tud_disconnect();reconnect_phase=2;reconnect_time=now;}
    else if(reconnect_phase==2&&(uint32_t)(now-reconnect_time)>=300){osg_gateway_commit_layout(&gateway);osg_usb_layout_build(&usb,&gateway);memset(previous_valid,0,sizeof previous_valid);memset(idle_ms,0,sizeof idle_ms);input_used=output_used=output_sent=0;dropping_line=false;response_tx_complete=false;reconnect_phase=0;tud_connect();}
    tight_loop_contents();
  }
}
