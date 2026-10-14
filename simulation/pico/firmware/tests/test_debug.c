#include "debug_gateway.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static osg_debug_gateway_t gateway,restored;
static uint8_t flash[OSG_DEBUG_JOURNAL_COUNT][2][OSG_JOURNAL_PAGE_SIZE];
static char output[OSG_DEBUG_RESPONSE_CAPACITY];
static uint32_t clock_ms;
static bool fail_flash;
static bool fail_gateway;
static const uint8_t *read_page(void *context,unsigned page) { return page<2?((uint8_t (*)[OSG_JOURNAL_PAGE_SIZE])context)[page]:NULL; }
static bool erase_page(void *context,unsigned page) { if(page>1 || fail_flash || (fail_gateway && context==flash[0])) return false;memset(((uint8_t (*)[OSG_JOURNAL_PAGE_SIZE])context)[page],255,OSG_JOURNAL_PAGE_SIZE);return true; }
static bool program(void *context,unsigned page,unsigned offset,const uint8_t bytes[8]) {
    if(page>1 || offset%8 || offset+8>OSG_JOURNAL_PAGE_SIZE || fail_flash) return false;
    uint8_t *out=((uint8_t (*)[OSG_JOURNAL_PAGE_SIZE])context)[page]+offset;
    for(unsigned i=0;i<8;i++) { if((out[i]&bytes[i])!=bytes[i]) return false;out[i]&=bytes[i]; }return true;
}
static void advance(unsigned ms) {
    for(unsigned t=0;t<ms;t++) {
        osg_gateway_layout_event_t event=osg_debug_tick(&gateway,++clock_ms,true);
        if(event==OSG_GATEWAY_LAYOUT_DETACH) assert(osg_debug_layout_activate(&gateway));
        for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) {
            uint8_t report[64];osg_gateway_delivery_t delivery;size_t n=osg_debug_report_prepare(&gateway,i,report,sizeof report,&delivery);
            if(n) osg_debug_report_complete(&gateway,&delivery,report,n);
        }
    }
}
static const char *request(const char *line) {
    assert(osg_debug_command(&gateway,line,output,sizeof output));return output;
}
static void debug(unsigned slot,const char *op,int a,int b,unsigned count) {
    char line[256],identity[33];osg_virtual_device_t *d=&gateway.devices[slot];osg_gateway_identity_format(&d->identity,identity);
    int n=snprintf(line,sizeof line,"debug 1 %u %s %u %s",gateway.core.epoch,identity,d->boot,op);assert(n>0);
    if(count) n+=snprintf(line+n,sizeof line-(size_t)n," %d",a);
    if(count==2) snprintf(line+n,sizeof line-(size_t)n," %d",b);
    assert(strstr(request(line),"\"ok\":true"));
}
static void add(const char *preset) {
    char line[128];snprintf(line,sizeof line,"debug 1 %u - 0 add %s",gateway.core.epoch,preset);
    const char *reply=request(line);if(!strstr(reply,"\"ok\":true")) fprintf(stderr,"Add %s: %s\n",preset,reply);
    assert(strstr(reply,"\"ok\":true"));advance(700);
}
static void child(unsigned slot,const char *type,const char *command) {
    char line[600],identity[33];osg_virtual_device_t *d=&gateway.devices[slot];osg_gateway_identity_format(&d->identity,identity);
    snprintf(line,sizeof line,"cockpit 2 %u %s %s %u %s",gateway.core.epoch,type,identity,d->boot,command);
    assert(strstr(request(line),"\"ok\":true"));
}
static void rejects(const char *command,const char *error) { assert(strstr(request(command),error)); }
int main(void) {
    osg_journal_io_t journals[OSG_DEBUG_JOURNAL_COUNT];memset(flash,255,sizeof flash);
    for(unsigned i=0;i<OSG_DEBUG_JOURNAL_COUNT;i++) journals[i]=(osg_journal_io_t){flash[i],read_page,erase_page,program};
    assert(!osg_debug_init(NULL,"000000000123456789abcdef","OSG-PICO-0123456789abcdef",NULL));
    assert(!osg_debug_init(&gateway,"bad","bad",NULL));
    assert(!osg_debug_init(&gateway,"000000000123456789abcdeg","OSG-PICO-0123456789abcdeg",NULL));
    assert(osg_debug_init(&gateway,"000000000123456789abcdef","OSG-PICO-0123456789abcdef",journals));
    osg_debug_connected(&gateway,true);advance(800);
    assert(gateway.core.running && gateway.core.active.slots[3].enabled);
    assert(strstr(request("hello"),"pico-debug-gateway protocol=3"));
    assert(strstr(request("identity"),"OSG-PICO-0123456789abcdef"));assert(strstr(request("info"),"maxVirtual=8"));
    assert(strstr(request("cockpit 1 snapshot"),"\"firmware\":\"0.1.0\""));
    assert(!osg_debug_command(&gateway,"hello",output,2));
    rejects("nonsense","invalid_request");rejects("debug 1 9 - 0 describe","stale_epoch");
    rejects("debug 1 65536 - 0 add invalid","invalid_preset");rejects("debug 1 65536 - 0 describe extra","invalid_request");
    assert(strstr(request("debug 1 65536 - 0 describe"),"\"definition\":"));
    debug(0,"contact",0,1,2);advance(30);
    int index=osg_gateway_find_module(&gateway.core,&gateway.devices[0].identity);assert(index>=0 && gateway.core.modules[index].report[0]&1);
    debug(0,"contact",0,0,2);advance(30);assert(!(gateway.core.modules[index].report[0]&1));
    debug(1,"axis",0,2048,2);advance(20);assert(gateway.devices[1].events.axes[0]>32000 && gateway.devices[1].events.axes[0]<34000);
    debug(1,"neutral",0,0,0);advance(20);assert(!gateway.devices[1].sample.axes[0]);
    add("master-caution");add("radio");add("all-inputs");
    assert(gateway.devices[6].runtime.layout.buttons==128 && gateway.devices[6].runtime.layout.axes==8 && gateway.devices[6].runtime.layout.hats==4);
    debug(6,"contact",142,1,2);debug(6,"hat",0,1,2);debug(6,"selector",0,2,2);advance(30);
    debug(6,"expansion",7,0,2);advance(120);assert(!(gateway.devices[6].sample.expansion_available&128));
    debug(6,"expansion",7,1,2);debug(6,"neutral",0,0,0);advance(30);
    /* Detent signs remain ordered even while multiple UI commands queue. */
    uint32_t head=gateway.devices[5].events.head;
    debug(5,"turn",0,2,2);debug(5,"turn",0,-1,2);advance(220);
    assert(gateway.devices[5].events.head>=head+6);assert(!gateway.devices[5].turn_count[0]);
    assert(!osg_encoder_output(&gateway.devices[5].acquisition.encoders[0],clock_ms));
    assert(!gateway.devices[5].acquisition.encoders[0].count && !(gateway.devices[5].report[0]&0x18));
    head=gateway.devices[5].events.head;debug(5,"turn",0,1,2);
    while(gateway.devices[5].turn_phase[0]!=3) advance(1);
    debug(5,"neutral",0,0,0);advance(20);assert(gateway.devices[5].events.head==head);
    child(5,"events","state");assert(strstr(output,"\"contactButtonCount\":3"));
    child(5,"events","read 1 0");assert(strstr(output,"targetEvents"));
    child(5,"action","describe 0");assert(strstr(output,"display.frequency.set_text"));
    child(5,"action","invoke 1 display.frequency.set_text 3132332e343530");
    assert(!strcmp(gateway.devices[5].displays[0].text,"123.450"));
    child(5,"action","invoke 1 led.status.set_mode on");assert(gateway.devices[5].actions.status_mode==OSG_LED_ON);
    child(5,"action","state");assert(strstr(output,"123.450"));
    child(6,"action","invoke 1 led.rgb.set_color ff0000");assert(gateway.devices[6].actions.rgb==0xff0000);
    child(6,"action","invoke 1 led.boolean.set_enabled 1");assert(gateway.devices[6].actions.enabled);
    child(5,"config","get");child(5,"config","claim 17");child(5,"config","axis 17 0 0 48 100 3900 0 1 100");
    child(5,"config","apply_save 17");advance(700);assert(gateway.devices[5].runtime.active.axis_options[0].inverted);
    debug(5,"neutral",0,0,0);advance(30);assert(gateway.devices[5].sample.axes[0]==3900 && gateway.devices[5].events.axes[0]==0);
    child(1,"config","claim 18");child(1,"config","axis 18 0 0 48 0 4095 32768 1 100");
    child(1,"config","apply_save 18");advance(700);debug(1,"neutral",0,0,0);advance(30);assert(gateway.devices[1].events.axes[0]==32768);
    child(5,"config","get_active");assert(strstr(output,"targetConfig"));
    char identity[33],line[200];osg_gateway_identity_format(&gateway.devices[5].identity,identity);
    snprintf(line,sizeof line,"cockpit 3 %u disable %s",gateway.core.epoch,identity);assert(strstr(request(line),"\"ok\":true"));advance(700);
    int slot=osg_gateway_find_slot(&gateway.core.active,&gateway.devices[5].identity);assert(slot>=0 && !gateway.core.active.slots[slot].enabled);
    uint32_t event_head=gateway.devices[5].events.head;debug(5,"contact",2,1,2);advance(30);debug(5,"contact",2,0,2);advance(30);
    assert(gateway.devices[5].events.head==event_head+2);
    debug(5,"direct",1,0,1);advance(40);index=osg_gateway_find_module(&gateway.core,&gateway.devices[5].identity);assert(gateway.core.modules[index].direct_usb);
    debug(5,"direct",0,0,1);advance(40);
    debug(5,"online",0,0,1);assert(!gateway.core.modules[index].online);
    snprintf(line,sizeof line,"cockpit 3 %u forget %s",gateway.core.epoch,identity);assert(strstr(request(line),"\"ok\":true"));advance(700);
    debug(5,"online",1,0,1);advance(700);assert(osg_gateway_find_slot(&gateway.core.pending,&gateway.devices[5].identity)>=0);
    uint32_t boot=gateway.devices[5].boot;debug(5,"reboot",0,0,0);advance(60);assert(gateway.devices[5].boot==boot+1 && gateway.devices[5].runtime.active.axis_options[0].inverted);
    snprintf(line,sizeof line,"debug 1 %u %s %u neutral",gateway.core.epoch,identity,boot);rejects(line,"stale_module_boot");
    rejects("debug 1 65536 nope 1 neutral","invalid_request");rejects("debug 1 65536 5049434f000000000000000000000099 1 neutral","unknown_module");
    const char *invalid[]={"axis -1 0","axis 8 0","axis 0 4096","contact 152 1","contact 0 2","turn 8 1","turn 0 0","turn 0 33","hat 4 0","hat 0 8","selector 8 0","selector 0 3","expansion 8 1","expansion 0 2","direct 2","online 2","unknown","neutral extra"};
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++) {
        osg_gateway_identity_format(&gateway.devices[6].identity,identity);snprintf(line,sizeof line,"debug 1 %u %s %u %s",gateway.core.epoch,identity,gateway.devices[6].boot,invalid[i]);rejects(line,"invalid_request");
    }
    debug(6,"turn",0,32,2);debug(6,"turn",0,32,2);
    snprintf(line,sizeof line,"debug 1 %u %s %u turn 0 1",gateway.core.epoch,identity,gateway.devices[6].boot);rejects(line,"input_queue_full");debug(6,"neutral",0,0,0);
    fail_flash=true;rejects("debug 1 65536 - 0 add pedals","flash_save_failed");fail_flash=false;
    fail_gateway=true;rejects("debug 1 65536 - 0 add radio","flash_save_failed");fail_gateway=false;
    assert(!gateway.devices[7].occupied && !osg_journal_erased(&journals[8]));
    add("output-only");rejects("debug 1 65536 - 0 add pedals","capacity_exceeded");
    osg_virtual_device_t *lights=&gateway.devices[7];
    assert(!lights->runtime.layout.length && !lights->runtime.active.button_count && !lights->runtime.active.axis_count);
    int lights_slot=osg_gateway_find_slot(&gateway.core.active,&lights->identity);
    assert(lights_slot>=0 && !gateway.core.active.slots[lights_slot].enabled && !gateway.core.active.slots[lights_slot].report_id);
    child(7,"action","describe 0");assert(strstr(output,"\"ownership\":{") && strstr(output,"led.rgb.set_color"));
    child(7,"action","claim 1 led.rgb.set_color 00112233-4455-6677-8899-aabbccddeeff 500 000000");assert(strstr(output,"\"leaseId\":1"));
    child(7,"action","invoke 1 led.rgb.set_color ffffff");assert(strstr(output,"outputBusy") && !lights->actions.rgb);
    child(7,"action","claim 1 led.rgb.set_color 11223344-5566-7788-99aa-bbccddeeff00 500 000000");assert(strstr(output,"outputBusy"));
    child(7,"action","invoke_owned 00112233-4455-6677-8899-aabbccddeeff 1 1 led.rgb.set_color 12abef");assert(lights->actions.rgb==0x12abef);
    child(7,"action","invoke 1 led.boolean.set_enabled 1");assert(lights->actions.enabled);
    advance(501);assert(!lights->actions.rgb && lights->actions.enabled);
    child(7,"action","ownership 0");assert(strstr(output,"\"fallbackStatus\":\"applied\"") && !strstr(output,"00112233-4455"));
    child(7,"action","claim 1 led.rgb.set_color 00112233-4455-6677-8899-aabbccddeeff 5000 000000");
    child(7,"action","invoke_owned 00112233-4455-6677-8899-aabbccddeeff 2 1 led.rgb.set_color ffffff");
    debug(7,"online",0,0,1);assert(!lights->actions.rgb && !lights->actions.registry.leases[0].owned);
    debug(7,"online",1,0,1);advance(60);
    child(7,"action","claim 1 led.rgb.set_color 00112233-4455-6677-8899-aabbccddeeff 5000 000000");
    child(7,"action","invoke_owned 00112233-4455-6677-8899-aabbccddeeff 3 1 led.rgb.set_color ffffff");
    debug(7,"reboot",0,0,0);advance(60);assert(!lights->actions.rgb && !lights->actions.enabled && !lights->actions.registry.lease_counter);
    assert(osg_debug_init(&restored,gateway.uid,gateway.serial,journals));assert(restored.core.epoch==131072 && restored.devices[7].occupied && restored.devices[5].runtime.active.axis_options[0].inverted);
    child(0,"action","claim 1 led.rgb.set_color 00112233-4455-6677-8899-aabbccddeeff 5000 000000");
    child(0,"action","invoke_owned 00112233-4455-6677-8899-aabbccddeeff 1 1 led.rgb.set_color ffffff");
    assert(gateway.devices[0].actions.rgb==0xffffff);
    debug(0,"contact",0,1,2);advance(30);osg_debug_connected(&gateway,false);
    assert(!gateway.devices[0].actions.rgb && !gateway.devices[0].actions.registry.leases[0].owned);assert(!gateway.core.running && !gateway.devices[0].sample.digital_closed);
    osg_debug_connected(&gateway,true);debug(0,"contact",0,1,2);advance(OSG_DEBUG_WATCHDOG_MS+1);assert(!gateway.connected && !gateway.core.running && !gateway.devices[0].sample.digital_closed);
    /* Fail closed when retained gateway storage is corrupt, including metadata. */
    memset(flash[0],0,sizeof flash[0]);assert(!osg_debug_init(&restored,gateway.uid,gateway.serial,journals));
    puts("Pico debug core: acquisition, lifecycle, DCS actions/events, flash and failure tests passed");
    return 0;
}
