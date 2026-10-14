#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gateway.h"
#include "usb_layout.h"
static osg_gateway_t g;
static char response[OSG_RESPONSE_CAPACITY];
static uint32_t now;
static bool command(const char *s){return osg_gateway_command(&g,s,strlen(s),now,response,sizeof response);}
static void tick(uint32_t advance){now+=advance;osg_gateway_tick(&g,now,true,true);}
static void reset(void){now=0;osg_gateway_init(&g,"OSG-PICO-0123456789ABCDEF");osg_gateway_tick(&g,now,true,true);}
static void check_report(unsigned slot,const uint8_t *expected,size_t size){uint8_t report[OSG_MAX_REPORT_SIZE];assert(osg_gateway_report(&g,slot,report)==size);assert(memcmp(report,expected,size)==0);}
static void start(void){assert(command("{\"id\":\"start\",\"command\":\"start\"}"));assert(g.running);}
static void check_usb_ascii(uint8_t index,const char *expected){
  uint16_t descriptor[OSG_USB_STRING_CAPACITY];size_t n=osg_usb_string_descriptor(&g,index,descriptor,OSG_USB_STRING_CAPACITY);
  assert(n==strlen(expected)+1&&descriptor[0]==((3u<<8)|(2*n)));
  for(size_t i=0;expected[i];i++)assert(descriptor[i+1]==(unsigned char)expected[i]);
}
static void usb_names(void){
  reset();const char *names[]={"OSG Sequential shifter","OSG Pedal set","OSG Button panel","OSG Handbrake"};
  for(unsigned i=0;i<4;i++){assert(!strcmp(g.modules[i].name,names[i]));assert(!strcmp(g.modules[i].label,names[i]));check_usb_ascii((uint8_t)(5+i),names[i]);}
  check_usb_ascii(2,"OpenSimGear Pico Gateway");uint16_t descriptor[OSG_USB_STRING_CAPACITY];
  assert(osg_usb_string_descriptor(&g,0,descriptor,OSG_USB_STRING_CAPACITY)==2&&descriptor[0]==0x0304&&descriptor[1]==0x0409);
  assert(!osg_usb_string_descriptor(&g,9,descriptor,OSG_USB_STRING_CAPACITY));
  start();assert(command("{\"id\":\"hold\",\"command\":\"input\",\"moduleId\":\"OSG-SQ-0042\",\"buttons\":[1]}"));tick(10);
  assert(command("{\"id\":\"rename\",\"command\":\"configure\",\"moduleId\":\"OSG-SQ-0042\",\"settings\":{\"label\":\"  OSG Gear \\u00e9 \\ud83c\\udfce  \",\"debounce\":0,\"inverted\":false}}"));
  assert(osg_gateway_needs_reconnect(&g)&&strstr(response,"\"needsReconnect\":true"));assert(!strcmp(g.modules[0].label,"OSG Gear é 🏎"));assert(!strcmp(g.modules[0].name,names[0]));
  check_usb_ascii(5,names[0]);const uint8_t one[]={1};check_report(0,one,1);
  /* Restoring the published name needs no re-enumeration; other settings are live. */
  assert(command("{\"id\":\"undo\",\"command\":\"configure\",\"moduleId\":\"OSG-SQ-0042\",\"settings\":{\"label\":\"OSG Sequential shifter\",\"debounce\":0,\"inverted\":true}}"));assert(!osg_gateway_needs_reconnect(&g));
  assert(command("{\"id\":\"rename\",\"command\":\"configure\",\"moduleId\":\"OSG-SQ-0042\",\"settings\":{\"label\":\"OSG Gear \\u00e9 \\ud83c\\udfce\",\"debounce\":0,\"inverted\":false}}"));
  assert(command("{\"id\":\"reconnect\",\"command\":\"reconnect\"}"));check_usb_ascii(5,names[0]);osg_gateway_commit_layout(&g);assert(!osg_gateway_needs_reconnect(&g));const uint8_t zero[]={0};check_report(0,zero,1);
  const uint16_t name[]={0x031c,'O','S','G',' ','G','e','a','r',' ',0x00e9,' ',0xd83c,0xdfce};
  assert(osg_usb_string_descriptor(&g,5,descriptor,OSG_USB_STRING_CAPACITY)==sizeof name/sizeof name[0]);assert(!memcmp(descriptor,name,sizeof name));
  /* Removal keeps the old name until its published USB interface is detached. */
  assert(command("{\"id\":\"remove\",\"command\":\"remove\",\"moduleId\":\"OSG-SQ-0042\"}"));assert(osg_usb_string_descriptor(&g,5,descriptor,OSG_USB_STRING_CAPACITY)==sizeof name/sizeof name[0]);assert(!memcmp(descriptor,name,sizeof name));osg_gateway_commit_layout(&g);check_usb_ascii(5,names[1]);assert(!osg_usb_string_descriptor(&g,8,descriptor,OSG_USB_STRING_CAPACITY));
  /* USB limits count UTF-16 units, and never split an emoji's surrogate pair. */
  for(unsigned i=0;i<80;i++) { memcpy(g.layout[0].label+4*i,"🏎",4); }
  g.layout[0].label[320]=0;
  assert(osg_usb_string_descriptor(&g,5,descriptor,OSG_USB_STRING_CAPACITY)==127&&descriptor[0]==0x03fe&&descriptor[125]==0xd83c&&descriptor[126]==0xdfce);
  memset(g.layout[0].label,'x',125);memcpy(g.layout[0].label+125,"🏎",5);
  assert(osg_usb_string_descriptor(&g,5,descriptor,OSG_USB_STRING_CAPACITY)==126&&descriptor[0]==0x03fc&&descriptor[125]=='x');
  assert(osg_usb_string_descriptor(&g,5,descriptor,2)==2&&descriptor[1]=='x');assert(!osg_usb_string_descriptor(&g,5,descriptor,1));
  memcpy(g.layout[0].label,"\xf0\x80\x80\x80",5);assert(!osg_usb_string_descriptor(&g,5,descriptor,OSG_USB_STRING_CAPACITY));
}
static void protocol_and_validation(void){
  reset();assert(command("{\"id\":\"hello\",\"command\":\"hello\"}"));assert(strstr(response,"\"backend\":\"pico\""));assert(strstr(response,"\"hidInterface\":2"));assert(strstr(response,"\"usbSerialNumber\":\"OSG-PICO-"));assert(!g.running);
  assert(!command("{\"id\":\"x\",\"command\":\"input\",\"moduleId\":\"OSG-SQ-0042\",\"buttons\":[1]}"));start();
  assert(command("{\"id\":\"x\",\"command\":\"input\",\"moduleId\":\"OSG-PD-0018\",\"axes\":[0,32768,65535]}"));const uint8_t pedals[]={0,0,0,128,255,255};check_report(1,pedals,6);
  osg_gateway_t saved=g;
  assert(!command("{\"id\":\"x\",\"command\":\"input\",\"moduleId\":\"OSG-SQ-0042\",\"buttons\":[1,1]}"));assert(!memcmp(&saved,&g,sizeof g));
  assert(!command("{\"id\":\"x\",\"command\":\"input\",\"moduleId\":\"OSG-PD-0018\",\"axes\":[0,3,65536]}"));assert(!memcmp(&saved,&g,sizeof g));
  assert(!command("{\"id\":\"x\",\"command\":\"configure\",\"moduleId\":\"OSG-SQ-0042\",\"settings\":{\"label\":\"ok\",\"debounce\":101,\"inverted\":false}}"));assert(!memcmp(&saved,&g,sizeof g));
  assert(!command("{\"id\":\"x\",\"command\":\"input\",\"moduleId\":\"OSG-SQ-0042\",\"buttons\":[],\"buttons\":[1]}"));assert(!memcmp(&saved,&g,sizeof g));
  assert(!command("{\"id\":\"x\",\"command\":\"start\",}"));assert(!command("{\"id\":\"x\",\"command\":\"status\"} garbage"));
  assert(!command("{\"id\":\"x\",\"command\":\"configure\",\"moduleId\":\"OSG-SQ-0042\",\"settings\":{\"label\":\"bad\\u0000\",\"debounce\":10,\"inverted\":false}}"));
  assert(command("{\"id\":\"escaped\\\"id\",\"command\":\"configure\",\"moduleId\":\"OSG-SQ-0042\",\"settings\":{\"label\":\"\\u00e9 \\\"gear\\\" \\ud83c\\udfce\",\"debounce\":10,\"inverted\":true}}"));assert(strstr(response,"é \\\"gear\\\" 🏎"));assert(strstr(response,"escaped\\\"id"));
}
static void input_safety(void){
  reset();start();const uint8_t zero[]={0};
  assert(command("{\"id\":\"a\",\"command\":\"input\",\"moduleId\":\"OSG-SQ-0042\",\"buttons\":[1]}"));check_report(0,zero,1);tick(9);check_report(0,zero,1);tick(1);const uint8_t one[]={1};check_report(0,one,1);
  assert(command("{\"id\":\"b\",\"command\":\"input\",\"moduleId\":\"OSG-SQ-0042\",\"buttons\":[]}"));tick(4);assert(command("{\"id\":\"c\",\"command\":\"input\",\"moduleId\":\"OSG-SQ-0042\",\"buttons\":[3]}"));tick(6);check_report(0,one,1);tick(4);const uint8_t three[]={4};check_report(0,three,1);
  assert(command("{\"id\":\"i\",\"command\":\"configure\",\"moduleId\":\"OSG-SQ-0042\",\"settings\":{\"label\":\"Shift\",\"debounce\":0,\"inverted\":true}}"));check_report(0,one,1);
  assert(command("{\"id\":\"p\",\"command\":\"presence\",\"moduleId\":\"OSG-SQ-0042\",\"online\":false}"));check_report(0,zero,1);assert(!command("{\"id\":\"p\",\"command\":\"input\",\"moduleId\":\"OSG-SQ-0042\",\"buttons\":[1]}"));
  assert(command("{\"id\":\"i\",\"command\":\"configure\",\"moduleId\":\"OSG-HB-0023\",\"settings\":{\"label\":\"Brake\",\"debounce\":0,\"inverted\":true}}"));const uint8_t axis_zero[]={0,0};check_report(3,axis_zero,2);
  assert(command("{\"id\":\"p\",\"command\":\"presence\",\"moduleId\":\"OSG-HB-0023\",\"online\":true}"));assert(command("{\"id\":\"p\",\"command\":\"input\",\"moduleId\":\"OSG-HB-0023\",\"axes\":[32768]}"));const uint8_t inverted[]={255,127};check_report(3,inverted,2);
  tick(OSG_WATCHDOG_MS-1);assert(g.running);tick(1);assert(!g.running);check_report(3,axis_zero,2);start();check_report(3,axis_zero,2);osg_gateway_tick(&g,now,true,false);assert(!g.running);check_report(3,axis_zero,2);
  reset();now=UINT32_MAX-5;osg_gateway_tick(&g,now,true,true);start();assert(command("{\"id\":\"a\",\"command\":\"input\",\"moduleId\":\"OSG-SQ-0042\",\"buttons\":[1]}"));tick(10);check_report(0,one,1);tick(OSG_WATCHDOG_MS);assert(!g.running);
}
static void dynamic_layout(void){
  reset();start();assert(command("{\"id\":\"add\",\"command\":\"add\",\"module\":{\"id\":\"CUSTOM_WHEEL\",\"name\":\"Wheel\",\"kind\":\"wheel\",\"buttonCount\":32,\"axisCount\":6}}"));assert(g.module_count==5&&g.layout_count==4&&!g.modules[4].registered&&osg_gateway_needs_reconnect(&g));assert(strstr(response,"\"hidInterface\":null"));
  assert(!command("{\"id\":\"a\",\"command\":\"input\",\"moduleId\":\"CUSTOM_WHEEL\",\"buttons\":[32],\"axes\":[1,2,3,4,5,6]}"));
  assert(command("{\"id\":\"r\",\"command\":\"reconnect\"}"));assert(g.reconnecting&&!g.running);assert(strstr(response,"\"reconnecting\":true"));osg_gateway_commit_layout(&g);assert(g.layout_count==5&&!osg_gateway_needs_reconnect(&g));start();
  assert(command("{\"id\":\"a\",\"command\":\"configure\",\"moduleId\":\"CUSTOM_WHEEL\",\"settings\":{\"label\":\"Wheel\",\"debounce\":0,\"inverted\":false}}"));
  assert(command("{\"id\":\"a\",\"command\":\"input\",\"moduleId\":\"CUSTOM_WHEEL\",\"buttons\":[1,32],\"axes\":[1,2,3,4,5,65535]}"));const uint8_t max[]={1,0,0,128,1,0,2,0,3,0,4,0,5,0,255,255};check_report(4,max,16);
  assert(command("{\"id\":\"del\",\"command\":\"remove\",\"moduleId\":\"CUSTOM_WHEEL\"}"));assert(g.module_count==4&&g.layout_count==5&&osg_gateway_needs_reconnect(&g));const uint8_t neutral[16]={0};check_report(4,neutral,16);
  assert(!command("{\"id\":\"add\",\"command\":\"add\",\"module\":{\"id\":\"CUSTOM_WHEEL\",\"name\":\"Wheel\",\"kind\":\"wheel\",\"buttonCount\":1,\"axisCount\":0}}"));
  for(unsigned i=0;i<4;i++){char line[128];snprintf(line,sizeof line,"{\"id\":\"del\",\"command\":\"remove\",\"moduleId\":\"%s\"}",g.modules[0].id);assert(command(line));}assert(g.module_count==0);osg_gateway_commit_layout(&g);assert(g.layout_count==0&&!osg_gateway_needs_reconnect(&g));
  for(unsigned i=0;i<8;i++){char line[256];snprintf(line,sizeof line,"{\"id\":\"add\",\"command\":\"add\",\"module\":{\"id\":\"PANEL_%u\",\"name\":\"Panel %u\",\"kind\":\"panel\",\"buttonCount\":32,\"axisCount\":6}}",i,i);assert(command(line));}assert(g.module_count==8);assert(!command("{\"id\":\"r\",\"command\":\"register\"}"));osg_gateway_commit_layout(&g);
}
static void maximum_snapshot(void){
  assert(g.module_count==8);
  for(unsigned i=0;i<8;i++){osg_module_t *m=&g.modules[i];for(unsigned c=0;c<80;c++){memcpy(m->name+c*4,"🏎",4);memcpy(m->label+c*4,"🏎",4);}m->name[320]=m->label[320]=0;m->buttons=UINT32_MAX;for(unsigned a=0;a<6;a++)m->axes[a]=65535;}
  assert(command("{\"id\":\"maximum\",\"command\":\"status\"}"));assert(strlen(response)>8000);assert(response[strlen(response)-1]=='\n');assert(strstr(response,"\"ok\":true"));
}
static void usb_descriptors(void){
  osg_usb_layout_t usb;
  osg_usb_layout_build(&usb,&g);assert(g.layout_count==8);assert(usb.configuration_length==9+66+25*8);assert(usb.configuration[4]==10);
  unsigned p=0,in_endpoints=0,out_endpoints=0,interfaces=0,hids=0;uint32_t endpoint_mask=0;
  while(p<usb.configuration_length){unsigned length=usb.configuration[p],type=usb.configuration[p+1];assert(length>=2&&p+length<=usb.configuration_length);if(type==4){interfaces++;assert(usb.configuration[p+2]==interfaces-1);}if(type==0x21){unsigned advertised=usb.configuration[p+7]|(usb.configuration[p+8]<<8);assert(advertised==usb.report_lengths[hids++]);}if(type==5){uint8_t address=usb.configuration[p+2];unsigned bit=(address&15)+((address&128)?16:0);assert(!(endpoint_mask&((uint32_t)1<<bit)));endpoint_mask|=(uint32_t)1<<bit;if(address&128)in_endpoints++;else out_endpoints++;}p+=length;}
  assert(interfaces==10&&hids==8&&in_endpoints==10&&out_endpoints==1);
  const uint8_t shifter[]={0x05,0x01,0x09,0x04,0xa1,0x01,0x05,0x09,0x19,0x01,0x29,0x03,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x03,0x81,0x02,0x75,0x01,0x95,0x05,0x81,0x03,0xc0};uint8_t report[96];assert(osg_hid_descriptor(3,0,report)==sizeof shifter);assert(!memcmp(report,shifter,sizeof shifter));
  g.module_count=0;osg_gateway_commit_layout(&g);osg_usb_layout_build(&usb,&g);assert(usb.configuration_length==75&&usb.configuration[4]==2);
}
int main(void){protocol_and_validation();usb_names();input_safety();dynamic_layout();maximum_snapshot();usb_descriptors();puts("Pico protocol, lifecycle, report and USB descriptor tests passed.");}
