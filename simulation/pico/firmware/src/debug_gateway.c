#include "debug_gateway.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RECORD_SIZE=OSG_GW_REGISTRY_RECORD_SIZE+12 };
typedef struct { char *out;size_t capacity,length;bool valid; } writer_t;
static void append(writer_t *w,const char *format,...) {
    if(!w->valid || w->length>=w->capacity) { w->valid=false;return; }
    va_list args;va_start(args,format);int n=vsnprintf(w->out+w->length,w->capacity-w->length,format,args);va_end(args);
    if(n<0 || (size_t)n>=w->capacity-w->length) { w->valid=false;return; }w->length+=(size_t)n;
}
static bool persist(void *context,const osg_gateway_registry_t *registry) {
    osg_debug_gateway_t *g=context;
    if(!g->journals[0].read) return true;
    uint8_t record[RECORD_SIZE];size_t n=osg_gateway_registry_encode(registry,record,sizeof record);
    if(n!=OSG_GW_REGISTRY_RECORD_SIZE) return false;
    memcpy(record+n,"PDBG",4);
    for(unsigned i=0;i<OSG_DEBUG_DEVICE_COUNT;i++) record[n+4+i]=g->devices[i].occupied?g->devices[i].preset:255;
    return osg_journal_commit(&g->journals[0],record,sizeof record,g->core.epoch);
}
bool osg_debug_init(osg_debug_gateway_t *g,const char uid[25],const char *serial,const osg_journal_io_t *journals) {
    if(!g || !uid || strlen(uid)!=24 || !serial || strlen(serial)!=25 ||
       strncmp(serial,"OSG-PICO-",9) || strncmp(uid,"00000000",8) ||
       !strcmp(uid,"000000000000000000000000")) return false;
    for(unsigned i=0;i<24;i++) if(!((uid[i]>='0' && uid[i]<='9') || (uid[i]>='a' && uid[i]<='f'))) return false;
    for(unsigned i=0;i<16;i++) if(tolower((unsigned char)serial[i+9])!=uid[i+8]) return false;
    memset(g,0,sizeof *g);strcpy(g->uid,uid);strcpy(g->serial,serial);
    if(journals) memcpy(g->journals,journals,sizeof g->journals);
    uint8_t record[RECORD_SIZE],presets[OSG_DEBUG_DEVICE_COUNT]={0,1,2,3,255,255,255,255};size_t size=0;uint32_t epoch=0;
    osg_gateway_registry_t registry,*restored=NULL;
    if(g->journals[0].read) {
        if(osg_journal_load(&g->journals[0],record,sizeof record,&size,&epoch)) {
            if(size!=sizeof record || memcmp(record+OSG_GW_REGISTRY_RECORD_SIZE,"PDBG",4) ||
               !osg_gateway_registry_decode(record,OSG_GW_REGISTRY_RECORD_SIZE,&registry)) return false;
            memcpy(presets,record+OSG_GW_REGISTRY_RECORD_SIZE+4,sizeof presets);restored=&registry;
        } else if(!osg_journal_erased(&g->journals[0])) return false;
    }
    if(epoch>UINT32_MAX-65536U || !osg_gateway_init(&g->core,epoch+65536U,restored)) return false;
    for(unsigned i=0;i<OSG_DEBUG_DEVICE_COUNT;i++) if(presets[i]!=255) {
        if(!osg_virtual_init(&g->devices[i],i,presets[i],g->core.epoch+i+1,0,g->journals[i+1].read?&g->journals[i+1]:NULL)) return false;
    }
    return persist(g,&g->core.pending);
}
void osg_debug_connected(osg_debug_gateway_t *g,bool connected) {
    if(g->connected==connected) return;
    g->connected=connected;g->watchdog=connected;g->last_command=g->now;
    for(unsigned i=0;i<OSG_DEBUG_DEVICE_COUNT;i++) if(g->devices[i].occupied) {
        (void)osg_virtual_debug(&g->devices[i],"neutral",0,0,g->now);
        osg_virtual_rearm(&g->devices[i],g->now,true);
        if(!connected) {
            osg_runtime_invalidate_claim(&g->devices[i].runtime);
            osg_actions_invalidate_scope(&g->devices[i].actions.registry,OSG_ACTION_SCOPE_CAN,g->now);
        }
    }
    osg_gateway_set_running(&g->core,connected && !g->layout.detached);
}
osg_gateway_layout_event_t osg_debug_tick(osg_debug_gateway_t *g,uint32_t now,bool drained) {
    g->now=now;
    if(g->watchdog && (uint32_t)(now-g->last_command)>=OSG_DEBUG_WATCHDOG_MS) osg_debug_connected(g,false);
    for(unsigned i=0;i<OSG_DEBUG_DEVICE_COUNT;i++) osg_virtual_tick(&g->devices[i],&g->core,now);
    osg_gateway_tick(&g->core,now);
    osg_gateway_layout_event_t event=osg_gateway_layout_service(&g->layout,&g->core,now,drained,persist,g);
    if(event==OSG_GATEWAY_LAYOUT_ATTACH) osg_gateway_set_running(&g->core,g->connected);
    return event;
}
bool osg_debug_layout_activate(osg_debug_gateway_t *g) {
    osg_gateway_set_running(&g->core,false);
    return osg_gateway_layout_activate(&g->layout,&g->core,g->now);
}
static osg_virtual_device_t *device(osg_debug_gateway_t *g,const osg_gateway_identity_t *identity) {
    for(unsigned i=0;i<OSG_DEBUG_DEVICE_COUNT;i++) if(g->devices[i].occupied && osg_gateway_identity_equal(&g->devices[i].identity,identity)) return &g->devices[i];
    return NULL;
}
static void metadata(osg_debug_gateway_t *g,writer_t *w) {
    append(w,"{\"maxDevices\":8,\"presets\":[");
    for(unsigned i=0;i<osg_debug_preset_count;i++) append(w,"%s\"%s\"",i?",":"",osg_debug_presets[i]);
    append(w,"],\"devices\":[");bool first=true;
    for(unsigned i=0;i<OSG_DEBUG_DEVICE_COUNT;i++) {
        osg_virtual_device_t *d=&g->devices[i];if(!d->occupied) continue;
        char identity[33];osg_gateway_identity_format(&d->identity,identity);
        append(w,"%s{\"id\":\"%s\",\"preset\":\"%s\",\"online\":%s,\"boot\":%lu,\"directUsb\":%s,\"contacts\":[",first?"":",",identity,osg_debug_presets[d->preset],d->online?"true":"false",(unsigned long)d->boot,d->direct?"true":"false");first=false;
        bool first_contact=true;
        for(unsigned input=0;input<152;input++) {
            bool closed=input<24?(d->sample.digital_closed&(UINT32_C(1)<<input))!=0:(d->sample.expansion_closed[(input-24)/16]&(1U<<((input-24)%16)))!=0;
            if(closed) { append(w,"%s%u",first_contact?"":",",input);first_contact=false; }
        }
        append(w,"],\"axes\":[");for(unsigned j=0;j<8;j++) append(w,"%s%u",j?",":"",d->sample.axes[j]);
        append(w,"],\"expansionMask\":%u,\"definition\":",d->sample.expansion_available);
        if(!osg_runtime_snapshot(&d->runtime,true,g->child,sizeof g->child)) { w->valid=false;return; }
        char *start=strstr(g->child,"\"config\":");char *end=strstr(g->child,"},\"layout\":");
        if(!start || !end) { w->valid=false;return; }start+=9;end[1]=0;append(w,"%s}",start);
    }
    append(w,"]}");
}
static size_t response(osg_debug_gateway_t *g,const osg_gateway_cockpit_request_t *r,const char *error,const char *target,char *out,size_t capacity) {
    size_t n=r->action==OSG_GW_COCKPIT_ACTION?osg_gateway_cockpit_action_response(&g->core,g->uid,r->id,error,target,out,capacity):
        r->action==OSG_GW_COCKPIT_EVENTS?osg_gateway_cockpit_events_response(&g->core,g->uid,r->id,error,target,out,capacity):
        osg_gateway_cockpit_config_response(&g->core,g->uid,r->id,error,target,out,capacity);
    if(!n) return 0;
    const char *old="\"gatewayType\":\"stm32-can-gateway\"";char *where=strstr(out,old);char replacement[160];
    int length=snprintf(replacement,sizeof replacement,"\"gatewayType\":\"pico-debug-gateway\",\"serial\":\"%s\",\"debug\":1,\"maxVirtual\":8",g->serial);
    if(!where || length<0 || (size_t)length>=sizeof replacement || n+(size_t)length-strlen(old)>=capacity) return 0;
    size_t offset=(size_t)(where-out),old_length=strlen(old);
    memmove(where+length,where+old_length,n-offset-old_length+1);memcpy(where,replacement,(size_t)length);n+=(size_t)length-old_length;
    char *firmware=strstr(out,"\"firmware\":\"0.4.0\"");if(firmware) memcpy(firmware+12,"0.1.0",5);
    if(n<3) return 0;
    writer_t w={out,capacity,n-3,true};append(&w,",\"debug\":");metadata(g,&w);append(&w,"}\r\n");
    if(!w.valid) { out[0]=0;return 0; }return w.length;
}
static bool token_u32(const char **cursor,uint32_t *out) {
    const char *p=*cursor;uint32_t value=0;unsigned digits=0;
    while(*p>='0' && *p<='9') { unsigned d=(unsigned)(*p-'0');if(value>(UINT32_MAX-d)/10U) return false;value=value*10U+d;p++;digits++; }
    if(!digits || (*p && *p!=' ')) return false;
    *out=value;*cursor=*p?p+1:p;return true;
}
static const char *debug_command(osg_debug_gateway_t *g,const char *line,osg_gateway_cockpit_request_t *r) {
    const char *p=line+6;
    if(!token_u32(&p,&r->id) || !r->id || !token_u32(&p,&r->epoch)) return "invalid_request";
    if(r->epoch!=g->core.epoch) return "stale_epoch";
    char identity[33];const char *space=strchr(p,' ');if(!space || space-p>32) return "invalid_request";
    memcpy(identity,p,(size_t)(space-p));identity[space-p]=0;p=space+1;
    if(!token_u32(&p,&r->boot)) return "invalid_request";
    char op[24];space=strchr(p,' ');size_t op_length=space?(size_t)(space-p):strlen(p);
    if(!op_length || op_length>=sizeof op) return "invalid_request";
    memcpy(op,p,op_length);op[op_length]=0;p=space?space+1:p+op_length;
    if(!strcmp(op,"describe")) return !strcmp(identity,"-") && !r->boot && !*p?NULL:"invalid_request";
    if(!strcmp(op,"add")) {
        if(strcmp(identity,"-") || r->boot || !*p) return "invalid_request";
        unsigned preset=0;while(preset<osg_debug_preset_count && strcmp(p,osg_debug_presets[preset])) preset++;
        if(preset==osg_debug_preset_count) return "invalid_preset";
        unsigned slot=0;while(slot<OSG_DEBUG_DEVICE_COUNT && g->devices[slot].occupied) slot++;
        if(slot==OSG_DEBUG_DEVICE_COUNT) return "capacity_exceeded";
        /* The gateway journal is authoritative for slot ownership. A previous
         * failed Add can leave an otherwise valid orphan device journal. Fresh
         * additions always start from the selected preset, including after a
         * power loss between device initialization and gateway metadata commit. */
        osg_journal_io_t *settings=&g->journals[slot+1];
        if(settings->read && !osg_journal_erased(settings) &&
           (!settings->erase(settings->context,0) || !settings->erase(settings->context,1))) return "flash_save_failed";
        if(!osg_virtual_init(&g->devices[slot],slot,preset,g->core.epoch+slot+1,g->now,g->journals[slot+1].read?&g->journals[slot+1]:NULL)) return "flash_save_failed";
        if(!persist(g,&g->core.pending)) { g->devices[slot].occupied=false;return "flash_save_failed"; }return NULL;
    }
    if(!osg_gateway_identity_parse(identity,&r->identity)) return "invalid_request";
    osg_virtual_device_t *d=device(g,&r->identity);if(!d) return "unknown_module";
    if(!r->boot || r->boot!=d->boot) return "stale_module_boot";
    unsigned count=!strcmp(op,"contact") || !strcmp(op,"axis") || !strcmp(op,"turn") || !strcmp(op,"hat") || !strcmp(op,"selector") || !strcmp(op,"expansion")?2:
        !strcmp(op,"online") || !strcmp(op,"direct")?1:0;
    int values[2]={0};
    for(unsigned i=0;i<count;i++) {
        bool negative=*p=='-';if(negative) p++;uint32_t value;
        if(!token_u32(&p,&value) || value>4095 || (negative && value==0)) return "invalid_request";
        values[i]=negative?-(int)value:(int)value;
    }
    if(*p) return "invalid_request";
    const char *error=osg_virtual_debug(d,op,values[0],values[1],g->now);
    if(!error && !d->online) {
        int index=osg_gateway_find_module(&g->core,&d->identity);
        if(index>=0) { g->core.modules[index].last_seen=g->now-OSG_GW_OFFLINE_TIMEOUT_MS;osg_gateway_tick(&g->core,g->now); }
    }
    return error;
}
size_t osg_debug_command(osg_debug_gateway_t *g,const char *line,char *out,size_t capacity) {
    if(!g || !line || !out || !capacity || strlen(line)>=OSG_DEBUG_LINE_CAPACITY) return 0;
    g->last_command=g->now;
    if(!strcmp(line,"hello") || !strcmp(line,"identity") || !strcmp(line,"info")) {
        int n=snprintf(out,capacity,"gatewayType=pico-debug-gateway protocol=3 canProtocol=3 firmware=0.1.0 board=pico_w identity=%s serial=%s epoch=%lu maxModules=32 debug=1 maxVirtual=8\r\n",g->uid,g->serial,(unsigned long)g->core.epoch);
        return n>0 && (size_t)n<capacity?(size_t)n:0;
    }
    osg_gateway_cockpit_request_t request={0};const char *error=NULL,*target=NULL;
    if(!strncmp(line,"debug ",6)) error=debug_command(g,line,&request);
    else if(!osg_gateway_cockpit_parse(line,&request)) error="invalid_request";
    else if(request.action==OSG_GW_COCKPIT_CONFIG || request.action==OSG_GW_COCKPIT_ACTION || request.action==OSG_GW_COCKPIT_EVENTS) {
        osg_virtual_device_t *d=device(g,&request.identity);int index=osg_gateway_find_module(&g->core,&request.identity);
        if(request.epoch!=g->core.epoch) error="stale_epoch";
        else if(!d || !d->online || index<0 || !g->core.modules[index].online) error="module_offline";
        else if(request.boot!=d->boot) error="stale_module_boot";
        else if(!g->core.modules[index].has_sequence) error="configuration_route_not_ready";
        else {
            char command[OSG_GW_CONFIG_COMMAND_CAPACITY];int n=snprintf(command,sizeof command,"%s%s",request.action==OSG_GW_COCKPIT_ACTION?"action ":request.action==OSG_GW_COCKPIT_EVENTS?"events ":"",request.command);
            if(n<0 || (size_t)n>=sizeof command) error="invalid_request";
            else { error=osg_virtual_command(d,command,g->now,g->child,sizeof g->child);target=g->child;
                /* The normal adapter reconciles settings through the renewed lease. */
                osg_virtual_tick(d,&g->core,g->now);
            }
        }
    } else { bool apply=false;error=osg_gateway_cockpit_execute(&g->core,&request,NULL,NULL,&apply); }
    return response(g,&request,error,target,out,capacity);
}
size_t osg_debug_report_prepare(osg_debug_gateway_t *g,unsigned slot,uint8_t *out,size_t capacity,osg_gateway_delivery_t *delivery) {
    if(!g || slot>=OSG_GW_MAX_MODULES || !out || capacity<2) return 0;
    size_t size=osg_gateway_delivery_prepare(&g->core,slot,out+1,capacity-1,delivery);
    if(size) out[0]=g->core.active.slots[slot].report_id;
    return size?size+1:0;
}
void osg_debug_report_complete(osg_debug_gateway_t *g,const osg_gateway_delivery_t *delivery,const uint8_t *report,size_t size) {
    osg_gateway_message_t ack;
    if(osg_gateway_delivery_complete(&g->core,delivery,report,size,&ack)) {
        /* The compact wire ACK carries node/boot/lease, not an identity. The
         * already-validated immutable delivery supplies the in-process route. */
        ack.identity=delivery->identity;
        osg_virtual_device_t *d=device(g,&ack.identity);if(d) osg_virtual_ack(d,&ack,g->now);
    }
}
