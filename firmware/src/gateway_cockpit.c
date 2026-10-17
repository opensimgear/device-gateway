#include "opensimgear/gateway_cockpit.h"
#include "opensimgear/gateway_publication.h"
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
static bool hex32(const char **cursor,uint32_t *out) {
    const char *p=*cursor;uint32_t value=0;
    for(unsigned i=0;i<8;i++) {
        unsigned char c=(unsigned char)*p++;unsigned n;
        if(c>='0' && c<='9') n=c-'0';else if(c>='a' && c<='f') n=c-'a'+10U;else if(c>='A' && c<='F') n=c-'A'+10U;else return false;
        value=(value<<4)|n;
    }
    if(*p && *p!=' ') return false;
    *cursor=p;*out=value;return true;
}
static bool publication_parse(const char *p,osg_gateway_cockpit_request_t *out) {
    out->action=OSG_GW_COCKPIT_PUBLICATION;
    if(!strcmp(p,"describe")) {out->publication_action=OSG_GW_PUBLICATION_DESCRIBE;return true;}
    if(!strcmp(p,"state")) {out->publication_action=OSG_GW_PUBLICATION_STATE;return true;}
    if(!strncmp(p,"claim ",6)) {out->publication_action=OSG_GW_PUBLICATION_CLAIM;p+=6;}
    else if(!strncmp(p,"renew ",6)) {out->publication_action=OSG_GW_PUBLICATION_RENEW;p+=6;}
    else if(!strncmp(p,"release ",8)) {out->publication_action=OSG_GW_PUBLICATION_RELEASE;p+=8;}
    else if(!strncmp(p,"inspect ",8)) {out->publication_action=OSG_GW_PUBLICATION_INSPECT;p+=8;}
    else return false;
    if(strlen(p)<34 || p[32]!=' ') return false;
    char owner[33];memcpy(owner,p,32);owner[32]=0;osg_gateway_identity_t parsed;
    if(!osg_gateway_identity_parse(owner,&parsed)) return false;
    memcpy(out->owner,parsed.bytes,16);p+=33;
    if(out->publication_action==OSG_GW_PUBLICATION_CLAIM) {
        if(!number(&p,&out->publication_revision) || !out->publication_revision || *p++!=' ' ||
           !hex32(&p,&out->target_mask) || !out->target_mask || *p++!=' ' || !hex32(&p,&out->enabled_mask) ||
           (out->enabled_mask&~out->target_mask) || *p++!=' ') return false;
    } else {
        if(!number(&p,&out->publication_lease) || !out->publication_lease) return false;
        if(out->publication_action==OSG_GW_PUBLICATION_RELEASE || out->publication_action==OSG_GW_PUBLICATION_INSPECT) return !*p;
        if(*p++!=' ') return false;
    }
    return number(&p,&out->ttl) && !*p && out->ttl>=OSG_PUBLICATION_MIN_TTL_MS && out->ttl<=OSG_PUBLICATION_MAX_TTL_MS;
}
bool osg_gateway_cockpit_parse(const char *line,osg_gateway_cockpit_request_t *out) {
    if(!line || !out) return false;
    memset(out,0,sizeof *out);
    if(strncmp(line,"cockpit ",8)) return false;
    const char *p=line+8;
    if(!number(&p,&out->id) || *p++!=' ') return false;
    if(!strcmp(p,"snapshot")) { out->action=OSG_GW_COCKPIT_SNAPSHOT; return true; }
    if(!number(&p,&out->epoch) || !out->epoch || *p++!=' ') return false;
    if(!strncmp(p,"publication ",12)) return publication_parse(p+12,out);
    if(!strncmp(p,"config ",7) || !strncmp(p,"action ",7) || !strncmp(p,"events ",7)) {
        out->action=*p=='a' ? OSG_GW_COCKPIT_ACTION : *p=='e' ? OSG_GW_COCKPIT_EVENTS : OSG_GW_COCKPIT_CONFIG; p+=7;
        if(strlen(p)<35 || p[32]!=' ') return false;
        char identity[33]; memcpy(identity,p,32); identity[32]=0;
        if(!osg_gateway_identity_parse(identity,&out->identity)) return false;
        p+=33;
        if(!number(&p,&out->boot) || !out->boot || *p++!=' ') return false;
        size_t length=strlen(p);
        size_t prefix=out->action!=OSG_GW_COCKPIT_CONFIG ? 7U : 0U;
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
    if(r && r->action==OSG_GW_COCKPIT_PUBLICATION) return "publication_clock_required";
    return osg_gateway_cockpit_execute_at(g,r,persist,context,apply,0);
}
const char *osg_gateway_cockpit_execute_at(osg_gateway_t *g,const osg_gateway_cockpit_request_t *r,
                                      bool (*persist)(void *),void *context,bool *apply,uint32_t now) {
    (void)persist; (void)context;
    if(!apply) return "invalid_request";
    *apply=false;
    if(!g || !r) return "invalid_request";
    if(r->action==OSG_GW_COCKPIT_SNAPSHOT) return NULL;
    if(r->epoch!=g->epoch) return "stale_epoch";
    osg_gateway_publication_tick(g,now);
    if(r->action==OSG_GW_COCKPIT_PUBLICATION) {
        switch(r->publication_action) {
            case OSG_GW_PUBLICATION_DESCRIBE:case OSG_GW_PUBLICATION_STATE:case OSG_GW_PUBLICATION_INSPECT:return NULL;
            case OSG_GW_PUBLICATION_CLAIM:return osg_gateway_publication_claim(g,r->owner,r->publication_revision,r->target_mask,r->enabled_mask,r->ttl,now);
            case OSG_GW_PUBLICATION_RENEW:return osg_gateway_publication_renew(g,r->owner,r->publication_lease,r->ttl,now);
            case OSG_GW_PUBLICATION_RELEASE:return osg_gateway_publication_release(g,r->owner,r->publication_lease,now);
            default:return "invalid_publication_request";
        }
    }
    if(osg_gateway_publication_busy(g,NULL)) return "publication_busy";
    bool ok=false;
    switch(r->action) {
        case OSG_GW_COCKPIT_ENABLE: {
            int module=osg_gateway_find_module(g,&r->identity),slot=osg_gateway_find_slot(&g->pending,&r->identity);
            const osg_gateway_profile_t *p=module>=0?&g->modules[module].profile:slot>=0?&g->pending.slots[slot].profile:NULL;
            if(p && !p->button_count && !p->axis_count && !p->hat_count) return "input_not_supported";
            ok=osg_gateway_enable(g,&r->identity);break;
        }
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
static const char *phase(osg_publication_phase_t p) {
    switch(p) {case OSG_PUBLICATION_OWNED:return "owned";case OSG_PUBLICATION_RESTORING:return "restoring";
        case OSG_PUBLICATION_ATTENTION:return "attention";default:return "idle";}
}
static const char *outcome(osg_publication_result_t p) {
    switch(p) {case OSG_PUBLICATION_OK:return "ok";case OSG_PUBLICATION_RESTORE:return "restore";
        case OSG_PUBLICATION_PRESERVED:return "preserved";case OSG_PUBLICATION_UNVERIFIED:return "unverified";
        case OSG_PUBLICATION_EXHAUSTED:return "exhausted";default:return "no_change";}
}
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
        uid,(unsigned long)g->epoch,boolean(g->running),boolean(osg_gateway_needs_apply(g) || g->layout_transition || g->publication.dirty));
    if(g->layout_error) append(&w,"\"%s\"",g->layout_error); else append(&w,"null");
    osg_gateway_identity_t publisher;memcpy(publisher.bytes,g->publication.lease.scope.publisher,16);
    append(&w,",\"controllerPublicationOverride\":");
    if(osg_gateway_identity_valid(&publisher)) append(&w,"{\"schema\":1,\"scope\":\"gateway-can\",\"nonpersistent\":true,\"captureAtClaim\":true,\"restore\":\"compare-if-unchanged\",\"minLeaseMs\":%u,\"maxLeaseMs\":%u,\"maxTargets\":32,\"reconnectRequired\":true}",OSG_PUBLICATION_MIN_TTL_MS,OSG_PUBLICATION_MAX_TTL_MS);
    else append(&w,"null");
    const osg_gateway_publication_t *p=&g->publication;
    append(&w,",\"publicationRevision\":%lu,\"configuredGeneration\":%lu,\"effectiveGeneration\":%lu,\"publication\":{\"phase\":\"%s\",\"outcome\":\"%s\",\"leaseId\":%lu,\"expiresAtMs\":%lu,\"transition\":\"%llu\",\"claimRevision\":%lu,\"targetMask\":\"%08lx\",\"enabledMask\":\"%08lx\",\"expectedReconnect\":%s}",
        (unsigned long)g->publication_revision,(unsigned long)g->configured.generation,(unsigned long)g->active.generation,
        phase(p->lease.phase),outcome(p->lease.outcome),(unsigned long)p->lease.ticket.lease_id,(unsigned long)p->lease.ticket.expires_at,
        (unsigned long long)p->lease.ticket.transition,(unsigned long)p->claim_revision,(unsigned long)p->target_mask,(unsigned long)p->enabled_mask,boolean(p->expected_reconnect));
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
        /* Retain neutral staged-Disable inventory, but a selected owned
         * overlay follows effective HID visibility even if durable intent is
         * disabled. The native publication delivery gate is authoritative. */
        if((enabled || osg_gateway_publication_busy(g,&m->identity)) && match && g->running && osg_gateway_publication_deliverable(g,&m->identity) && m->online && !m->direct_usb && m->has_input) memcpy(report,m->report,sizeof report);
        char identity[33]; osg_gateway_identity_format(&m->identity,identity);
        bool output_only=!m->profile.button_count && !m->profile.axis_count && !m->profile.hat_count;
        append(&w,"%s{\"identity\":\"%s\",\"online\":%s,\"directUsb\":%s,\"node\":%u,\"sourceNode\":%u,\"boot\":%lu,\"configurationGeneration\":%lu,\"lease\":%lu,\"profile\":{\"buttons\":%u,\"axes\":%u,\"hats\":%u,\"revision\":%u,\"axisUsage\":[",
            first?"":",",identity,boolean(m->online),boolean(m->direct_usb),m->node,m->source_node,(unsigned long)m->boot_session,(unsigned long)m->configuration_generation,(unsigned long)m->lease,
            m->profile.button_count,m->profile.axis_count,m->profile.hat_count,m->profile.revision);
        for(unsigned axis=0;axis<m->profile.axis_count;axis++) append(&w,"%s%u",axis?",":"",m->profile.axis_usage[axis]);
        append(&w,"],\"neutral\":[");
        for(unsigned axis=0;axis<m->profile.axis_count;axis++) append(&w,"%s%u",axis?",":"",m->profile.axis_neutral[axis]);
        int configured=osg_gateway_find_slot(&g->configured,&m->identity);
        append(&w,"]},\"publicationReportId\":%u,\"configuredDescriptorIndex\":%d",configured>=0?g->configured.slots[configured].report_id:0,configured>=0 && g->configured.slots[configured].enabled?g->configured.slots[configured].order:-1);
        append(&w,",\"publicationSlot\":%d,\"configuredEnabled\":%s,\"enabled\":%s,\"published\":%s,\"descriptorIndex\":%d,\"reportId\":",pending,boolean(configured>=0 && g->configured.slots[configured].enabled),boolean(enabled),boolean(published),published?g->active.slots[active].order:-1);
        if(published) append(&w,"%u",g->active.slots[active].report_id); else append(&w,"null");
        append(&w,",\"profileMatch\":%s,\"hasInput\":%s,\"report\":\"",boolean(match),boolean(m->has_input));
        size_t length=osg_gateway_profile_report_size(&m->profile);
        for(size_t byte=0;byte<length;byte++) append(&w,"%02x",report[byte]);
        append(&w,"\"%s}",output_only?",\"outputOnly\":true":""); first=false;
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
size_t osg_gateway_cockpit_events_response(const osg_gateway_t *g,const char uid[25],uint32_t id,
    const char *error,const char *target_events,char *out,size_t capacity) {
    return module_response(g,uid,id,error,target_events,"targetEvents",out,capacity);
}
size_t osg_gateway_cockpit_response(const osg_gateway_t *g,const char uid[25],uint32_t id,
    const char *error,char *out,size_t capacity) {
    return osg_gateway_cockpit_config_response(g,uid,id,error,NULL,out,capacity);
}
size_t osg_gateway_cockpit_request_response(const osg_gateway_t *g,const char uid[25],
    const osg_gateway_cockpit_request_t *r,const char *error,char *out,size_t capacity) {
    if(!r) return 0;
    if(r->action==OSG_GW_COCKPIT_PUBLICATION && r->publication_action==OSG_GW_PUBLICATION_INSPECT) {
        bool owned=!error && g && r->epoch==g->epoch && osg_gateway_publication_inspect(g,r->owner,r->publication_lease);
        return module_response(g,uid,r->id,error,boolean(owned),"publicationOwned",out,capacity);
    }
    return osg_gateway_cockpit_response(g,uid,r->id,error,out,capacity);
}
