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
    if(!strcmp(p,"save")) { out->action=OSG_GW_COCKPIT_SAVE; return true; }
    if(!strcmp(p,"apply")) { out->action=OSG_GW_COCKPIT_APPLY; return true; }
    if(!strncmp(p,"register ",9)) { out->action=OSG_GW_COCKPIT_REGISTER; p+=9; }
    else if(!strncmp(p,"unregister ",11)) { out->action=OSG_GW_COCKPIT_UNREGISTER; p+=11; }
    else return false;
    return osg_gateway_identity_parse(p,&out->identity);
}
const char *osg_gateway_cockpit_execute(osg_gateway_t *g,const osg_gateway_cockpit_request_t *r,
                                      bool (*persist)(void *),void *context,bool *apply) {
    if(!apply) return "invalid_request";
    *apply=false;
    if(!g || !r || r->action>OSG_GW_COCKPIT_APPLY) return "invalid_request";
    if(r->action==OSG_GW_COCKPIT_SNAPSHOT) return NULL;
    if(r->epoch!=g->epoch) return "stale_epoch";
    if(r->action==OSG_GW_COCKPIT_REGISTER || r->action==OSG_GW_COCKPIT_UNREGISTER) {
        bool ok=r->action==OSG_GW_COCKPIT_REGISTER ? osg_gateway_register(g,&r->identity) : osg_gateway_unregister(g,&r->identity);
        return ok?NULL:"registration_rejected";
    }
    if(r->action==OSG_GW_COCKPIT_APPLY && !osg_gateway_needs_apply(g)) return NULL;
    if(!persist || !persist(context)) return "flash_save_failed";
    *apply=r->action==OSG_GW_COCKPIT_APPLY;
    return NULL;
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
size_t osg_gateway_cockpit_response(const osg_gateway_t *g,const char uid[25],uint32_t id,
                                   const char *error,char *out,size_t capacity) {
    if(!out || !capacity) return 0;
    out[0]=0;
    if(!g || !uid || strlen(uid)!=24) return 0;
    for(unsigned i=0;i<24;i++) if(!((uid[i]>='0' && uid[i]<='9') || (uid[i]>='a' && uid[i]<='f') || (uid[i]>='A' && uid[i]<='F'))) return 0;
    /* Error codes are machine tokens, not arbitrary externally supplied text. */
    if(error) for(const char *p=error;*p;p++) if(!((*p>='a' && *p<='z') || *p=='_')) return 0;
    writer_t w={.out=out,.capacity=capacity,.valid=true};
    append(&w,"{\"id\":%lu,\"ok\":%s,\"error\":",(unsigned long)id,boolean(!error));
    if(error) append(&w,"\"%s\"",error); else append(&w,"null");
    append(&w,",\"state\":{\"gatewayType\":\"stm32-can-gateway\",\"protocol\":2,\"canProtocol\":3,\"firmware\":\"0.3.0\",\"identity\":\"%s\",\"epoch\":%lu,\"running\":%s,\"needsApply\":%s,\"maxModules\":10,\"modules\":[",
        uid,(unsigned long)g->epoch,boolean(g->running),boolean(osg_gateway_needs_apply(g)));
    bool first=true;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) {
        const osg_gateway_module_t *m=&g->modules[i];
        if(!m->occupied) continue;
        int pending=osg_gateway_find_slot(&g->pending,&m->identity),active=osg_gateway_find_slot(&g->active,&m->identity);
        bool match=active>=0 && osg_gateway_profile_equal(&m->profile,&g->active.slots[active].profile);
        bool pending_match=pending>=0 && osg_gateway_profile_equal(&m->profile,&g->pending.slots[pending].profile);
        uint8_t report[OSG_GW_MAX_REPORT_SIZE]; osg_gateway_profile_neutral(&m->profile,report);
        if(match && g->running && m->online && !m->direct_usb && m->has_input) memcpy(report,m->report,sizeof report);
        char identity[33]; osg_gateway_identity_format(&m->identity,identity);
        append(&w,"%s{\"identity\":\"%s\",\"online\":%s,\"directUsb\":%s,\"node\":%u,\"lease\":%lu,\"profile\":{\"buttons\":%u,\"axes\":%u,\"hats\":%u,\"revision\":%u,\"axisUsage\":[",
            first?"":",",identity,boolean(m->online),boolean(m->direct_usb),m->node,(unsigned long)m->lease,
            m->profile.button_count,m->profile.axis_count,m->profile.hat_count,m->profile.revision);
        for(unsigned axis=0;axis<m->profile.axis_count;axis++) append(&w,"%s%u",axis?",":"",m->profile.axis_usage[axis]);
        append(&w,"],\"neutral\":[");
        for(unsigned axis=0;axis<m->profile.axis_count;axis++) append(&w,"%s%u",axis?",":"",m->profile.axis_neutral[axis]);
        append(&w,"]},\"pendingSlot\":%d,\"activeSlot\":%d,\"profileMatch\":%s,\"pendingProfileMatch\":%s,\"hasInput\":%s,\"report\":\"",pending,active,boolean(match),boolean(pending_match),boolean(m->has_input));
        size_t length=osg_gateway_profile_report_size(&m->profile);
        for(size_t byte=0;byte<length;byte++) append(&w,"%02x",report[byte]);
        append(&w,"\"}"); first=false;
    }
    append(&w,"]}}\r\n");
    if(!w.valid) { out[0]=0; return 0; }
    return w.length;
}
