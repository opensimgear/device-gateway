#include "opensimgear/gateway_config.h"
#include "opensimgear/gateway_publication.h"
#include <string.h>

static bool live(const osg_gateway_config_t *c,const osg_gateway_t *g) {
    int index=osg_gateway_find_module(g,&c->route.identity);
    if(index<0 || g->epoch!=c->route.gateway_epoch) return false;
    const osg_gateway_module_t *m=&g->modules[index];
    return m->online && m->node==c->route.node && m->source_node==c->route.source_node &&
        m->boot_session==c->route.boot_session && m->lease==c->route.lease;
}
static void fail(osg_gateway_config_t *c,const char *token) {
    size_t n=strlen(token); memcpy(c->response,"ERR ",4); memcpy(c->response+4,token,n);
    memcpy(c->response+4+n,"\nnull",6); c->complete=true;
}
const char *osg_gateway_config_start(osg_gateway_config_t *c,const osg_gateway_t *g,
    const osg_gateway_cockpit_request_t *r,uint32_t now) {
    if(!c || !g || !r || !r->id || (r->action!=OSG_GW_COCKPIT_CONFIG && r->action!=OSG_GW_COCKPIT_ACTION && r->action!=OSG_GW_COCKPIT_EVENTS)) return "invalid_request";
    if(c->busy) return "configuration_busy";
    if(r->epoch!=g->epoch) return "stale_epoch";
    /* Management events and outputs stay independent. Input configuration has
     * a separate writer and cannot change a captured publication tuple. */
    if(r->action==OSG_GW_COCKPIT_CONFIG && osg_gateway_publication_busy(g,&r->identity)) return "publication_busy";
    int index=osg_gateway_find_module(g,&r->identity);
    if(index<0 || !g->modules[index].online) return "module_offline";
    const osg_gateway_module_t *m=&g->modules[index];
    if(r->boot!=m->boot_session) return "stale_module_boot";
    /* A freshly renewed publication lease has not necessarily reached the
     * device yet. Require Input/Status on it before sending a mutation once. */
    if(!m->has_sequence) return "configuration_route_not_ready";
    size_t length=strlen(r->command);
    bool action=r->action==OSG_GW_COCKPIT_ACTION;
    bool events=r->action==OSG_GW_COCKPIT_EVENTS;
    size_t prefix=(action || events) ? 7U : 0U;
    if(!length || length+prefix>=sizeof c->command) return "invalid_request";
    memset(c,0,sizeof *c); c->action=action;c->events=events;
    if(prefix) memcpy(c->command,events?"events ":"action ",prefix);
    memcpy(c->command+prefix,r->command,length+1);
    c->route=(osg_gateway_message_t){.type=OSG_GW_CONFIG_REQUEST,.identity=m->identity,
        .node=m->node,.source_node=m->source_node,.boot_session=m->boot_session,.lease=m->lease,
        .gateway_epoch=g->epoch,.sequence=r->id,.config_total=(uint16_t)(length+prefix)};
    c->started=now; c->busy=true; return NULL;
}
void osg_gateway_config_tick(osg_gateway_config_t *c,const osg_gateway_t *g,osg_cyphal_t *transport,uint32_t now) {
    if(!c || !c->busy || c->complete) return;
    if(!live(c,g)) { fail(c,"configuration_route_changed"); return; }
    if((uint32_t)(now-c->started)>=OSG_GATEWAY_CONFIG_TIMEOUT_MS) { fail(c,"configuration_timeout"); return; }
    if(c->sent>=c->route.config_total) return;
    osg_gateway_message_t m=c->route; m.config_offset=c->sent;
    unsigned remaining=m.config_total-c->sent;
    m.config_length=(uint8_t)(remaining>OSG_GW_CONFIG_CHUNK_SIZE ? OSG_GW_CONFIG_CHUNK_SIZE : remaining);
    memcpy(m.config_data,c->command+c->sent,m.config_length);
    if(osg_cyphal_send(transport,&m,now)) c->sent+=m.config_length;
}
bool osg_gateway_config_receive(osg_gateway_config_t *c,const osg_gateway_t *g,const osg_gateway_message_t *m) {
    if(!c || !m || !c->busy || c->complete || m->type!=OSG_GW_CONFIG_RESPONSE || !live(c,g) ||
       !osg_gateway_identity_equal(&m->identity,&c->route.identity) || m->node!=c->route.node ||
       m->source_node!=c->route.source_node || m->boot_session!=c->route.boot_session ||
       m->lease!=c->route.lease || m->gateway_epoch!=c->route.gateway_epoch || m->sequence!=c->route.sequence ||
       m->config_offset!=c->received || !m->config_length || m->config_length>OSG_GW_CONFIG_CHUNK_SIZE ||
       !m->config_total || m->config_total>=sizeof c->response ||
       (unsigned)m->config_offset+m->config_length>m->config_total ||
       (c->received && m->config_total!=c->total)) return false;
    c->total=m->config_total;
    memcpy(c->response+c->received,m->config_data,m->config_length); c->received+=m->config_length;
    if(c->received==c->total) { c->response[c->received]=0; c->complete=true; }
    return true;
}
bool osg_gateway_config_result(osg_gateway_config_t *c,const char **error,const char **json) {
    if(!c || !c->busy || !c->complete || !error || !json) return false;
    char *split=strchr(c->response,'\n');
    *error="invalid_module_response"; *json=NULL;
    if(split) {
        *split=0;
        if(!strcmp(c->response,"OK")) { *error=NULL; *json=split+1; }
        else if(!strncmp(c->response,"ERR ",4)) {
            bool valid=true;
            for(char *p=c->response+4;*p;p++) if(!((*p>='a' && *p<='z') || *p=='_')) valid=false;
            if(valid) { *error=c->response+4; *json=split+1; }
        }
    }
    c->busy=false; return true;
}
void osg_gateway_config_cancel(osg_gateway_config_t *c) { if(c) memset(c,0,sizeof *c); }
