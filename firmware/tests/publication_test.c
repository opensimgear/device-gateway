#include "opensimgear/gateway_publication.h"
#include "opensimgear/gateway_layout.h"
#include "opensimgear/gateway_cockpit.h"
#include "opensimgear/gateway_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t owner[16]={1},other[16]={2};
static osg_gateway_identity_t identity(unsigned i) {osg_gateway_identity_t id={{0}};id.bytes[15]=(uint8_t)i;return id;}
static osg_gateway_message_t announce(unsigned id) {
    return (osg_gateway_message_t){.type=OSG_GW_ANNOUNCE,.identity=identity(id),.boot_session=100,
        .source_node=(uint8_t)(40+id),.configuration_generation=1,.profile={.button_count=8,.revision=1}};
}
typedef struct {unsigned writes;uint8_t bytes[OSG_GW_REGISTRY_RECORD_SIZE];} flash_t;
static bool persist(void *context,const osg_gateway_registry_t *r) {
    flash_t *f=context;f->writes++;return osg_gateway_registry_encode(r,f->bytes,sizeof f->bytes)==sizeof f->bytes;
}
static void discover(osg_gateway_t *g,unsigned id,uint32_t now) {
    osg_gateway_message_t m=announce(id),reply;assert(osg_gateway_receive(g,&m,now,&reply)==OSG_GW_RX_ASSIGNMENT);
}
static void fixture(osg_gateway_t *g,flash_t *f,unsigned count) {
    assert(osg_gateway_init(g,65536,NULL));
    osg_gateway_identity_t publisher=identity(200);assert(osg_gateway_publication_identity(g,&publisher));
    for(unsigned i=1;i<=count;i++) discover(g,i,0);
    osg_gateway_apply_layout(g);assert(!osg_gateway_needs_apply(g));
    memset(f,0,sizeof *f);assert(osg_gateway_registry_encode(&g->configured,f->bytes,sizeof f->bytes)==sizeof f->bytes);
    osg_gateway_set_running(g,true);
}
static void cycle(osg_gateway_t *g,osg_gateway_layout_t *s,flash_t *f,uint32_t now) {
    assert(osg_gateway_layout_service(s,g,now,true,persist,f)==OSG_GATEWAY_LAYOUT_IDLE);
    if(!s->pending) return;
    assert(osg_gateway_layout_service(s,g,now+100U,true,persist,f)==OSG_GATEWAY_LAYOUT_DETACH);
    assert(osg_gateway_layout_activate(s,g,now+100U));
    assert(osg_gateway_layout_service(s,g,now+350U,true,persist,f)==OSG_GATEWAY_LAYOUT_ATTACH);
    osg_gateway_publication_reconnect(g,now+350U,true);
}
static void unchanged(const osg_gateway_t *g,const flash_t *f,const uint8_t *bytes) {
    uint8_t record[OSG_GW_REGISTRY_RECORD_SIZE];
    assert(!f->writes);assert(!memcmp(bytes,f->bytes,sizeof f->bytes));
    assert(osg_gateway_registry_encode(&g->configured,record,sizeof record)==sizeof record);
    assert(!memcmp(bytes,record,sizeof record));assert(!osg_gateway_needs_apply(g));
}
static void restore_test(void) {
    osg_gateway_t g;flash_t f;fixture(&g,&f,2);osg_gateway_layout_t s={0};
    uint8_t bytes[sizeof f.bytes],descriptor[4096];memcpy(bytes,f.bytes,sizeof bytes);
    size_t before=osg_gateway_descriptor(&g,descriptor,sizeof descriptor);assert(before);
    uint32_t revision=g.publication_revision,old_module_lease=g.modules[0].lease;
    assert(!osg_gateway_publication_claim(&g,owner,revision,1,0,5000,10));
    uint32_t lease=g.publication.lease.ticket.lease_id,expiry=g.publication.lease.ticket.expires_at;
    assert(!osg_gateway_publication_claim(&g,owner,revision,1,0,5000,11));assert(g.publication.lease.ticket.expires_at==expiry);
    assert(!strcmp(osg_gateway_publication_claim(&g,other,revision,1,0,5000,12),"publication_busy"));
    assert(!strcmp(osg_gateway_publication_claim(&g,owner,revision,1,1,5000,13),"publication_busy"));
    cycle(&g,&s,&f,20);
    assert(!g.active.slots[0].enabled && g.active.slots[1].enabled && g.active.slots[1].order==0);
    assert(g.modules[0].lease>old_module_lease && !g.modules[0].has_input);
    assert(osg_gateway_descriptor(&g,descriptor,sizeof descriptor)<before);unchanged(&g,&f,bytes);
    assert(!osg_gateway_disable(&g,&g.modules[0].identity));assert(!osg_gateway_disable(&g,&g.modules[1].identity));
    osg_gateway_config_t c={0};osg_gateway_cockpit_request_t r={.id=1,.epoch=g.epoch,.action=OSG_GW_COCKPIT_CONFIG,.identity=g.modules[0].identity,.boot=100};strcpy(r.command,"show");
    assert(!strcmp(osg_gateway_config_start(&c,&g,&r,400),"publication_busy"));
    assert(!strcmp(osg_gateway_publication_renew(&g,other,lease,5000,401),"publication_not_owned"));
    assert(!osg_gateway_publication_renew(&g,owner,lease,5000,402));
    assert(!osg_gateway_publication_release(&g,owner,lease,403));assert(g.publication.lease.phase==OSG_PUBLICATION_RESTORING && !g.running);
    cycle(&g,&s,&f,410);assert(g.publication.lease.phase==OSG_PUBLICATION_IDLE);
    assert(g.active.slots[0].enabled && g.active.slots[0].order==0 && g.active.slots[1].order==1);
    assert(osg_gateway_descriptor(&g,descriptor,sizeof descriptor)==before);unchanged(&g,&f,bytes);
    assert(!osg_gateway_publication_release(&g,owner,lease,800));assert(!f.writes);
}
static void disabled_and_arrival_test(void) {
    osg_gateway_t g;flash_t f;fixture(&g,&f,2);assert(osg_gateway_disable(&g,&g.modules[0].identity));osg_gateway_apply_layout(&g);
    assert(osg_gateway_registry_encode(&g.configured,f.bytes,sizeof f.bytes)==sizeof f.bytes);
    osg_gateway_layout_t s={0};uint32_t revision=g.publication_revision;
    assert(!osg_gateway_publication_claim(&g,owner,revision,1,1,5000,10));cycle(&g,&s,&f,20);
    assert(g.active.slots[0].enabled && g.active.slots[0].order==1 && g.active.slots[1].order==0);assert(!f.writes);
    discover(&g,3,400);assert(g.publication_revision!=revision);
    assert(osg_gateway_needs_apply(&g));
    assert(osg_gateway_layout_service(&s,&g,401,true,persist,&f)==OSG_GATEWAY_LAYOUT_IDLE);
    cycle(&g,&s,&f,501);assert(f.writes==1);
    assert(!g.configured.slots[0].enabled && g.configured.slots[2].enabled);
    assert(g.active.slots[0].enabled && g.active.slots[0].order==1 && g.active.slots[2].order==2);
    assert(g.publication.lease.phase==OSG_PUBLICATION_OWNED);
    osg_gateway_registry_t decoded;assert(osg_gateway_registry_decode(f.bytes,sizeof f.bytes,&decoded));assert(!decoded.slots[0].enabled);
    uint32_t lease=g.publication.lease.ticket.lease_id;assert(!osg_gateway_publication_release(&g,owner,lease,900));
    cycle(&g,&s,&f,910);assert(f.writes==1 && !g.active.slots[0].enabled && g.active.slots[2].enabled);
    assert(g.active.slots[1].order==0 && g.active.slots[2].order==1);
}
static void expiry_and_fence_test(void) {
    osg_gateway_t g;flash_t f;fixture(&g,&f,1);osg_gateway_layout_t s={0};uint32_t revision=g.publication_revision;
    assert(!osg_gateway_publication_claim(&g,owner,revision,1,0,1000,10));cycle(&g,&s,&f,20);
    uint8_t descriptor[4096];assert(!osg_gateway_descriptor(&g,descriptor,sizeof descriptor));
    osg_publication_ticket_t old=g.publication.lease.ticket;
    osg_gateway_publication_tick(&g,1010);assert(g.publication.lease.phase==OSG_PUBLICATION_RESTORING);
    assert(!osg_gateway_publication_admit(&g,&old,false,1010));cycle(&g,&s,&f,1011);
    assert(g.active.slots[0].enabled && !f.writes && !g.modules[0].has_input);
    assert(!strcmp(osg_gateway_publication_claim(&g,owner,revision,1,0,1000,1400),"stale_publication_revision"));
    revision=g.publication_revision;assert(!osg_gateway_publication_claim(&g,owner,revision,1,0,250,1410));
    assert(osg_gateway_layout_service(&s,&g,1420,false,persist,&f)==OSG_GATEWAY_LAYOUT_IDLE && s.pending);
    /* A non-reading client cannot pin the claim past expiry and later apply it. */
    assert(osg_gateway_layout_service(&s,&g,2520,false,persist,&f)==OSG_GATEWAY_LAYOUT_IDLE);
    assert(!s.pending && !g.reserved_leases && g.active.slots[0].enabled && !f.writes);
}
static void reconnect_and_loss_test(void) {
    osg_gateway_t g;flash_t f;fixture(&g,&f,1);osg_gateway_layout_t s={0};
    assert(!osg_gateway_publication_claim(&g,owner,g.publication_revision,1,0,5000,1));
    assert(osg_gateway_layout_service(&s,&g,2,true,persist,&f)==OSG_GATEWAY_LAYOUT_IDLE);
    assert(osg_gateway_layout_service(&s,&g,102,true,persist,&f)==OSG_GATEWAY_LAYOUT_DETACH);assert(osg_gateway_layout_activate(&s,&g,102));
    uint64_t generation=g.publication.route_generation;assert(g.publication.expected_reconnect);
    osg_gateway_publication_reconnect(&g,300,false);assert(g.publication.lease.phase==OSG_PUBLICATION_OWNED && g.publication.route_generation==generation);
    osg_gateway_publication_reconnect(&g,1602,false);assert(g.publication.lease.phase==OSG_PUBLICATION_RESTORING && g.publication.route_generation>generation);
    assert(osg_gateway_layout_service(&s,&g,1603,true,persist,&f)==OSG_GATEWAY_LAYOUT_ATTACH);cycle(&g,&s,&f,1604);assert(g.active.slots[0].enabled && !f.writes);
    assert(!osg_gateway_publication_claim(&g,owner,g.publication_revision,1,0,5000,2000));cycle(&g,&s,&f,2001);
    osg_gateway_publication_transport_lost(&g,2400);assert(g.publication.lease.phase==OSG_PUBLICATION_RESTORING);cycle(&g,&s,&f,2401);
    assert(g.active.slots[0].enabled && !f.writes);
    /* Module reboot revokes before a new application route can deliver. */
    assert(!osg_gateway_publication_claim(&g,owner,g.publication_revision,1,0,5000,2800));cycle(&g,&s,&f,2801);
    osg_gateway_message_t m=announce(1),reply;m.boot_session++;
    assert(osg_gateway_receive(&g,&m,3200,&reply)==OSG_GW_RX_ASSIGNMENT);osg_gateway_publication_tick(&g,3201);
    assert(g.publication.lease.phase==OSG_PUBLICATION_RESTORING);cycle(&g,&s,&f,3202);assert(g.active.slots[0].enabled && !f.writes);
    osg_gateway_t reboot;assert(osg_gateway_init(&reboot,131072,&g.configured));assert(reboot.active.slots[0].enabled && reboot.publication.lease.phase==OSG_PUBLICATION_IDLE);
}
static void wire_test(void) {
    osg_gateway_t g;flash_t f;fixture(&g,&f,32);osg_gateway_cockpit_request_t r;bool apply=true;char command[384],json[32768];
    int n=snprintf(command,sizeof command,"cockpit 4294967295 65536 publication claim 00112233445566778899aabbccddeeff %lu ffffffff 00000000 5000",(unsigned long)g.publication_revision);
    assert(n>0 && n<384 && osg_gateway_cockpit_parse(command,&r));
    assert(!osg_gateway_cockpit_execute_at(&g,&r,NULL,NULL,&apply,1) && !apply);
    assert(g.publication.lease.original.count==32);
    assert(!strcmp(osg_gateway_cockpit_execute(&g,&r,NULL,NULL,&apply),"publication_clock_required"));
    assert(osg_gateway_cockpit_response(&g,"0123456789abcdef01234567",r.id,NULL,json,sizeof json));
    assert(strstr(json,"\"controllerPublicationOverride\":{\"schema\":1") && strstr(json,"\"publicationSlot\":31") && strstr(json,"\"phase\":\"owned\""));
    assert(!strstr(json,"00112233445566778899aabbccddeeff"));
    snprintf(command,sizeof command,"cockpit 1 65536 publication renew 00112233445566778899aabbccddeeff %lu 4000",(unsigned long)g.publication.lease.ticket.lease_id);
    assert(osg_gateway_cockpit_parse(command,&r));assert(!osg_gateway_cockpit_execute_at(&g,&r,NULL,NULL,&apply,2));assert(g.publication.lease.ticket.expires_at==4002);
    snprintf(command,sizeof command,"cockpit 1 65536 publication release 00112233445566778899aabbccddeeff %lu",(unsigned long)g.publication.lease.ticket.lease_id);
    assert(osg_gateway_cockpit_parse(command,&r));assert(!osg_gateway_cockpit_execute_at(&g,&r,NULL,NULL,&apply,3));assert(g.publication.lease.phase==OSG_PUBLICATION_IDLE);
    const char *bad[]={"cockpit 1 65536 publication claim 00112233445566778899aabbccddeeff 1 0000000 00000000 5000",
        "cockpit 1 65536 publication claim 00112233445566778899aabbccddeeff 1 00000001 00000002 5000",
        "cockpit 1 65536 publication claim 00112233445566778899aabbccddeeff 1 00000001 00000000 5001",
        "cockpit 1 65536 publication renew 00112233445566778899aabbccddeeff 0 5000",
        "cockpit 1 65536 publication release 00112233445566778899aabbccddeeff 1 trailing"};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++) assert(!osg_gateway_cockpit_parse(bad[i],&r));
    assert(osg_gateway_cockpit_parse("cockpit 1 65536 publication state",&r));assert(!osg_gateway_cockpit_execute_at(&g,&r,NULL,NULL,&apply,4));
    assert(osg_gateway_cockpit_parse("cockpit 1 65536 publication describe",&r));assert(!osg_gateway_cockpit_execute_at(&g,&r,NULL,NULL,&apply,5));
}
static void delivery_revocation_test(void) {
    osg_gateway_t g;flash_t f;fixture(&g,&f,1);assert(osg_gateway_disable(&g,&g.modules[0].identity));osg_gateway_apply_layout(&g);
    osg_gateway_layout_t s={0};assert(!osg_gateway_publication_claim(&g,owner,g.publication_revision,1,1,5000,1));cycle(&g,&s,&f,2);
    osg_gateway_set_running(&g,true);osg_gateway_module_t *m=&g.modules[0];
    osg_gateway_message_t input={.type=OSG_GW_INPUT,.source_node=m->source_node,.node=m->node,.boot_session=m->boot_session,.lease=m->lease,.sequence=1,.report={1}};
    assert(osg_gateway_receive(&g,&input,400,NULL)==OSG_GW_RX_ACCEPTED);
    uint8_t packet[2]={g.active.slots[0].report_id,0};osg_gateway_delivery_t d;osg_gateway_message_t ack;
    assert(osg_gateway_delivery_prepare(&g,0,packet+1,1,&d)==1 && d.valid && packet[1]==1);
    assert(osg_gateway_delivery_complete(&g,&d,packet,2,&ack));
    char json[32768];assert(osg_gateway_cockpit_response(&g,"0123456789abcdef01234567",1,NULL,json,sizeof json));
    assert(strstr(json,"\"configuredEnabled\":false,\"enabled\":false,\"published\":true") && strstr(json,"\"report\":\"01\""));
    assert(!osg_gateway_publication_release(&g,owner,g.publication.lease.ticket.lease_id,401));
    osg_gateway_set_running(&g,true);input.sequence++;
    assert(osg_gateway_receive(&g,&input,402,NULL)==OSG_GW_RX_ACCEPTED);
    assert(!osg_gateway_delivery_complete(&g,&d,packet,2,&ack));
    assert(osg_gateway_delivery_prepare(&g,0,packet+1,1,&d)==1 && !d.valid && packet[1]==0);
    cycle(&g,&s,&f,403);assert(!g.active.slots[0].enabled && !f.writes);
}
static void attention_preservation_test(void) {
    osg_gateway_t g;flash_t f;fixture(&g,&f,2);osg_gateway_layout_t s={0};
    assert(!osg_gateway_publication_claim(&g,owner,g.publication_revision,1,0,5000,1));cycle(&g,&s,&f,2);
    /* Exact newer independently written descriptor: preserve it on release. */
    g.active.slots[0].enabled=true;g.active.slots[0].order=1;g.active.slots[1].order=0;
    assert(osg_gateway_registry_valid(&g.active));
    assert(!strcmp(osg_gateway_publication_release(&g,owner,g.publication.lease.ticket.lease_id,400),"publication_preserved"));
    assert(g.active.slots[0].order==1 && !g.publication.dirty && g.publication.lease.phase==OSG_PUBLICATION_IDLE);
    fixture(&g,&f,1);memset(&s,0,sizeof s);
    assert(!osg_gateway_publication_claim(&g,owner,g.publication_revision,1,0,5000,1));cycle(&g,&s,&f,2);
    g.active.slots[0].profile.revision++;
    osg_gateway_publication_transport_lost(&g,400);
    assert(g.publication.lease.phase==OSG_PUBLICATION_ATTENTION && !g.publication.dirty);
    assert(!osg_gateway_publication_deliverable(&g,&g.active.slots[0].identity));
    assert(!strcmp(osg_gateway_publication_claim(&g,owner,g.publication_revision,1,0,5000,401),"publication_busy"));
    assert(!f.writes);
}
static void lost_confirmation_test(void) {
    osg_gateway_t g;flash_t f;fixture(&g,&f,1);osg_gateway_layout_t s={0};
    assert(!osg_gateway_publication_claim(&g,owner,g.publication_revision,1,0,5000,1));
    osg_publication_ticket_t ticket=g.publication.lease.ticket;
    assert(osg_gateway_layout_service(&s,&g,2,true,persist,&f)==OSG_GATEWAY_LAYOUT_IDLE);
    assert(osg_gateway_publication_admit(&g,&ticket,false,3));
    assert(osg_gateway_activate_prepared(&g,&s.target,true,&s.activation));
    /* Device IO succeeded, but readback confirmation was lost. */
    osg_gateway_publication_tick(&g,5001);
    assert(g.publication.lease.phase==OSG_PUBLICATION_RESTORING);
    assert(!osg_gateway_publication_admit(&g,&ticket,false,5001));
    memset(&s,0,sizeof s);cycle(&g,&s,&f,5002);assert(g.active.slots[0].enabled && !f.writes);
}
static void bounds_test(void) {
    osg_gateway_t g;flash_t f;fixture(&g,&f,1);
    uint32_t revision=g.publication_revision;
    assert(!strcmp(osg_gateway_publication_claim(&g,owner,revision,0,0,5000,1),"invalid_publication_request"));
    assert(!strcmp(osg_gateway_publication_claim(&g,owner,revision,1,2,5000,1),"invalid_publication_request"));
    assert(!strcmp(osg_gateway_publication_claim(&g,owner,revision,2,0,5000,1),"publication_target_not_ready"));
    g.modules[0].direct_usb=true;assert(!strcmp(osg_gateway_publication_claim(&g,owner,revision,1,0,5000,1),"publication_target_not_ready"));g.modules[0].direct_usb=false;
    assert(!strcmp(osg_gateway_publication_claim(&g,owner,revision,1,0,5001,1),"invalid_publication_request"));
    g.publication_revision=UINT32_MAX;osg_gateway_publication_changed(&g);assert(!g.publication_revision);
    assert(!strcmp(osg_gateway_publication_claim(&g,owner,revision,1,0,5000,1),"stale_publication_revision"));
    fixture(&g,&f,1);g.lease_counter=65535;
    assert(!osg_gateway_publication_claim(&g,owner,g.publication_revision,1,0,5000,1));osg_gateway_layout_t s={0};
    assert(osg_gateway_layout_service(&s,&g,2,true,persist,&f)==OSG_GATEWAY_LAYOUT_IDLE);
    assert(!s.pending && !strcmp(g.layout_error,"lease_exhausted") && !f.writes && g.active.slots[0].enabled);
    assert(!osg_gateway_publication_release(&g,owner,g.publication.lease.ticket.lease_id,3));
}
static void automatic_profile_writer_test(void) {
    osg_gateway_t g;flash_t f;fixture(&g,&f,2);osg_gateway_layout_t s={0};
    assert(!osg_gateway_publication_claim(&g,owner,g.publication_revision,1,0,5000,1));cycle(&g,&s,&f,2);
    osg_gateway_message_t m=announce(2),reply;m.configuration_generation++;m.profile.button_count=0;m.profile.revision++;
    assert(osg_gateway_receive(&g,&m,400,&reply)==OSG_GW_RX_ASSIGNMENT);
    assert(osg_gateway_registry_valid(&g.pending) && !g.pending.slots[1].enabled);
    assert(g.publication.lease.phase==OSG_PUBLICATION_RESTORING);cycle(&g,&s,&f,401);
    assert(g.active.slots[0].enabled && !f.writes);
    assert(osg_gateway_layout_service(&s,&g,800,true,persist,&f)==OSG_GATEWAY_LAYOUT_IDLE);cycle(&g,&s,&f,900);
    assert(g.active.slots[0].enabled && !g.active.slots[1].enabled && f.writes==1 && osg_gateway_registry_valid(&g.configured));
}
static void wrapped_clock_test(void) {
    osg_gateway_t g;flash_t f;fixture(&g,&f,1);osg_gateway_layout_t s={0};
    uint32_t start=UINT32_MAX-200U;
    assert(!osg_gateway_publication_claim(&g,owner,g.publication_revision,1,0,500,start));cycle(&g,&s,&f,start+1U);
    assert(g.publication.lease.phase==OSG_PUBLICATION_OWNED && !g.active.slots[0].enabled);
    osg_gateway_publication_tick(&g,start+500U);assert(g.publication.lease.phase==OSG_PUBLICATION_RESTORING);cycle(&g,&s,&f,start+501U);
    assert(g.active.slots[0].enabled && !f.writes);
}
static void cancelled_saved_arrival_test(void) {
    osg_gateway_t g;flash_t f;fixture(&g,&f,1);osg_gateway_layout_t s={0};
    assert(!osg_gateway_publication_claim(&g,owner,g.publication_revision,1,1,5000,1));cycle(&g,&s,&f,2);
    discover(&g,2,400);assert(osg_gateway_layout_service(&s,&g,401,true,persist,&f)==OSG_GATEWAY_LAYOUT_IDLE);
    assert(osg_gateway_layout_service(&s,&g,501,true,persist,&f)==OSG_GATEWAY_LAYOUT_IDLE && s.pending && f.writes==1);
    assert(g.configured.slots[1].enabled && !g.active.slots[1].occupied);
    assert(!osg_gateway_publication_release(&g,owner,g.publication.lease.ticket.lease_id,550));
    assert(g.publication.lease.phase==OSG_PUBLICATION_IDLE);
    assert(osg_gateway_layout_service(&s,&g,601,true,persist,&f)==OSG_GATEWAY_LAYOUT_IDLE && s.pending);
    assert(osg_gateway_layout_service(&s,&g,701,true,persist,&f)==OSG_GATEWAY_LAYOUT_DETACH);
    assert(osg_gateway_layout_activate(&s,&g,701));
    assert(g.active.slots[1].enabled && f.writes==1 && !g.reserved_leases);
}
static void inspect_racing_owner_test(void) {
    osg_gateway_t g;flash_t f;fixture(&g,&f,1);osg_gateway_layout_t s={0};
    uint32_t revision=g.publication_revision;
    /* Foreign owner wins the same revision/masks, and its reply is lost. */
    assert(!osg_gateway_publication_claim(&g,other,revision,1,0,5000,1));
    assert(!strcmp(osg_gateway_publication_claim(&g,owner,revision,1,0,5000,2),"publication_busy"));
    uint32_t lease=g.publication.lease.ticket.lease_id,expiry=g.publication.lease.ticket.expires_at;
    uint64_t transition=g.publication.lease.ticket.transition;
    osg_gateway_cockpit_request_t r;bool apply=true;char command[384],json[32768];
    snprintf(command,sizeof command,"cockpit 1 65536 publication inspect 01000000000000000000000000000000 %lu",(unsigned long)lease);
    assert(osg_gateway_cockpit_parse(command,&r));assert(!osg_gateway_cockpit_execute_at(&g,&r,NULL,NULL,&apply,3));
    assert(osg_gateway_cockpit_request_response(&g,"0123456789abcdef01234567",&r,NULL,json,sizeof json));assert(strstr(json,"\"publicationOwned\":false"));
    memcpy(r.owner,other,16);r.id++;
    assert(osg_gateway_cockpit_request_response(&g,"0123456789abcdef01234567",&r,NULL,json,sizeof json));assert(strstr(json,"\"publicationOwned\":true"));
    r.publication_lease++;assert(osg_gateway_cockpit_request_response(&g,"0123456789abcdef01234567",&r,NULL,json,sizeof json));assert(strstr(json,"\"publicationOwned\":false"));r.publication_lease--;
    r.epoch++;assert(!strcmp(osg_gateway_cockpit_execute_at(&g,&r,NULL,NULL,&apply,4),"stale_epoch"));
    assert(osg_gateway_cockpit_request_response(&g,"0123456789abcdef01234567",&r,"stale_epoch",json,sizeof json));assert(strstr(json,"\"publicationOwned\":false"));r.epoch--;
    assert(g.publication.lease.ticket.expires_at==expiry && g.publication.lease.ticket.transition==transition);
    cycle(&g,&s,&f,10);assert(osg_gateway_publication_inspect(&g,other,lease) && !osg_gateway_publication_inspect(&g,owner,lease));
    assert(!osg_gateway_publication_release(&g,other,lease,400));assert(osg_gateway_publication_inspect(&g,other,lease));cycle(&g,&s,&f,401);
    assert(!osg_gateway_publication_inspect(&g,other,lease) && !f.writes);
}
static void configured_writer_preservation_test(void) {
    osg_gateway_t g;flash_t f;fixture(&g,&f,1);osg_gateway_layout_t s={0};
    assert(!osg_gateway_publication_claim(&g,owner,g.publication_revision,1,0,5000,1));cycle(&g,&s,&f,2);
    /* Model an independently committed native configured choice. */
    g.configured.slots[0].enabled=false;g.configured.slots[0].order=255;g.configured.generation++;g.pending=g.configured;
    assert(persist(&f,&g.configured));
    assert(!strcmp(osg_gateway_publication_release(&g,owner,g.publication.lease.ticket.lease_id,400),"publication_preserved"));
    assert(g.publication.lease.phase==OSG_PUBLICATION_IDLE && !g.publication.dirty && !g.active.slots[0].enabled);
    assert(osg_gateway_layout_service(&s,&g,401,true,persist,&f)==OSG_GATEWAY_LAYOUT_IDLE && !s.pending && f.writes==1);
    /* Recheck immediately before IO, after release admitted a restore plan. */
    fixture(&g,&f,1);memset(&s,0,sizeof s);
    assert(!osg_gateway_publication_claim(&g,owner,g.publication_revision,1,0,5000,1));cycle(&g,&s,&f,2);
    assert(!osg_gateway_publication_release(&g,owner,g.publication.lease.ticket.lease_id,400));
    assert(osg_gateway_layout_service(&s,&g,401,true,persist,&f)==OSG_GATEWAY_LAYOUT_IDLE && s.pending);
    g.configured.slots[0].enabled=false;g.configured.slots[0].order=255;g.configured.generation++;g.pending=g.configured;
    assert(persist(&f,&g.configured));
    assert(osg_gateway_layout_service(&s,&g,501,true,persist,&f)==OSG_GATEWAY_LAYOUT_DETACH);
    assert(!osg_gateway_layout_activate(&s,&g,501));
    assert(g.publication.lease.phase==OSG_PUBLICATION_IDLE && g.publication.lease.outcome==OSG_PUBLICATION_PRESERVED);
    assert(!g.active.slots[0].enabled && !g.reserved_leases && f.writes==1);
}
static void vectors(void) {
    osg_gateway_t g;flash_t f;fixture(&g,&f,1);osg_gateway_layout_t s={0};char json[32768];
    assert(osg_gateway_cockpit_response(&g,"0123456789abcdef01234567",1,NULL,json,sizeof json));fputs(json,stdout);
    assert(!osg_gateway_publication_claim(&g,owner,g.publication_revision,1,0,5000,1));cycle(&g,&s,&f,2);
    assert(osg_gateway_cockpit_response(&g,"0123456789abcdef01234567",2,NULL,json,sizeof json));fputs(json,stdout);
    osg_gateway_cockpit_request_t r={.id=3,.epoch=g.epoch,.action=OSG_GW_COCKPIT_PUBLICATION,.publication_action=OSG_GW_PUBLICATION_INSPECT,.publication_lease=g.publication.lease.ticket.lease_id};memcpy(r.owner,owner,16);
    assert(osg_gateway_cockpit_request_response(&g,"0123456789abcdef01234567",&r,NULL,json,sizeof json));fputs(json,stdout);
    assert(!osg_gateway_publication_release(&g,owner,g.publication.lease.ticket.lease_id,400));cycle(&g,&s,&f,401);
    assert(osg_gateway_cockpit_response(&g,"0123456789abcdef01234567",4,NULL,json,sizeof json));fputs(json,stdout);
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--publication-vectors")) {vectors();return 0;}
    restore_test();disabled_and_arrival_test();expiry_and_fence_test();reconnect_and_loss_test();wire_test();delivery_revocation_test();attention_preservation_test();lost_confirmation_test();bounds_test();automatic_profile_writer_test();wrapped_clock_test();cancelled_saved_arrival_test();inspect_racing_owner_test();configured_writer_preservation_test();puts("gateway volatile publication: all tests passed");return 0;
}
