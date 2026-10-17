#include "opensimgear/publication.h"
#include <string.h>

static bool nonzero(const uint8_t id[16]) {
    if(!id) return false;
    unsigned bits=0;for(unsigned i=0;i<16;i++) bits|=id[i];return bits!=0;
}
static bool scope_valid(const osg_publication_scope_t *s) {
    return s && nonzero(s->publisher) && s->boot && s->route_generation;
}
static bool scope_equal(const osg_publication_scope_t *a,const osg_publication_scope_t *b) {
    return a && b && !memcmp(a->publisher,b->publisher,16) && a->boot==b->boot && a->route_generation==b->route_generation;
}
static bool fixed_equal(const osg_publication_target_t *a,const osg_publication_target_t *b) {
    return !memcmp(a->identity,b->identity,16) && a->boot==b->boot &&
        a->configuration_generation==b->configuration_generation && a->profile_revision==b->profile_revision &&
        a->input_capable==b->input_capable && a->report_id==b->report_id;
}
bool osg_publication_view_valid(const osg_publication_view_t *v) {
    if(!v || !v->count || v->count>OSG_PUBLICATION_MAX_TARGETS) return false;
    for(unsigned i=0;i<v->count;i++) {
        const osg_publication_target_t *t=&v->targets[i];
        if(!nonzero(t->identity) || !t->boot || !t->configuration_generation || !t->profile_revision ||
           t->report_id>32 || (t->enabled ? (!t->input_capable || t->order>=32) : t->order!=255)) return false;
        for(unsigned j=0;j<i;j++) {
            const osg_publication_target_t *p=&v->targets[j];
            if(!memcmp(t->identity,p->identity,16) || (t->enabled && p->enabled &&
               (t->order==p->order || (t->report_id && t->report_id==p->report_id)))) return false;
        }
    }
    return true;
}
static bool view_equal(const osg_publication_view_t *a,const osg_publication_view_t *b,bool fixed) {
    if(!osg_publication_view_valid(a) || !osg_publication_view_valid(b) || a->count!=b->count) return false;
    for(unsigned i=0;i<a->count;i++) if(!fixed_equal(&a->targets[i],&b->targets[i]) ||
        (!fixed && (a->targets[i].enabled!=b->targets[i].enabled || a->targets[i].order!=b->targets[i].order))) return false;
    return true;
}
static bool ttl_valid(uint32_t ttl) {return ttl>=OSG_PUBLICATION_MIN_TTL_MS && ttl<=OSG_PUBLICATION_MAX_TTL_MS;}
static bool time_valid(const osg_publication_t *s,uint32_t now) {return !s->clock_started || (int32_t)(now-s->now)>=0;}
static void clock_at(osg_publication_t *s,uint32_t now) {s->now=now;s->clock_started=true;}
static bool expired(const osg_publication_t *s,uint32_t now) {return (int32_t)(now-s->ticket.expires_at)>=0;}
static bool identity(const osg_publication_t *s,const osg_publication_scope_t *scope,const uint8_t owner[16],uint32_t lease) {
    return scope_valid(scope) && nonzero(owner) && lease && lease==s->ticket.lease_id && scope_equal(&s->scope,scope) && !memcmp(owner,s->owner,16);
}
static bool ticket_equal(const osg_publication_t *s,const osg_publication_ticket_t *t) {
    /* Renewal does not invalidate an already queued immutable transition. */
    return t && t->lease_id==s->ticket.lease_id && t->transition==s->ticket.transition;
}
void osg_publication_init(osg_publication_t *s) {if(s) memset(s,0,sizeof *s);}
osg_publication_result_t osg_publication_claim_current(osg_publication_t *s,
    const osg_publication_scope_t *scope,const uint8_t owner[16],const osg_publication_view_t *actual,
    const osg_publication_view_t *desired,uint32_t now,uint32_t ttl,osg_publication_ticket_t *ticket) {
    if(!s || !ticket || !scope_valid(scope) || !nonzero(owner) || !ttl_valid(ttl) || !view_equal(actual,desired,true)) return OSG_PUBLICATION_INVALID;
    if(!time_valid(s,now)) return OSG_PUBLICATION_STALE_TIME;
    if(s->phase!=OSG_PUBLICATION_IDLE) {
        if(s->phase!=OSG_PUBLICATION_OWNED || !scope_equal(&s->scope,scope) || memcmp(owner,s->owner,16) || !view_equal(desired,&s->desired,false)) return OSG_PUBLICATION_BUSY;
        if(expired(s,now)) return OSG_PUBLICATION_EXPIRED;
        /* Local read may now reflect desired; original must never be recaptured. */
        if(!view_equal(actual,&s->last_owned,false) && !(s->apply_started && view_equal(actual,&s->desired,false))) return OSG_PUBLICATION_BUSY;
        clock_at(s,now);*ticket=s->ticket;return OSG_PUBLICATION_OK;
    }
    if(s->lease_counter==UINT32_MAX || s->transition==UINT64_MAX) return OSG_PUBLICATION_EXHAUSTED;
    s->scope=*scope;memcpy(s->owner,owner,16);s->original=*actual;s->desired=*desired;s->last_owned=*actual;
    s->ticket=(osg_publication_ticket_t){++s->lease_counter,now+ttl,++s->transition};
    s->phase=OSG_PUBLICATION_OWNED;s->outcome=OSG_PUBLICATION_OK;s->apply_started=s->apply_confirmed=false;clock_at(s,now);*ticket=s->ticket;return OSG_PUBLICATION_OK;
}
osg_publication_result_t osg_publication_renew(osg_publication_t *s,const osg_publication_scope_t *scope,
    const uint8_t owner[16],uint32_t lease,uint32_t now,uint32_t ttl,osg_publication_ticket_t *ticket) {
    if(!s || !ticket || !ttl_valid(ttl)) return OSG_PUBLICATION_INVALID;
    if(!identity(s,scope,owner,lease) || s->phase!=OSG_PUBLICATION_OWNED) return OSG_PUBLICATION_NOT_OWNED;
    if(!time_valid(s,now)) return OSG_PUBLICATION_STALE_TIME;
    if(expired(s,now)) return OSG_PUBLICATION_EXPIRED;
    s->ticket.expires_at=now+ttl;clock_at(s,now);*ticket=s->ticket;return OSG_PUBLICATION_OK;
}
bool osg_publication_apply_allowed(const osg_publication_t *s,const osg_publication_scope_t *scope,
    const uint8_t owner[16],const osg_publication_ticket_t *ticket,const osg_publication_view_t *actual,uint32_t now) {
    return s && s->phase==OSG_PUBLICATION_OWNED && !s->apply_started && ticket && identity(s,scope,owner,ticket->lease_id) && ticket_equal(s,ticket) &&
        time_valid(s,now) && !expired(s,now) && view_equal(actual,&s->last_owned,false);
}
bool osg_publication_begin_apply(osg_publication_t *s,const osg_publication_scope_t *scope,
    const uint8_t owner[16],const osg_publication_ticket_t *ticket,const osg_publication_view_t *actual,uint32_t now) {
    if(!osg_publication_apply_allowed(s,scope,owner,ticket,actual,now)) return false;
    s->apply_started=true;clock_at(s,now);return true;
}
bool osg_publication_confirm_applied(osg_publication_t *s,const osg_publication_scope_t *scope,
    const uint8_t owner[16],const osg_publication_ticket_t *ticket,const osg_publication_view_t *actual,uint32_t now) {
    if(!s || s->phase!=OSG_PUBLICATION_OWNED || !s->apply_started || !ticket || !identity(s,scope,owner,ticket->lease_id) || !ticket_equal(s,ticket) ||
       !time_valid(s,now) || expired(s,now) || !view_equal(actual,&s->desired,false)) return false;
    s->last_owned=*actual;s->apply_confirmed=true;clock_at(s,now);return true;
}
static osg_publication_result_t revoke(osg_publication_t *s,const osg_publication_view_t *actual,uint32_t now) {
    clock_at(s,now);
    if(s->transition==UINT64_MAX) {s->phase=OSG_PUBLICATION_ATTENTION;s->outcome=OSG_PUBLICATION_EXHAUSTED;return s->outcome;}
    s->ticket.transition=++s->transition;
    if(!osg_publication_view_valid(actual)) {s->phase=OSG_PUBLICATION_ATTENTION;s->outcome=OSG_PUBLICATION_UNVERIFIED;}
    else if(view_equal(actual,&s->original,false)) {s->phase=OSG_PUBLICATION_IDLE;s->outcome=OSG_PUBLICATION_NO_CHANGE;}
    else if(view_equal(actual,&s->last_owned,false) ||
        (s->apply_started && !s->apply_confirmed && view_equal(actual,&s->desired,false))) {
        /* begin_apply recorded this exact native intent before IO. A current
         * local read can establish its restoration comparison point even when
         * the post-write confirmation was lost. Never recapture original. */
        s->last_owned=*actual;s->phase=OSG_PUBLICATION_RESTORING;s->outcome=OSG_PUBLICATION_RESTORE;
    }
    else {s->phase=OSG_PUBLICATION_IDLE;s->outcome=OSG_PUBLICATION_PRESERVED;}
    return s->outcome;
}
osg_publication_result_t osg_publication_release(osg_publication_t *s,const osg_publication_scope_t *scope,
    const uint8_t owner[16],uint32_t lease,const osg_publication_view_t *actual,uint32_t now) {
    if(!s) return OSG_PUBLICATION_INVALID;
    if(!identity(s,scope,owner,lease)) return OSG_PUBLICATION_NOT_OWNED;
    if(!time_valid(s,now)) return OSG_PUBLICATION_STALE_TIME;
    if(s->phase==OSG_PUBLICATION_IDLE || s->phase==OSG_PUBLICATION_RESTORING) return s->outcome;
    return revoke(s,actual,now);
}
osg_publication_result_t osg_publication_tick(osg_publication_t *s,const osg_publication_view_t *actual,uint32_t now) {
    if(!s) return OSG_PUBLICATION_INVALID;
    if(!time_valid(s,now)) return OSG_PUBLICATION_STALE_TIME;
    clock_at(s,now);
    if(s->phase==OSG_PUBLICATION_OWNED && expired(s,now)) return revoke(s,actual,now);
    return s->phase==OSG_PUBLICATION_IDLE?OSG_PUBLICATION_NO_CHANGE:s->outcome;
}
osg_publication_result_t osg_publication_invalidate(osg_publication_t *s,const osg_publication_scope_t *scope,
    const osg_publication_view_t *actual,uint32_t now) {
    if(!s || !scope_valid(scope)) return OSG_PUBLICATION_INVALID;
    if(!scope_equal(&s->scope,scope)) return OSG_PUBLICATION_NOT_OWNED;
    if(!time_valid(s,now)) return OSG_PUBLICATION_STALE_TIME;
    if(s->phase==OSG_PUBLICATION_IDLE || s->phase==OSG_PUBLICATION_RESTORING) return s->outcome;
    return revoke(s,actual,now);
}
bool osg_publication_restore_allowed(osg_publication_t *s,const osg_publication_ticket_t *ticket,const osg_publication_view_t *actual) {
    if(!s || s->phase!=OSG_PUBLICATION_RESTORING || !ticket_equal(s,ticket)) return false;
    if(!osg_publication_view_valid(actual)) {s->phase=OSG_PUBLICATION_ATTENTION;s->outcome=OSG_PUBLICATION_UNVERIFIED;return false;}
    if(!view_equal(actual,&s->last_owned,false)) {s->phase=OSG_PUBLICATION_IDLE;s->outcome=OSG_PUBLICATION_PRESERVED;return false;}
    return true;
}
bool osg_publication_confirm_restored(osg_publication_t *s,const osg_publication_ticket_t *ticket,const osg_publication_view_t *actual) {
    if(!s || s->phase!=OSG_PUBLICATION_RESTORING || !ticket_equal(s,ticket) || !view_equal(actual,&s->original,false)) return false;
    s->phase=OSG_PUBLICATION_IDLE;s->outcome=OSG_PUBLICATION_NO_CHANGE;return true;
}
