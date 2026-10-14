#include "opensimgear/gateway.h"
#include "opensimgear/gateway_cockpit.h"
#include "opensimgear/gateway_config.h"
#include "opensimgear/gateway_layout.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static osg_gateway_identity_t identity(unsigned value) {
    osg_gateway_identity_t id={{0}}; id.bytes[15]=(uint8_t)value; return id;
}
static osg_gateway_profile_t profile(void) {
    return (osg_gateway_profile_t){.button_count=3,.axis_count=2,.revision=1,
        .axis_usage={0x30,0x31},.axis_neutral={0,32768}};
}
static osg_gateway_message_t announce(unsigned id) {
    return (osg_gateway_message_t){.type=OSG_GW_ANNOUNCE,.identity=identity(id),
        .boot_session=100,.source_node=(uint8_t)(20+id),.profile=profile(),.configuration_generation=1};
}
static osg_gateway_receive_result_t receive(osg_gateway_t *g,const osg_gateway_message_t *msg,
                                            uint32_t now,osg_gateway_message_t *reply) {
    return osg_gateway_receive(g,msg,now,reply);
}
static osg_gateway_message_t discover(osg_gateway_t *g,unsigned id,uint32_t now) {
    osg_gateway_message_t msg=announce(id),assignment; osg_gateway_message_t reply;
    assert(receive(g,&msg,now,&reply)==OSG_GW_RX_ASSIGNMENT);
    assignment=reply; assignment.source_node=msg.source_node; return assignment;
}
static osg_gateway_message_t input(osg_gateway_message_t assignment,uint32_t seq) {
    assignment.type=OSG_GW_INPUT; assignment.gateway_epoch=0; assignment.sequence=seq;
    memset(assignment.report,0,sizeof assignment.report); assignment.report[0]=5;
    assignment.report[1]=0x34; assignment.report[2]=0x12;
    assignment.report[3]=0xff; assignment.report[4]=0xff; return assignment;
}
static void check_report(osg_gateway_t *g,unsigned slot,bool active) {
    uint8_t report[24]; assert(osg_gateway_report(g,slot,report,sizeof report)==5);
    const uint8_t neutral[]={0,0,0,0,0x80},pressed[]={5,0x34,0x12,0xff,0xff};
    assert(!memcmp(report,active?pressed:neutral,5));
}
static void protocol_test(void) {
    assert(osg_gateway_crc32("123456789",9)==UINT32_C(0xcbf43926));
    osg_gateway_identity_t id=identity(2),parsed=id;
    assert(osg_gateway_identity_parse("00112233445566778899AABBCCDDEEFF",&parsed));
    char text[33]; osg_gateway_identity_format(&parsed,text);
    assert(!strcmp(text,"00112233445566778899aabbccddeeff"));
    assert(!osg_gateway_identity_parse("00000000000000000000000000000000",&parsed));
    assert(!osg_gateway_identity_parse("ffffffffffffffffffffffffffffffff",&parsed));
    assert(!osg_gateway_identity_parse("1234",&parsed));
    osg_gateway_profile_t invalid=profile();
    invalid.axis_usage[1]=0x30; assert(!osg_gateway_profile_valid(&invalid));
    invalid=profile(); invalid.axis_neutral[7]=1; assert(!osg_gateway_profile_valid(&invalid));
    invalid=profile(); invalid.button_count=129; assert(!osg_gateway_profile_valid(&invalid));
    invalid=profile(); invalid.hat_count=5; assert(!osg_gateway_profile_valid(&invalid));
}
static void lifecycle_test(void) {
    osg_gateway_t g; assert(osg_gateway_init(&g,0x10000,NULL));
    osg_gateway_message_t a=discover(&g,1,0),in=input(a,1);
    assert(a.node==1 && a.lease==0x10001);
    osg_gateway_identity_t id=identity(1);
    assert(osg_gateway_register(&g,&id)); assert(osg_gateway_needs_apply(&g));
    assert(!g.active.slots[0].occupied);
    osg_gateway_apply_layout(&g); assert(!osg_gateway_needs_apply(&g));
    assert(osg_gateway_find_slot(&g.active,&id)==0); check_report(&g,0,false);
    assert(receive(&g,&in,1,NULL)==OSG_GW_RX_REJECTED); /* Publication rejects the discovery lease. */
    a=discover(&g,1,1); in=input(a,1);
    assert(receive(&g,&in,1,NULL)==OSG_GW_RX_ACCEPTED); check_report(&g,0,false);
    osg_gateway_set_running(&g,true); check_report(&g,0,false);
    assert(receive(&g,&in,2,NULL)==OSG_GW_RX_REJECTED); /* pre-start replay */
    in.sequence=2; assert(receive(&g,&in,3,NULL)==OSG_GW_RX_ACCEPTED); check_report(&g,0,true);
    osg_gateway_tick(&g,253); check_report(&g,0,false); assert(g.modules[0].online);
    in.sequence=3; assert(receive(&g,&in,254,NULL)==OSG_GW_RX_ACCEPTED); check_report(&g,0,true);
    osg_gateway_message_t hb=a; hb.type=OSG_GW_HEARTBEAT; hb.sequence=4; hb.direct_usb=true;
    assert(receive(&g,&hb,255,NULL)==OSG_GW_RX_ACCEPTED); check_report(&g,0,false);
    in.sequence=5; assert(receive(&g,&in,256,NULL)==OSG_GW_RX_ACCEPTED); check_report(&g,0,false);
    hb.sequence=6; hb.direct_usb=false; assert(receive(&g,&hb,257,NULL)==OSG_GW_RX_ACCEPTED);
    check_report(&g,0,false); in.sequence=7; assert(receive(&g,&in,258,NULL)==OSG_GW_RX_ACCEPTED);
    check_report(&g,0,true);
    osg_gateway_message_t stale=in; stale.lease--; stale.sequence=8;
    assert(receive(&g,&stale,259,NULL)==OSG_GW_RX_REJECTED); check_report(&g,0,true);
    stale=in; stale.boot_session++; stale.sequence=8;
    assert(receive(&g,&stale,259,NULL)==OSG_GW_RX_REJECTED);
    stale=in; stale.node=2; stale.sequence=8; assert(receive(&g,&stale,259,NULL)==OSG_GW_RX_REJECTED);
    stale=in; stale.report[0]=0x80; stale.sequence=8; assert(receive(&g,&stale,259,NULL)==OSG_GW_RX_REJECTED);
    stale=in; stale.report[23]=1; stale.sequence=8; assert(receive(&g,&stale,259,NULL)==OSG_GW_RX_REJECTED);
    osg_gateway_tick(&g,1758); assert(!g.modules[0].online); check_report(&g,0,false);
    osg_gateway_message_t b=discover(&g,2,1759); assert(b.node==a.node && b.lease!=a.lease);
    in.sequence=8; assert(receive(&g,&in,1760,NULL)==OSG_GW_RX_REJECTED); check_report(&g,0,false);
    osg_gateway_message_t revived=discover(&g,1,1761); assert(revived.node==2 && revived.lease!=a.lease);
    in=input(revived,1); assert(receive(&g,&in,1762,NULL)==OSG_GW_RX_ACCEPTED); check_report(&g,0,true);
    osg_gateway_set_running(&g,false); check_report(&g,0,false);
    osg_gateway_set_running(&g,true); assert(receive(&g,&in,1763,NULL)==OSG_GW_RX_REJECTED);
    osg_gateway_bus_reset(&g); assert(!g.running && !g.modules[0].online); check_report(&g,0,false);
    assert(g.active.slots[0].occupied); assert(receive(&g,&in,1764,NULL)==OSG_GW_RX_REJECTED);
}
static void bounds_and_wrap_test(void) {
    osg_gateway_t g; assert(osg_gateway_init(&g,0x20000,NULL));
    osg_gateway_message_t reply;
    for(unsigned i=1;i<=OSG_GW_MAX_MODULES;i++) {
        osg_gateway_message_t a=discover(&g,i,0xfffffff0U); assert(a.node==i);
        osg_gateway_identity_t id=identity(i); assert(osg_gateway_register(&g,&id));
    }
    osg_gateway_message_t eleventh=announce(OSG_GW_MAX_MODULES+1);
    assert(receive(&g,&eleventh,0xfffffff1U,&reply)==OSG_GW_RX_FULL);
    assert(g.rejected_announcements==1 && !strcmp(g.admission_error,"capacity_exceeded"));
    assert(osg_gateway_identity_equal(&g.rejected_identity,&eleventh.identity));
    osg_gateway_apply_layout(&g); osg_gateway_set_running(&g,true);
    osg_gateway_message_t a=discover(&g,1,0xfffffff2U),in=input(a,0xfffffffeU);
    assert(receive(&g,&in,0xfffffff3U,NULL)==OSG_GW_RX_ACCEPTED);
    in.sequence=0xffffffffU; assert(receive(&g,&in,0xfffffff4U,NULL)==OSG_GW_RX_ACCEPTED);
    in.sequence=0; assert(receive(&g,&in,0xfffffff5U,NULL)==OSG_GW_RX_ACCEPTED);
    in.sequence=0xffffffffU; assert(receive(&g,&in,0xfffffff6U,NULL)==OSG_GW_RX_REJECTED);
    osg_gateway_tick(&g,0x20); check_report(&g,0,true);
    osg_gateway_tick(&g,0xef); check_report(&g,0,false);
    osg_gateway_tick(&g,0x5dc); assert(!g.modules[0].online);
    assert(receive(&g,&eleventh,0x5dd,&reply)==OSG_GW_RX_FULL); /* retained offline records reserve capacity */
    osg_gateway_identity_t removed=identity(2); assert(osg_gateway_unregister(&g,&removed));
    assert(receive(&g,&eleventh,0x5de,&reply)==OSG_GW_RX_FULL); /* active layout still reserves it */
    osg_gateway_apply_layout(&g); assert(receive(&g,&eleventh,0x5df,&reply)==OSG_GW_RX_ASSIGNMENT);
    assert(osg_gateway_find_slot(&g.active,&removed)<0);
    assert(osg_gateway_register(&g,&eleventh.identity)); assert(osg_gateway_find_slot(&g.pending,&eleventh.identity)==1);
    osg_gateway_t exhausted; assert(osg_gateway_init(&exhausted,0x30000,NULL)); exhausted.lease_counter=65535;
    assert(receive(&exhausted,&eleventh,0,&reply)==OSG_GW_RX_FULL);
}
static void registry_test(void) {
    osg_gateway_registry_t r={.generation=0xffffffffU},decoded;
    r.slots[2]=(osg_gateway_slot_t){.occupied=true,.enabled=true,.report_id=3,.order=0,.identity=identity(7),.profile=profile()};
    uint8_t record[OSG_GW_REGISTRY_RECORD_SIZE],next[OSG_GW_REGISTRY_RECORD_SIZE],partial[OSG_GW_REGISTRY_RECORD_SIZE];
    assert(osg_gateway_registry_encode(&r,record,sizeof record)==sizeof record);
    assert(osg_gateway_registry_decode(record,sizeof record,&decoded));
    assert(decoded.slots[2].occupied && decoded.slots[2].report_id==3 && !decoded.slots[0].occupied);
    /* Legacy sparse record: descriptor order is compacted, report ID retained. */
    uint8_t legacy[500]={0}; memcpy(legacy,record,16); legacy[4]=2; legacy[6]=10;
    legacy[12]=0xf4; legacy[13]=1; memcpy(legacy+16+48*2,record+16+48*2,48);
    legacy[16+48*2]=1; legacy[16+48*2+46]=legacy[16+48*2+47]=0;
    uint32_t crc=osg_gateway_crc32(legacy,496);
    for(unsigned i=0;i<4;i++) legacy[496+i]=(uint8_t)(crc>>(8*i));
    assert(osg_gateway_registry_decode(legacy,sizeof legacy,&decoded));
    assert(decoded.slots[0].enabled && decoded.slots[0].report_id==3 && decoded.slots[0].order==0);
    legacy[4]=1; crc=osg_gateway_crc32(legacy,496);
    for(unsigned i=0;i<4;i++) legacy[496+i]=(uint8_t)(crc>>(8*i));
    assert(osg_gateway_registry_decode(legacy,sizeof legacy,&decoded));
    assert(osg_gateway_registry_encode(&decoded,next,sizeof next)==sizeof next && next[4]==3);
    legacy[16+48*2+3]=3; crc=osg_gateway_crc32(legacy,496);
    for(unsigned i=0;i<4;i++) legacy[496+i]=(uint8_t)(crc>>(8*i));
    assert(!osg_gateway_registry_decode(legacy,sizeof legacy,&decoded));
    r.generation=0; assert(osg_gateway_registry_encode(&r,next,sizeof next)==sizeof next);
    assert(osg_gateway_registry_select(record,sizeof record,next,sizeof next,&decoded)==1 && decoded.generation==0);
    for(unsigned i=0;i<sizeof record;i++) {
        memcpy(partial,next,sizeof partial); partial[i]^=1;
        assert(!osg_gateway_registry_decode(partial,sizeof partial,&decoded));
        assert(osg_gateway_registry_select(record,sizeof record,partial,sizeof partial,&decoded)==0);
        memset(partial,0xff,sizeof partial); memcpy(partial,next,i);
        assert(osg_gateway_registry_select(record,sizeof record,partial,sizeof partial,&decoded)==0);
    }
    assert(osg_gateway_registry_select(NULL,0,NULL,0,&decoded)==-1);
    r.slots[4]=r.slots[2]; assert(!osg_gateway_registry_valid(&r));
    assert(!osg_gateway_registry_encode(&r,next,sizeof next));
    osg_gateway_t g; assert(osg_gateway_init(&g,0x40000,&decoded));
    assert(!g.modules[2].online && g.active.slots[2].occupied); check_report(&g,2,false);
}
static void descriptor_test(void) {
    osg_gateway_t g; assert(osg_gateway_init(&g,0x50000,NULL));
    uint8_t before[1024],after[1024]; size_t empty=osg_gateway_descriptor(&g,before,sizeof before);
    assert(!empty); /* CDC-only, no HID collection before first enrollment */
    for(unsigned i=1;i<=10;i++) {
        discover(&g,i,i); osg_gateway_identity_t id=identity(i); assert(osg_gateway_register(&g,&id));
    }
    osg_gateway_apply_layout(&g); size_t n=osg_gateway_descriptor(&g,before,sizeof before);
    assert(n>empty && n<1024); assert(!osg_gateway_descriptor(&g,after,n-1));
    unsigned ids=0,collections=0; uint8_t report_id=0; unsigned bits[11]={0},size=0,count=0;
    for(size_t i=0;i<n;) {
        uint8_t item=before[i++]; unsigned bytes=item&3; if(bytes==3) bytes=4;
        assert(i+bytes<=n); uint32_t value=0;
        for(unsigned b=0;b<bytes;b++) value|=(uint32_t)before[i+b]<<(8*b);
        if(item==0x85) { report_id=(uint8_t)value; assert(report_id==++ids); }
        if(item==0xa1) collections++;
        if(item==0x75) size=(unsigned)value;
        if(item==0x95) count=(unsigned)value;
        if((item&0xfc)==0x80) bits[report_id]+=size*count;
        i+=bytes;
    }
    assert(ids==10 && collections==10);
    for(unsigned i=1;i<=10;i++) assert(bits[i]==40);
    osg_gateway_tick(&g,2000); assert(osg_gateway_descriptor(&g,after,sizeof after)==n);
    assert(!memcmp(before,after,n)); /* unplug preserves exact descriptors */
    osg_gateway_identity_t id=identity(1); osg_gateway_message_t reply;
    osg_gateway_message_t changed=announce(1); changed.profile.button_count=4;
    assert(receive(&g,&changed,2001,&reply)==OSG_GW_RX_ASSIGNMENT);
    assert(osg_gateway_descriptor(&g,after,sizeof after)==n && !memcmp(before,after,n));
    assert(osg_gateway_register(&g,&id)); assert(osg_gateway_needs_apply(&g));
    osg_gateway_apply_layout(&g); assert(osg_gateway_descriptor(&g,after,sizeof after)==n);
    assert(memcmp(before,after,n));
}
static osg_gateway_profile_t maximum_profile(void) {
    osg_gateway_profile_t p={.button_count=128,.hat_count=4,.axis_count=8,.revision=1};
    for(unsigned a=0;a<8;a++) { p.axis_usage[a]=(uint8_t)(0x30+a); p.axis_neutral[a]=(uint16_t)(a*9000); }
    return p;
}
static void maximum_profile_test(void) {
    osg_gateway_registry_t r={0};
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) r.slots[i]=(osg_gateway_slot_t){.occupied=true,.enabled=true,.report_id=(uint8_t)(i+1),.order=(uint8_t)i,.identity=identity(i+1),.profile=maximum_profile()};
    osg_gateway_t g; assert(osg_gateway_init(&g,0x60000,&r));
    uint8_t descriptor[OSG_GW_DESCRIPTOR_CAPACITY],report[OSG_GW_MAX_REPORT_SIZE];
    size_t length=osg_gateway_descriptor(&g,descriptor,sizeof descriptor); assert(length && length<=sizeof descriptor);
    unsigned bit_count[OSG_GW_MAX_MODULES+1]={0},item_size=0,item_count=0,report_id=0;
    for(size_t i=0;i<length;) {
        uint8_t item=descriptor[i++]; unsigned bytes=item&3; if(bytes==3) bytes=4;
        assert(item!=0xfe && i+bytes<=length); uint32_t value=0;
        for(unsigned j=0;j<bytes;j++) value|=(uint32_t)descriptor[i+j]<<(8*j);
        if(item==0x85) { report_id=value; assert(report_id>=1 && report_id<=OSG_GW_MAX_MODULES); }
        if(item==0x75) item_size=value;
        if(item==0x95) item_count=value;
        if((item&0xfc)==0x80) bit_count[report_id]+=item_size*item_count;
        i+=bytes;
    }
    for(unsigned i=1;i<=OSG_GW_MAX_MODULES;i++) assert(bit_count[i]==34*8);
    osg_gateway_message_t msg=announce(1); msg.profile=r.slots[0].profile;
    osg_gateway_message_t assignment; assert(receive(&g,&msg,0,&assignment)==OSG_GW_RX_ASSIGNMENT);
    msg=assignment; msg.source_node=21;
    msg.type=OSG_GW_INPUT; msg.sequence=1;
    for(unsigned i=0;i<sizeof msg.report;i++) msg.report[i]=(uint8_t)(i+1);
    msg.report[16]=0x70; msg.report[17]=0x88;
    osg_gateway_set_running(&g,true); assert(receive(&g,&msg,1,NULL)==OSG_GW_RX_ACCEPTED);
    assert(osg_gateway_report(&g,0,report,sizeof report)==34 && !memcmp(report,msg.report,34));
    assert(!osg_gateway_report(&g,0,report,33));
    osg_gateway_tick(&g,251); assert(osg_gateway_report(&g,0,report,sizeof report)==34);
    for(unsigned i=0;i<16;i++) assert(report[i]==0);
    assert(report[16]==0x88 && report[17]==0x88);
    for(unsigned i=0;i<8;i++) {
        uint16_t actual=(uint16_t)(report[18+2*i]|((uint16_t)report[19+2*i]<<8));
        assert(actual==r.slots[0].profile.axis_neutral[i]);
    }
    /* Invalid hat directions, button padding and unused hat nibbles are rejected. */
    msg.sequence=2; msg.report[16]=0x90;
    assert(receive(&g,&msg,252,NULL)==OSG_GW_RX_REJECTED);
    osg_gateway_profile_t odd={.button_count=3,.hat_count=3,.revision=1};
    uint8_t odd_report[34]; osg_gateway_profile_neutral(&odd,odd_report);
    assert(odd_report[1]==0x88 && odd_report[2]==8 && osg_gateway_profile_report_valid(&odd,odd_report));
    odd_report[2]|=0x80; assert(!osg_gateway_profile_report_valid(&odd,odd_report));
    odd_report[2]=8; odd_report[0]=0x80; assert(!osg_gateway_profile_report_valid(&odd,odd_report));
    /* Registered profile stays authoritative until explicit apply. */
    msg=announce(1); msg.profile=r.slots[0].profile; msg.profile.revision=2;
    assert(receive(&g,&msg,253,&assignment)==OSG_GW_RX_ASSIGNMENT);
    msg=assignment; msg.source_node=21; msg.type=OSG_GW_INPUT; msg.sequence=1;
    memset(msg.report,0xff,sizeof msg.report); msg.report[16]=0x70; msg.report[17]=0x88;
    assert(receive(&g,&msg,254,NULL)==OSG_GW_RX_ACCEPTED);
    assert(osg_gateway_report(&g,0,report,sizeof report)==34 && report[0]==0);
    osg_gateway_identity_t id=identity(1);
    assert(osg_gateway_register(&g,&id)); osg_gateway_apply_layout(&g); osg_gateway_set_running(&g,true);
    msg.sequence=2; assert(receive(&g,&msg,255,NULL)==OSG_GW_RX_ACCEPTED);
    assert(osg_gateway_report(&g,0,report,sizeof report)==34 && report[0]==0xff);
}
static void delivery_test(void) {
    osg_gateway_t g; assert(osg_gateway_init(&g,0x10000,NULL));
    osg_gateway_message_t assignment=discover(&g,1,0),in=input(assignment,1);
    osg_gateway_identity_t id=identity(1); assert(osg_gateway_register(&g,&id));
    osg_gateway_apply_layout(&g); osg_gateway_set_running(&g,true);
    assignment=discover(&g,1,0); in=input(assignment,1);
    uint8_t wire[OSG_GW_MAX_REPORT_SIZE+1]={1}; osg_gateway_delivery_t d;
    osg_gateway_message_t ack; osg_gateway_message_t decoded;
    assert(osg_gateway_delivery_prepare(&g,0,wire+1,sizeof wire-1,&d)==5 && !d.valid);
    assert(!osg_gateway_delivery_complete(&g,&d,wire,6,&ack)); /* neutral fallback */
    assert(receive(&g,&in,1,NULL)==OSG_GW_RX_ACCEPTED);
    g.modules[1]=g.modules[0]; g.modules[1].identity=identity(2);
    osg_gateway_message_t ambiguous=in; ambiguous.sequence=2;
    assert(receive(&g,&ambiguous,1,NULL)==OSG_GW_RX_REJECTED);
    memset(&g.modules[1],0,sizeof g.modules[1]);
    assert(osg_gateway_delivery_prepare(&g,0,wire+1,sizeof wire-1,&d)==5 && d.valid);
    assert(!osg_gateway_delivery_complete(&g,&d,wire,5,&ack));
    wire[0]=2; assert(!osg_gateway_delivery_complete(&g,&d,wire,6,&ack)); wire[0]=1;
    wire[1]^=1; assert(!osg_gateway_delivery_complete(&g,&d,wire,6,&ack)); wire[1]^=1;
    /* A newer CAN input or heartbeat must not rewrite the in-flight USB snapshot. */
    in.sequence=2; in.report[0]=3; assert(receive(&g,&in,2,NULL)==OSG_GW_RX_ACCEPTED);
    osg_gateway_message_t hb=assignment; hb.type=OSG_GW_HEARTBEAT; hb.sequence=3;
    assert(receive(&g,&hb,3,NULL)==OSG_GW_RX_ACCEPTED);
    assert(osg_gateway_delivery_complete(&g,&d,wire,6,&ack));
    decoded=ack;
    assert(decoded.type==OSG_GW_INPUT_ACK && decoded.sequence==1 && decoded.report[0]==5);
    assert(!osg_gateway_identity_valid(&decoded.identity)); /* compact route */
    assert(osg_gateway_receive(&g,&ack,4,NULL)==OSG_GW_RX_REJECTED);
    osg_gateway_set_running(&g,false); osg_gateway_set_running(&g,true);
    in.sequence=4; assert(receive(&g,&in,5,NULL)==OSG_GW_RX_ACCEPTED);
    assert(!osg_gateway_delivery_complete(&g,&d,wire,6,&ack)); /* USB/run generation */
    assert(osg_gateway_delivery_prepare(&g,0,wire+1,sizeof wire-1,&d)==5 && d.valid);
    hb.sequence=5; hb.direct_usb=true; assert(receive(&g,&hb,6,NULL)==OSG_GW_RX_ACCEPTED);
    hb.sequence=6; hb.direct_usb=false; assert(receive(&g,&hb,7,NULL)==OSG_GW_RX_ACCEPTED);
    in.sequence=7; assert(receive(&g,&in,8,NULL)==OSG_GW_RX_ACCEPTED);
    assert(!osg_gateway_delivery_complete(&g,&d,wire,6,&ack)); /* ownership changed and returned */
    assert(osg_gateway_delivery_prepare(&g,0,wire+1,sizeof wire-1,&d)==5 && d.valid);
    osg_gateway_tick(&g,258); in.sequence=8; assert(receive(&g,&in,259,NULL)==OSG_GW_RX_ACCEPTED);
    assert(!osg_gateway_delivery_complete(&g,&d,wire,6,&ack)); /* freshness generation */
    assert(osg_gateway_delivery_prepare(&g,0,wire+1,sizeof wire-1,&d)==5 && d.valid);
    osg_gateway_message_t reboot=announce(1); reboot.boot_session++;
    osg_gateway_message_t reply; assert(receive(&g,&reboot,260,&reply)==OSG_GW_RX_ASSIGNMENT);
    assert(!osg_gateway_delivery_complete(&g,&d,wire,6,&ack)); /* session and lease changed */
    osg_gateway_bus_reset(&g); assert(!osg_gateway_delivery_complete(&g,&d,wire,6,&ack));
}
static void transport_source_test(void) {
    osg_gateway_t g; assert(osg_gateway_init(&g,0x10000,NULL));
    osg_gateway_message_t announce_message=announce(1),assignment;
    announce_message.source_node=97; /* CAN address is independent of route1. */
    assert(receive(&g,&announce_message,0,&assignment)==OSG_GW_RX_ASSIGNMENT);
    assert(assignment.node==1 && g.modules[0].source_node==97);
    assignment.source_node=97;
    osg_gateway_identity_t id=identity(1); assert(osg_gateway_register(&g,&id));
    osg_gateway_apply_layout(&g); osg_gateway_set_running(&g,true);
    assert(receive(&g,&announce_message,0,&assignment)==OSG_GW_RX_ASSIGNMENT); assignment.source_node=97;
    osg_gateway_message_t in=input(assignment,1);
    assert(receive(&g,&in,1,NULL)==OSG_GW_RX_ACCEPTED);
    uint8_t wire[6]={1}; osg_gateway_delivery_t delivery;
    assert(osg_gateway_delivery_prepare(&g,0,wire+1,5,&delivery)==5 && delivery.valid);
    osg_gateway_message_t spoof=in; spoof.source_node=98; spoof.sequence=2;
    assert(receive(&g,&spoof,2,NULL)==OSG_GW_RX_REJECTED);
    assert(g.modules[0].last_seen==1); check_report(&g,0,true);
    /* A complete new announcement binds a moved source to a fresh lease. */
    announce_message.source_node=98;
    osg_gateway_message_t moved;
    assert(receive(&g,&announce_message,3,&moved)==OSG_GW_RX_ASSIGNMENT);
    assert(moved.lease!=assignment.lease && g.modules[0].source_node==98);
    assert(osg_gateway_find_slot(&g.active,&id)==0); check_report(&g,0,false);
    osg_gateway_message_t ack;
    assert(!osg_gateway_delivery_complete(&g,&delivery,wire,sizeof wire,&ack));
    assert(receive(&g,&in,4,NULL)==OSG_GW_RX_REJECTED);
    moved.source_node=98; in=input(moved,1);
    assert(receive(&g,&in,5,NULL)==OSG_GW_RX_ACCEPTED);
    /* A conflicting UID on the same source neutralizes the old route while
     * the standard CAN node-ID autoconfiguration resolves the collision. */
    osg_gateway_message_t conflict=announce(2); conflict.source_node=98;
    assert(receive(&g,&conflict,6,&moved)==OSG_GW_RX_REJECTED);
    assert(!g.modules[0].online); check_report(&g,0,false);
    assert(receive(&g,&in,7,NULL)==OSG_GW_RX_REJECTED);
    announce_message.source_node=0; /* Cyphal node-ID zero is valid. */
    assert(receive(&g,&announce_message,8,&moved)==OSG_GW_RX_ASSIGNMENT);
    assert(g.modules[0].source_node==0);
    announce_message.source_node=128;
    assert(receive(&g,&announce_message,9,&moved)==OSG_GW_RX_REJECTED);
}
static bool persist(void *context) { unsigned *calls=context; (*calls)++; return false; }
static void cockpit_test(void) {
    osg_gateway_cockpit_request_t r;
    assert(osg_gateway_cockpit_parse("cockpit 4294967295 snapshot",&r) && r.id==UINT32_MAX);
    const char *invalid[]={"cockpit 4294967296 snapshot","cockpit -1 snapshot","cockpit 1 snapshot ",
        "cockpit 1 0 disable x","cockpit 1 save","cockpit 1 1 apply","cockpit 1 1 register 00000000000000000000000000000001"};
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++) assert(!osg_gateway_cockpit_parse(invalid[i],&r));
    osg_gateway_t g; assert(osg_gateway_init(&g,0x10000,NULL)); discover(&g,1,0);
    char id[33],line[80]; osg_gateway_identity_t module=identity(1); osg_gateway_identity_format(&module,id);
    snprintf(line,sizeof line,"cockpit 2 1 disable %s",id); assert(osg_gateway_cockpit_parse(line,&r));
    bool apply=true; unsigned calls=0;
    assert(!strcmp(osg_gateway_cockpit_execute(&g,&r,persist,&calls,&apply),"stale_epoch") && !apply && !calls);
    assert(g.pending.slots[0].enabled); /* Automatic discovery already staged it. */
    snprintf(line,sizeof line,"cockpit 2 65536 disable %s",id); assert(osg_gateway_cockpit_parse(line,&r));
    assert(!osg_gateway_cockpit_execute(&g,&r,persist,&calls,&apply) && !apply && !calls);
    assert(!g.pending.slots[0].enabled);
    snprintf(line,sizeof line,"cockpit 3 65536 forget %s",id); assert(osg_gateway_cockpit_parse(line,&r));
    assert(!strcmp(osg_gateway_cockpit_execute(&g,&r,NULL,NULL,&apply),"device_online"));
    char output[OSG_GW_COCKPIT_RESPONSE_CAPACITY];
    assert(osg_gateway_cockpit_response(&g,"0123456789abcdef01234567",3,"stale_epoch",output,sizeof output));
    assert(strstr(output,"\"id\":3,\"ok\":false") && strstr(output,"\"layoutPending\":true"));
    assert(strstr(output,"\"enabled\":false,\"published\":false"));
    assert(!osg_gateway_cockpit_response(&g,"0123456789abcdef01234567",3,NULL,output,10) && !output[0]);
}
static void action_test(void) {
    osg_gateway_t g; assert(osg_gateway_init(&g,65536,NULL));
    osg_gateway_message_t assignment=discover(&g,1,0);
    osg_gateway_cockpit_request_t request;
    const char *line="cockpit 11 65536 action 00000000000000000000000000000001 100 describe 0";
    assert(osg_gateway_cockpit_parse(line,&request));
    assert(request.action==OSG_GW_COCKPIT_ACTION && !strcmp(request.command,"describe 0"));
    const char *invalid[]={
        "cockpit 11 65536 action 00000000000000000000000000000001 100 ",
        "cockpit 11 65536 action 00000000000000000000000000000001 100 state\n",
        "cockpit 11 65536 action 00000000000000000000000000000001 0 state",
        "cockpit 11 65536 action 00000000000000000000000000000000 100 state"};
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++) {
        osg_gateway_cockpit_request_t bad;
        assert(!osg_gateway_cockpit_parse(invalid[i],&bad));
    }
    char large[OSG_GW_CONFIG_COMMAND_CAPACITY+100];
    size_t prefix=(size_t)snprintf(large,sizeof large,"cockpit 11 65536 action 00000000000000000000000000000001 100 ");
    memset(large+prefix,'a',OSG_GW_CONFIG_COMMAND_CAPACITY-8); large[prefix+OSG_GW_CONFIG_COMMAND_CAPACITY-8]=0;
    assert(osg_gateway_cockpit_parse(large,&request));
    large[prefix+OSG_GW_CONFIG_COMMAND_CAPACITY-8]='a'; large[prefix+OSG_GW_CONFIG_COMMAND_CAPACITY-7]=0;
    assert(!osg_gateway_cockpit_parse(large,&request));
    assert(osg_gateway_cockpit_parse(line,&request));
    osg_gateway_config_t transaction={0};
    assert(!strcmp(osg_gateway_config_start(&transaction,&g,&request,0),"configuration_route_not_ready"));
    /* Status proves reachability even with no input and direct USB ownership. */
    osg_gateway_message_t status=assignment; status.type=OSG_GW_HEARTBEAT;
    status.sequence=1; status.direct_usb=true;
    assert(receive(&g,&status,1,NULL)==OSG_GW_RX_ACCEPTED);
    osg_gateway_identity_t id=identity(1); assert(osg_gateway_disable(&g,&id));
    uint32_t generation=g.pending.generation;
    assert(!osg_gateway_config_start(&transaction,&g,&request,1));
    assert(transaction.action && !strcmp(transaction.command,"action describe 0"));
    assert(transaction.route.config_total==strlen(transaction.command));
    assert(g.modules[0].direct_usb && !g.modules[0].has_input && g.pending.generation==generation);
    const char reply[]="OK\n{\"ok\":false,\"error\":\"invalidArgument\"}";
    osg_gateway_message_t response=transaction.route; response.type=OSG_GW_CONFIG_RESPONSE;
    response.config_total=(uint16_t)strlen(reply);
    for(unsigned offset=0;offset<strlen(reply);offset+=OSG_GW_CONFIG_CHUNK_SIZE) {
        response.config_offset=(uint16_t)offset;
        unsigned remaining=(unsigned)strlen(reply)-offset;
        response.config_length=(uint8_t)(remaining>OSG_GW_CONFIG_CHUNK_SIZE ? OSG_GW_CONFIG_CHUNK_SIZE : remaining);
        memcpy(response.config_data,reply+offset,response.config_length);
        assert(osg_gateway_config_receive(&transaction,&g,&response));
    }
    const char *error,*json;
    assert(osg_gateway_config_result(&transaction,&error,&json) && !error && !strcmp(json,reply+3));
    char output[OSG_GW_COCKPIT_RESPONSE_CAPACITY];
    assert(osg_gateway_cockpit_action_response(&g,"0123456789abcdef01234567",11,error,json,output,sizeof output));
    assert(strstr(output,"\"targetAction\":{\"ok\":false") && !strstr(output,"targetConfig"));
    assert(!osg_gateway_cockpit_action_response(&g,"0123456789abcdef01234567",11,error,json,output,10) && !output[0]);
    assert(!osg_gateway_config_start(&transaction,&g,&request,2));
    osg_gateway_config_tick(&transaction,&g,NULL,OSG_GATEWAY_CONFIG_TIMEOUT_MS+2);
    assert(osg_gateway_config_result(&transaction,&error,&json) && !strcmp(error,"configuration_timeout"));
    assert(!osg_gateway_config_start(&transaction,&g,&request,3));
    osg_gateway_config_cancel(&transaction);
    assert(!transaction.busy && !transaction.action);
    assert(osg_gateway_cockpit_parse("cockpit 12 65536 events 00000000000000000000000000000001 100 read 1 0",&request));
    assert(request.action==OSG_GW_COCKPIT_EVENTS);
    assert(!osg_gateway_config_start(&transaction,&g,&request,4));
    assert(transaction.events && !transaction.action && !strcmp(transaction.command,"events read 1 0"));
    const char event_reply[]="OK\n{\"ok\":false,\"error\":\"eventGap\"}";
    response=transaction.route;response.type=OSG_GW_CONFIG_RESPONSE;response.config_total=(uint16_t)strlen(event_reply);
    for(unsigned offset=0;offset<strlen(event_reply);offset+=OSG_GW_CONFIG_CHUNK_SIZE) {
        response.config_offset=(uint16_t)offset;
        unsigned remaining=(unsigned)strlen(event_reply)-offset;
        response.config_length=(uint8_t)(remaining>OSG_GW_CONFIG_CHUNK_SIZE ? OSG_GW_CONFIG_CHUNK_SIZE : remaining);
        memcpy(response.config_data,event_reply+offset,response.config_length);
        assert(osg_gateway_config_receive(&transaction,&g,&response));
    }
    assert(osg_gateway_config_result(&transaction,&error,&json) && !error);
    assert(osg_gateway_cockpit_events_response(&g,"0123456789abcdef01234567",12,error,json,output,sizeof output));
    assert(strstr(output,"\"targetEvents\":{\"ok\":false") && !strstr(output,"targetAction") && !strstr(output,"targetConfig"));
    assert(!osg_gateway_cockpit_events_response(&g,"0123456789abcdef01234567",12,error,json,output,10) && !output[0]);
    assert(!osg_gateway_cockpit_parse("cockpit 12 65536 events 00000000000000000000000000000001 100 state\n",&request));
    /* Typed string commands remain hex ASCII on the wire; response metadata
     * and readable UTF-8 text survive the generic bounded JSON envelope. */
    assert(osg_gateway_cockpit_parse("cockpit 13 65536 action 00000000000000000000000000000001 100 invoke 1 display.text.set_text c3a9e6b3a2",&request));
    assert(!osg_gateway_config_start(&transaction,&g,&request,5));
    assert(!strcmp(transaction.command,"action invoke 1 display.text.set_text c3a9e6b3a2"));
    const char display_json[]="{\"ok\":true,\"schemaVersion\":1,\"catalogueRevision\":1,\"id\":\"display.text.set_text\",\"values\":[\"\xc3\xa9\xe6\xb3\xa2\"]}";
    assert(osg_gateway_cockpit_action_response(&g,"0123456789abcdef01234567",13,NULL,display_json,output,sizeof output));
    assert(strstr(output,display_json) && strstr(output,"targetAction") && !strstr(output,"targetEvents"));
    const char axes_json[]="{\"ok\":true,\"schemaVersion\":1,\"stream\":1,\"head\":0,\"oldest\":1,\"overflow\":0,\"buttonCount\":0,\"held\":[],\"encoderButtons\":[],\"axisCount\":1,\"axes\":[65535],\"axisAvailable\":[true]}";
    assert(osg_gateway_cockpit_events_response(&g,"0123456789abcdef01234567",14,NULL,axes_json,output,sizeof output));
    assert(strstr(output,axes_json) && strstr(output,"targetEvents") && !strstr(output,"targetAction"));
    osg_gateway_config_cancel(&transaction);
}
static void configuration_test(void) {
    osg_gateway_t g; assert(osg_gateway_init(&g,65536,NULL));
    osg_gateway_message_t initial=discover(&g,1,0);
    osg_gateway_config_t c={0}; osg_gateway_cockpit_request_t request;
    assert(osg_gateway_cockpit_parse("cockpit 9 65536 config 00000000000000000000000000000001 100 claim 17",&request));
    assert(request.action==OSG_GW_COCKPIT_CONFIG && request.boot==100 && !strcmp(request.command,"claim 17"));
    request.id=0;assert(!strcmp(osg_gateway_config_start(&c,&g,&request,0),"invalid_request"));request.id=9;
    assert(!osg_gateway_cockpit_parse("cockpit 9 65536 config 00000000000000000000000000000001 0 get",&request));
    assert(osg_gateway_cockpit_parse("cockpit 9 65536 config 00000000000000000000000000000001 100 get",&request));
    request.epoch++; assert(!strcmp(osg_gateway_config_start(&c,&g,&request,0),"stale_epoch"));request.epoch--;
    request.boot++; assert(!strcmp(osg_gateway_config_start(&c,&g,&request,0),"stale_module_boot"));request.boot--;
    assert(!strcmp(osg_gateway_config_start(&c,&g,&request,0),"configuration_route_not_ready") && !c.busy);
    osg_gateway_message_t ready=input(initial,1); assert(receive(&g,&ready,0,NULL)==OSG_GW_RX_ACCEPTED);
    assert(!osg_gateway_config_start(&c,&g,&request,0));
    assert(!strcmp(osg_gateway_config_start(&c,&g,&request,0),"configuration_busy"));
    osg_gateway_message_t response=c.route; response.type=OSG_GW_CONFIG_RESPONSE;
    const char reply[]="OK\n{\"generation\":1,\"owner\":0}";
    response.config_total=(uint16_t)strlen(reply); response.config_offset=24;
    response.config_length=(uint8_t)(strlen(reply)-24);memcpy(response.config_data,reply+24,response.config_length);
    assert(!osg_gateway_config_receive(&c,&g,&response)); /* Reordering cannot complete a partial reply. */
    response.config_offset=0;response.config_length=24;memcpy(response.config_data,reply,24);
    response.source_node++;assert(!osg_gateway_config_receive(&c,&g,&response));response.source_node--;
    response.lease++;assert(!osg_gateway_config_receive(&c,&g,&response));response.lease--;
    response.gateway_epoch++;assert(!osg_gateway_config_receive(&c,&g,&response));response.gateway_epoch--;
    response.boot_session++;assert(!osg_gateway_config_receive(&c,&g,&response));response.boot_session--;
    response.sequence++;assert(!osg_gateway_config_receive(&c,&g,&response));response.sequence--;
    assert(osg_gateway_config_receive(&c,&g,&response));assert(!osg_gateway_config_receive(&c,&g,&response));
    response.config_offset=24;response.config_length=(uint8_t)(strlen(reply)-24);memcpy(response.config_data,reply+24,response.config_length);
    assert(osg_gateway_config_receive(&c,&g,&response));
    const char *error,*json;assert(osg_gateway_config_result(&c,&error,&json) && !error && !strcmp(json,reply+3));
    assert(!osg_gateway_config_start(&c,&g,&request,0));
    g.modules[0].lease++;osg_gateway_config_tick(&c,&g,NULL,1);
    assert(osg_gateway_config_result(&c,&error,&json) && !strcmp(error,"configuration_route_changed"));
    g.modules[0].lease--;
    osg_gateway_layout_t layout={0};osg_gateway_layout_schedule(&layout,10);
    assert(osg_gateway_layout_tick(&layout,200,false)==OSG_GATEWAY_LAYOUT_IDLE);
    assert(osg_gateway_layout_tick(&layout,200,true)==OSG_GATEWAY_LAYOUT_DETACH && layout.detached);
    assert(osg_gateway_layout_tick(&layout,449,true)==OSG_GATEWAY_LAYOUT_IDLE);
    assert(osg_gateway_layout_tick(&layout,450,true)==OSG_GATEWAY_LAYOUT_ATTACH && layout.reconnects==1);
    osg_gateway_identity_t id=identity(1);assert(osg_gateway_register(&g,&id));
    osg_gateway_apply_layout(&g);osg_gateway_set_running(&g,true);
    osg_gateway_message_t old=discover(&g,1,2),old_report=input(old,50);
    assert(receive(&g,&old_report,3,NULL)==OSG_GW_RX_ACCEPTED);
    uint8_t packet[OSG_GW_MAX_REPORT_SIZE+1]={1};osg_gateway_delivery_t delivery;
    size_t size=osg_gateway_delivery_prepare(&g,0,packet+1,sizeof packet-1,&delivery);assert(delivery.valid);
    osg_gateway_message_t changed=announce(1),assignment,ack;changed.configuration_generation=2;
    assert(receive(&g,&changed,4,&assignment)==OSG_GW_RX_ASSIGNMENT && assignment.lease!=old.lease);
    assert(!osg_gateway_needs_apply(&g)); /* Settings generation is independent of HID shape. */
    assert(receive(&g,&old_report,5,NULL)==OSG_GW_RX_REJECTED);
    assert(!osg_gateway_delivery_complete(&g,&delivery,packet,size+1,&ack));
}
typedef struct { bool fail; unsigned writes; osg_gateway_registry_t saved; } lifecycle_store_t;
static bool lifecycle_persist(void *context,const osg_gateway_registry_t *registry) {
    lifecycle_store_t *store=context; store->writes++;
    if(store->fail) return false;
    store->saved=*registry; return true;
}
static osg_gateway_layout_event_t lifecycle_tick(osg_gateway_layout_t *layout,osg_gateway_t *g,
    lifecycle_store_t *store,uint32_t now,bool drained) {
    osg_gateway_layout_event_t event=osg_gateway_layout_service(layout,g,now,drained,lifecycle_persist,store);
    if(event==OSG_GATEWAY_LAYOUT_DETACH) assert(osg_gateway_activate_prepared(g,&layout->target,true,&layout->activation));
    if(event==OSG_GATEWAY_LAYOUT_ATTACH) osg_gateway_set_running(g,true);
    return event;
}
static void lifecycle_flush(osg_gateway_layout_t *layout,osg_gateway_t *g,lifecycle_store_t *store,uint32_t now) {
    (void)lifecycle_tick(layout,g,store,now,true);
    (void)lifecycle_tick(layout,g,store,now+100,true);
    (void)lifecycle_tick(layout,g,store,now+200,true);
    (void)lifecycle_tick(layout,g,store,now+450,true);
    assert(!osg_gateway_needs_apply(g) && !g->layout_transition);
}
static void disabled_snapshot_test(void) {
    osg_gateway_t g; assert(osg_gateway_init(&g,65536,NULL));
    osg_gateway_message_t announce_message=announce(1),assignment;
    announce_message.profile=maximum_profile();
    assert(receive(&g,&announce_message,0,&assignment)==OSG_GW_RX_ASSIGNMENT);
    osg_gateway_apply_layout(&g); osg_gateway_set_running(&g,true);
    assert(receive(&g,&announce_message,0,&assignment)==OSG_GW_RX_ASSIGNMENT);
    osg_gateway_message_t pressed=assignment; pressed.type=OSG_GW_INPUT; pressed.source_node=21; pressed.sequence=1;
    memset(pressed.report,255,sizeof pressed.report); pressed.report[16]=0x70; pressed.report[17]=0x88;
    assert(receive(&g,&pressed,1,NULL)==OSG_GW_RX_ACCEPTED);
    uint8_t actual[34]; assert(osg_gateway_report(&g,0,actual,sizeof actual)==34 && actual[0]==255);
    osg_gateway_identity_t id=identity(1); assert(osg_gateway_disable(&g,&id));
    char response[OSG_GW_COCKPIT_RESPONSE_CAPACITY],neutral_hex[69]; uint8_t neutral_report[34];
    osg_gateway_profile_neutral(&announce_message.profile,neutral_report);
    for(unsigned i=0;i<34;i++) snprintf(neutral_hex+2*i,3,"%02x",neutral_report[i]);
    assert(osg_gateway_cockpit_response(&g,"0123456789abcdef01234567",1,NULL,response,sizeof response));
    assert(strstr(response,"\"enabled\":false,\"published\":true") && strstr(response,neutral_hex));
    assert(osg_gateway_report(&g,0,actual,sizeof actual)==34 && actual[0]==255); /* Prior active USB is unchanged before save. */
}
static void automatic_lifecycle_test(void) {
    osg_gateway_t g; osg_gateway_layout_t layout={0}; lifecycle_store_t store={0};
    assert(osg_gateway_init(&g,65536,NULL)); (void)discover(&g,1,0);
    assert(g.pending.slots[0].enabled && !g.active.slots[0].occupied);
    assert(lifecycle_tick(&layout,&g,&store,0,true)==OSG_GATEWAY_LAYOUT_IDLE && !store.writes);
    (void)discover(&g,2,20); (void)lifecycle_tick(&layout,&g,&store,20,true);
    (void)lifecycle_tick(&layout,&g,&store,120,true); assert(store.writes==1);
    uint32_t saved=store.saved.generation;
    (void)discover(&g,3,130); /* Arrived after save: cannot enter the saved descriptor. */
    assert(g.pending.generation!=saved && !g.active.slots[0].occupied);
    assert(lifecycle_tick(&layout,&g,&store,220,true)==OSG_GATEWAY_LAYOUT_DETACH);
    assert(g.active.generation==saved && !g.active.slots[2].occupied);
    uint8_t two[4096],three[4096],removed[4096]; size_t two_size=osg_gateway_descriptor(&g,two,sizeof two);
    assert(lifecycle_tick(&layout,&g,&store,470,true)==OSG_GATEWAY_LAYOUT_ATTACH);
    lifecycle_flush(&layout,&g,&store,480);
    size_t three_size=osg_gateway_descriptor(&g,three,sizeof three);
    assert(three_size>two_size && !memcmp(two,three,two_size) && store.writes==2);
    (void)discover(&g,1,1000); (void)lifecycle_tick(&layout,&g,&store,1000,true);
    assert(store.writes==2); /* Repeat announce does not erase flash or reconnect. */
    osg_gateway_tick(&g,3000);
    assert(osg_gateway_descriptor(&g,removed,sizeof removed)==three_size && !memcmp(removed,three,three_size));
    check_report(&g,0,false); /* Offline enabled ghost stays enumerated and neutral. */
    osg_gateway_identity_t second=identity(2),third=identity(3);
    assert(osg_gateway_disable(&g,&second)); uint32_t disabled_generation=g.pending.generation;
    (void)discover(&g,2,3001); assert(g.pending.generation==disabled_generation && !g.pending.slots[1].enabled);
    lifecycle_flush(&layout,&g,&store,3010);
    size_t removed_size=osg_gateway_descriptor(&g,removed,sizeof removed);
    assert(removed_size<three_size && !osg_gateway_report(&g,1,three,sizeof three));
    assert(osg_gateway_descriptor_index(&g.active,&third)==1 && g.active.slots[2].report_id==3);
    uint8_t encoded[OSG_GW_REGISTRY_RECORD_SIZE]; osg_gateway_registry_t restored;
    assert(osg_gateway_registry_encode(&store.saved,encoded,sizeof encoded));
    assert(osg_gateway_registry_decode(encoded,sizeof encoded,&restored));
    assert(osg_gateway_init(&g,131072,&restored)); memset(&layout,0,sizeof layout);
    (void)discover(&g,2,4000); assert(!g.pending.slots[1].enabled && !osg_gateway_needs_apply(&g));
    assert(osg_gateway_enable(&g,&second)); assert(g.pending.slots[1].order==2 && g.pending.slots[1].report_id==2);
    lifecycle_flush(&layout,&g,&store,4010);
    three_size=osg_gateway_descriptor(&g,three,sizeof three);
    assert(three_size>removed_size && !memcmp(removed,three,removed_size)); /* Re-enable appends, never restores an old gap. */
    assert(!osg_gateway_forget(&g,&second)); osg_gateway_tick(&g,6000);
    assert(osg_gateway_forget(&g,&second)); lifecycle_flush(&layout,&g,&store,6010);
    assert(osg_gateway_find_module(&g,&second)<0 && osg_gateway_find_slot(&g.pending,&second)<0);
    (void)discover(&g,2,6500); assert(osg_gateway_descriptor_index(&g.pending,&second)==2);
    store.fail=true; (void)lifecycle_tick(&layout,&g,&store,6500,true);
    (void)lifecycle_tick(&layout,&g,&store,6600,true);
    assert(!strcmp(g.layout_error,"flash_save_failed") && osg_gateway_descriptor_index(&g.active,&second)<0);
    unsigned writes=store.writes; (void)lifecycle_tick(&layout,&g,&store,6601,true); assert(store.writes==writes);
    store.fail=false; (void)lifecycle_tick(&layout,&g,&store,7600,true);
    assert(lifecycle_tick(&layout,&g,&store,7700,false)==OSG_GATEWAY_LAYOUT_IDLE);
    /* Even an unread CDC response cannot prevent discovery from activating forever. */
    assert(lifecycle_tick(&layout,&g,&store,8700,false)==OSG_GATEWAY_LAYOUT_DETACH);
    assert(lifecycle_tick(&layout,&g,&store,8950,true)==OSG_GATEWAY_LAYOUT_ATTACH && !g.layout_error);
    assert(osg_gateway_disable(&g,&second)); lifecycle_flush(&layout,&g,&store,9000);
    unsigned reconnects=layout.reconnects;
    osg_gateway_message_t changed=announce(2),reply; changed.profile.button_count=4;
    assert(receive(&g,&changed,9500,&reply)==OSG_GW_RX_ASSIGNMENT);
    lifecycle_flush(&layout,&g,&store,9500);
    assert(!g.pending.slots[osg_gateway_find_slot(&g.pending,&second)].enabled && layout.reconnects==reconnects);
    /* Reverting a failed change must clear an obsolete persistence error. */
    assert(osg_gateway_enable(&g,&second)); lifecycle_flush(&layout,&g,&store,10000);
    assert(osg_gateway_disable(&g,&second)); store.fail=true;
    (void)lifecycle_tick(&layout,&g,&store,10500,true); (void)lifecycle_tick(&layout,&g,&store,10600,true);
    assert(g.layout_error); assert(osg_gateway_enable(&g,&second));
    (void)lifecycle_tick(&layout,&g,&store,10601,true);
    assert(!osg_gateway_needs_apply(&g) && !g.layout_error);

}
static void publication_lease_test(void) {
    osg_gateway_t g; osg_gateway_layout_t layout={0}; lifecycle_store_t store={0};
    assert(osg_gateway_init(&g,65536,NULL)); (void)discover(&g,1,0);
    assert(osg_gateway_activate(&g,&g.pending,true)); osg_gateway_set_running(&g,true);
    osg_gateway_message_t assigned=discover(&g,1,0),pressed=input(assigned,1);
    assert(receive(&g,&pressed,1,NULL)==OSG_GW_RX_ACCEPTED);
    uint8_t packet[6]={1}; osg_gateway_delivery_t delivery; osg_gateway_message_t ack;
    assert(osg_gateway_delivery_prepare(&g,0,packet+1,5,&delivery)==5 && delivery.valid);
    osg_gateway_identity_t id=identity(1); assert(osg_gateway_disable(&g,&id));
    g.lease_counter=65533;
    (void)lifecycle_tick(&layout,&g,&store,10,true); (void)lifecycle_tick(&layout,&g,&store,110,true);
    assert(store.writes==1 && g.reserved_leases==1 && layout.activation.count==1);
    /* A changed boot after save receives a newer lease. Publication must then
     * issue an even newer lease rather than use a previously reserved number. */
    osg_gateway_message_t reboot=announce(1),late; reboot.boot_session=101;
    assert(receive(&g,&reboot,120,&late)==OSG_GW_RX_ASSIGNMENT && late.lease==65536+65534);
    osg_gateway_message_t newcomer=announce(2),reply;
    assert(receive(&g,&newcomer,121,&reply)==OSG_GW_RX_FULL && !strcmp(g.admission_error,"lease_exhausted"));
    assert(g.lease_counter==65534 && g.reserved_leases==1); /* Arrival cannot spend the publication budget. */
    assert(lifecycle_tick(&layout,&g,&store,210,true)==OSG_GATEWAY_LAYOUT_DETACH);
    assert(g.modules[0].lease>late.lease && g.lease_counter==65535 && !g.reserved_leases);
    assert(!g.modules[0].has_sequence && !g.modules[0].has_input);
    late.type=OSG_GW_INPUT; late.source_node=21; late.sequence=1;
    memcpy(late.report,pressed.report,sizeof late.report);
    assert(receive(&g,&late,211,NULL)==OSG_GW_RX_REJECTED);
    assert(!osg_gateway_delivery_complete(&g,&delivery,packet,sizeof packet,&ack));
    assert(receive(&g,&reboot,212,&reply)==OSG_GW_RX_ASSIGNMENT && reply.lease==g.modules[0].lease);
    assert(receive(&g,&reboot,213,&reply)==OSG_GW_RX_ASSIGNMENT && g.lease_counter==65535);
    osg_gateway_config_t configuration={0}; osg_gateway_cockpit_request_t request;
    assert(osg_gateway_cockpit_parse("cockpit 1 65536 config 00000000000000000000000000000001 101 get_active",&request));
    assert(!strcmp(osg_gateway_config_start(&configuration,&g,&request,213),"configuration_route_not_ready"));
    osg_gateway_message_t ready=input(reply,1); ready.source_node=21;
    assert(receive(&g,&ready,214,NULL)==OSG_GW_RX_ACCEPTED);
    assert(!g.active.slots[0].enabled && !osg_gateway_config_start(&configuration,&g,&request,214));
    osg_gateway_config_cancel(&configuration); /* Hidden controllers remain configurable after new-lease proof. */
    assert(lifecycle_tick(&layout,&g,&store,460,true)==OSG_GATEWAY_LAYOUT_ATTACH);
    assert(osg_gateway_enable(&g,&id)); unsigned writes=store.writes;
    (void)lifecycle_tick(&layout,&g,&store,470,true); (void)lifecycle_tick(&layout,&g,&store,570,true);
    assert(!strcmp(g.layout_error,"lease_exhausted") && store.writes==writes && !g.active.slots[0].enabled);
    (void)lifecycle_tick(&layout,&g,&store,571,true); assert(store.writes==writes && !layout.pending);

    /* An offline remembered identity is reserved too: it may announce between
     * save and detach, and its pre-publication input still must be rejected. */
    osg_gateway_registry_t remembered=store.saved; memset(&layout,0,sizeof layout); memset(&store,0,sizeof store);
    assert(osg_gateway_init(&g,131072,&remembered)); g.lease_counter=65533;
    assert(osg_gateway_enable(&g,&id));
    (void)lifecycle_tick(&layout,&g,&store,0,true); (void)lifecycle_tick(&layout,&g,&store,100,true);
    assert(g.reserved_leases==1 && !g.modules[0].online);
    assigned=discover(&g,1,110); assert(assigned.lease==131072+65534);
    assert(lifecycle_tick(&layout,&g,&store,200,true)==OSG_GATEWAY_LAYOUT_DETACH);
    assert(g.modules[0].online && g.modules[0].lease==131072+65535);
    pressed=input(assigned,1); assert(receive(&g,&pressed,201,NULL)==OSG_GW_RX_REJECTED);
    assert(lifecycle_tick(&layout,&g,&store,450,true)==OSG_GATEWAY_LAYOUT_ATTACH);
    assigned=discover(&g,1,451); pressed=input(assigned,1);
    assert(receive(&g,&pressed,452,NULL)==OSG_GW_RX_ACCEPTED); check_report(&g,0,true);
    unsigned unchanged=g.lease_counter; assert(osg_gateway_activate(&g,&g.pending,false));
    assert(g.lease_counter==unchanged && g.modules[0].has_input); /* No visibility change, no rearm. */
}
static void snapshot(void) {
    osg_gateway_registry_t r={0};
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) r.slots[i]=(osg_gateway_slot_t){.occupied=true,.enabled=true,.report_id=(uint8_t)(i+1),.order=(uint8_t)i,.identity=identity(i+1),.profile=maximum_profile()};
    osg_gateway_t g; assert(osg_gateway_init(&g,0x10000,&r));
    char output[OSG_GW_COCKPIT_RESPONSE_CAPACITY];
    assert(osg_gateway_cockpit_response(&g,"0123456789abcdef01234567",123,NULL,output,sizeof output));
    fputs(output,stdout);
}
static void cockpit_case(const char *name,const osg_gateway_t *g,uint32_t id,const char *error,bool last) {
    char output[OSG_GW_COCKPIT_RESPONSE_CAPACITY];
    assert(osg_gateway_cockpit_response(g,"0123456789abcdef01234567",id,error,output,sizeof output));
    printf("  \"%s\": %s%s\n",name,output,last?"":",");
}
static void cockpit_vectors(void) {
    osg_gateway_t g; assert(osg_gateway_init(&g,0x10000,NULL));
    osg_gateway_message_t msg=announce(1); msg.profile=maximum_profile(); osg_gateway_message_t reply;
    assert(receive(&g,&msg,0,&reply)==OSG_GW_RX_ASSIGNMENT);
    puts("{"); cockpit_case("enrolled_pending",&g,1,NULL,false);
    osg_gateway_identity_t id=identity(1); assert(osg_gateway_register(&g,&id));
    cockpit_case("pending_profile",&g,2,NULL,false);
    osg_gateway_apply_layout(&g); osg_gateway_set_running(&g,true);
    assert(receive(&g,&msg,0,&reply)==OSG_GW_RX_ASSIGNMENT);
    msg=reply; msg.source_node=21; msg.type=OSG_GW_INPUT; msg.sequence=1;
    for(unsigned i=0;i<sizeof msg.report;i++) msg.report[i]=(uint8_t)(i+1);
    msg.report[16]=0x70; msg.report[17]=0x88;
    assert(receive(&g,&msg,1,NULL)==OSG_GW_RX_ACCEPTED);
    cockpit_case("active_report",&g,3,NULL,false);
    assert(osg_gateway_disable(&g,&id)); cockpit_case("staged_disabled",&g,11,NULL,false);
    assert(osg_gateway_enable(&g,&id));

    msg.type=OSG_GW_HEARTBEAT; msg.identity=id; msg.sequence=2; msg.direct_usb=true;
    assert(receive(&g,&msg,2,NULL)==OSG_GW_RX_ACCEPTED);
    cockpit_case("direct_usb",&g,4,NULL,false);
    osg_gateway_tick(&g,1502); cockpit_case("offline",&g,5,NULL,false);
    cockpit_case("stale_epoch",&g,6,"stale_epoch",false);
    msg=announce(1); msg.profile.hat_count=3; msg.profile.revision=2;
    assert(receive(&g,&msg,1503,&reply)==OSG_GW_RX_ASSIGNMENT);
    cockpit_case("profile_changed",&g,7,NULL,false);
    assert(osg_gateway_register(&g,&id)); cockpit_case("profile_updated_pending",&g,8,NULL,false);
    assert(osg_gateway_disable(&g,&id)); osg_gateway_apply_layout(&g);
    cockpit_case("disabled",&g,9,NULL,false);
    assert(osg_gateway_enable(&g,&id)); cockpit_case("enable_pending",&g,10,NULL,true);
    puts("}");
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--snapshot")) { snapshot(); return 0; }
    if(argc==2 && !strcmp(argv[1],"--cockpit-vectors")) { cockpit_vectors(); return 0; }
    protocol_test(); lifecycle_test(); bounds_and_wrap_test(); registry_test(); descriptor_test(); maximum_profile_test(); delivery_test(); transport_source_test(); cockpit_test(); configuration_test(); action_test(); automatic_lifecycle_test(); disabled_snapshot_test(); publication_lease_test();
    puts("gateway core: all tests passed"); return 0;
}
