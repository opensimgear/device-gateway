#include "opensimgear/gateway_cockpit.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool number(const char **cursor,uint32_t *out) {
    const char *p=*cursor; uint32_t value=0; unsigned digits=0;
    while(*p>='0' && *p<='9') {
        uint32_t digit=(uint32_t)(*p-'0');
        if(value>(UINT32_MAX-digit)/10U) return false;
        value=value*10U+digit; p++; digits++;
    }
    if(!digits || (*p && *p!=' ')) return false;
    *cursor=p; *out=value; return true;
}
bool osg_gateway_cockpit_parse(const char *line,osg_gateway_cockpit_request_t *out) {
    if(!line || !out) return false;
    memset(out,0,sizeof *out);
    if(strncmp(line,"cockpit ",8)) return false;
    const char *p=line+8;
    if(!number(&p,&out->id) || *p++!=' ') return false;
    if(!strcmp(p,"snapshot")) { out->action=OSG_GW_COCKPIT_SNAPSHOT; return true; }
    if(!number(&p,&out->epoch) || !out->epoch || *p++!=' ') return false;
    if(!strncmp(p,"config ",7) || !strncmp(p,"action ",7)) {
        out->action=*p=='a' ? OSG_GW_COCKPIT_ACTION : OSG_GW_COCKPIT_CONFIG; p+=7;
        if(strlen(p)<35 || p[32]!=' ') return false;
        char identity[33]; memcpy(identity,p,32); identity[32]=0;
        if(!osg_gateway_identity_parse(identity,&out->identity)) return false;
        p+=33;
        if(!number(&p,&out->boot) || !out->boot || *p++!=' ') return false;
        size_t length=strlen(p);
        size_t prefix=out->action==OSG_GW_COCKPIT_ACTION ? 7U : 0U;
        if(!length || length+prefix>=sizeof out->command) return false;
        for(size_t i=0;i<length;i++) if((unsigned char)p[i]<32 || (unsigned char)p[i]>126) return false;
        memcpy(out->command,p,length+1); return true;
    }
    if(!strncmp(p,"enable ",7)) { out->action=OSG_GW_COCKPIT_ENABLE; p+=7; }
    else if(!strncmp(p,"disable ",8)) { out->action=OSG_GW_COCKPIT_DISABLE; p+=8; }
    else if(!strncmp(p,"forget ",7)) { out->action=OSG_GW_COCKPIT_FORGET; p+=7; }
    else if(!strncmp(p,"update-profile ",15)) { out->action=OSG_GW_COCKPIT_UPDATE_PROFILE; p+=15; }
    else return false;
    return osg_gateway_identity_parse(p,&out->identity);
}
const char *osg_gateway_cockpit_execute(osg_gateway_t *g,const osg_gateway_cockpit_request_t *r,
                                      bool (*persist)(void *),void *context,bool *apply) {
    (void)persist; (void)context;
    if(!apply) return "invalid_request";
    *apply=false;
    if(!g || !r) return "invalid_request";
    if(r->action==OSG_GW_COCKPIT_SNAPSHOT) return NULL;
    if(r->epoch!=g->epoch) return "stale_epoch";
    bool ok=false;
    switch(r->action) {
        case OSG_GW_COCKPIT_ENABLE: ok=osg_gateway_enable(g,&r->identity); break;
        case OSG_GW_COCKPIT_DISABLE: ok=osg_gateway_disable(g,&r->identity); break;
        case OSG_GW_COCKPIT_FORGET: {
            int module=osg_gateway_find_module(g,&r->identity);
            if(module>=0 && g->modules[module].online) return "device_online";
            ok=osg_gateway_forget(g,&r->identity); break;
        }
        case OSG_GW_COCKPIT_UPDATE_PROFILE: ok=osg_gateway_update_profile(g,&r->identity); break;
        default: return "invalid_request";
    }
    return ok?NULL:"lifecycle_rejected";
}
typedef struct { char *out; size_t capacity,length; bool valid; } writer_t;
static void append(writer_t *w,const char *format,...) {
    if(!w->valid) return;
    va_list args; va_start(args,format);
    int n=vsnprintf(w->out+w->length,w->capacity-w->length,format,args);
    va_end(args);
    if(n<0 || (size_t)n>=w->capacity-w->length) { w->valid=false; return; }
    w->length+=(size_t)n;
}
static const char *boolean(bool value) { return value?"true":"false"; }
static size_t module_response(const osg_gateway_t *g,const char uid[25],uint32_t id,
    const char *error,const char *target,const char *target_key,char *out,size_t capacity) {
    if(!out || !capacity) return 0;
    out[0]=0;
    if(!g || !uid || strlen(uid)!=24) return 0;
    for(unsigned i=0;i<24;i++) if(!((uid[i]>='0' && uid[i]<='9') || (uid[i]>='a' && uid[i]<='f') || (uid[i]>='A' && uid[i]<='F'))) return 0;
    /* Error codes are machine tokens, not arbitrary externally supplied text. */
    if(error) for(const char *p=error;*p;p++) if(!((*p>='a' && *p<='z') || *p=='_')) return 0;
    writer_t w={.out=out,.capacity=capacity,.valid=true};
    append(&w,"{\"id\":%lu,\"ok\":%s,\"error\":",(unsigned long)id,boolean(!error));
    if(error) append(&w,"\"%s\"",error); else append(&w,"null");
    append(&w,",\"state\":{\"gatewayType\":\"stm32-can-gateway\",\"protocol\":3,\"canProtocol\":3,\"firmware\":\"0.4.0\",\"identity\":\"%s\",\"epoch\":%lu,\"running\":%s,\"lifecycle\":\"automatic\",\"layoutPending\":%s,\"layoutError\":",
        uid,(unsigned long)g->epoch,boolean(g->running),boolean(osg_gateway_needs_apply(g) || g->layout_transition));
    if(g->layout_error) append(&w,"\"%s\"",g->layout_error); else append(&w,"null");
    append(&w,",\"maxModules\":%u,\"maxRemembered\":%u,\"maxControllers\":%u,\"activeGeneration\":%lu,\"pendingGeneration\":%lu,\"rejectedAnnouncements\":%lu,\"lastRejectedIdentity\":",
        OSG_GW_MAX_MODULES,OSG_GW_MAX_MODULES,OSG_GW_MAX_MODULES,(unsigned long)g->active.generation,
        (unsigned long)g->pending.generation,(unsigned long)g->rejected_announcements);
    char rejected[33]; osg_gateway_identity_format(&g->rejected_identity,rejected);
    if(g->admission_error) append(&w,"\"%s\"",rejected); else append(&w,"null");
    append(&w,",\"admissionError\":");
    if(g->admission_error) append(&w,"\"%s\"",g->admission_error); else append(&w,"null");
    append(&w,",\"admissionFailure\":");
    if(g->admission_error) append(&w,"{\"identity\":\"%s\",\"error\":\"%s\"}",rejected,g->admission_error);
    else append(&w,"null");
    append(&w,",\"modules\":[");
    bool first=true;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) {
        const osg_gateway_module_t *m=&g->modules[i];
        if(!m->occupied) continue;
        int pending=osg_gateway_find_slot(&g->pending,&m->identity),active=osg_gateway_find_slot(&g->active,&m->identity);
        bool published=active>=0 && g->active.slots[active].enabled;
        bool enabled=pending>=0 && g->pending.slots[pending].enabled;
        bool match=published && osg_gateway_profile_equal(&m->profile,&g->active.slots[active].profile);
        uint8_t report[OSG_GW_MAX_REPORT_SIZE]; osg_gateway_profile_neutral(&m->profile,report);
        if(enabled && match && g->running && m->online && !m->direct_usb && m->has_input) memcpy(report,m->report,sizeof report);
        char identity[33]; osg_gateway_identity_format(&m->identity,identity);
        append(&w,"%s{\"identity\":\"%s\",\"online\":%s,\"directUsb\":%s,\"node\":%u,\"sourceNode\":%u,\"boot\":%lu,\"configurationGeneration\":%lu,\"lease\":%lu,\"profile\":{\"buttons\":%u,\"axes\":%u,\"hats\":%u,\"revision\":%u,\"axisUsage\":[",
            first?"":",",identity,boolean(m->online),boolean(m->direct_usb),m->node,m->source_node,(unsigned long)m->boot_session,(unsigned long)m->configuration_generation,(unsigned long)m->lease,
            m->profile.button_count,m->profile.axis_count,m->profile.hat_count,m->profile.revision);
        for(unsigned axis=0;axis<m->profile.axis_count;axis++) append(&w,"%s%u",axis?",":"",m->profile.axis_usage[axis]);
        append(&w,"],\"neutral\":[");
        for(unsigned axis=0;axis<m->profile.axis_count;axis++) append(&w,"%s%u",axis?",":"",m->profile.axis_neutral[axis]);
        append(&w,"]},\"enabled\":%s,\"published\":%s,\"descriptorIndex\":%d,\"reportId\":",boolean(enabled),boolean(published),published?g->active.slots[active].order:-1);
        if(published) append(&w,"%u",g->active.slots[active].report_id); else append(&w,"null");
        append(&w,",\"profileMatch\":%s,\"hasInput\":%s,\"report\":\"",boolean(match),boolean(m->has_input));
        size_t length=osg_gateway_profile_report_size(&m->profile);
        for(size_t byte=0;byte<length;byte++) append(&w,"%02x",report[byte]);
        append(&w,"\"}"); first=false;
    }
    append(&w,"]}");
    if(target) append(&w,",\"%s\":%s",target_key,target);
    append(&w,"}\r\n");
    if(!w.valid) { out[0]=0; return 0; }
    return w.length;
}
size_t osg_gateway_cockpit_config_response(const osg_gateway_t *g,const char uid[25],uint32_t id,
    const char *error,const char *target_config,char *out,size_t capacity) {
    return module_response(g,uid,id,error,target_config,"targetConfig",out,capacity);
}
size_t osg_gateway_cockpit_action_response(const osg_gateway_t *g,const char uid[25],uint32_t id,
    const char *error,const char *target_action,char *out,size_t capacity) {
    return module_response(g,uid,id,error,target_action,"targetAction",out,capacity);
}
size_t osg_gateway_cockpit_response(const osg_gateway_t *g,const char uid[25],uint32_t id,
    const char *error,char *out,size_t capacity) {
    return osg_gateway_cockpit_config_response(g,uid,id,error,NULL,out,capacity);
}
