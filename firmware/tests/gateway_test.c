#include "opensimgear/gateway.h"
#include "opensimgear/gateway_cockpit.h"
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
        .boot_session=100,.source_node=(uint8_t)(20+id),.profile=profile()};
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
    for(unsigned i=1;i<=10;i++) {
        osg_gateway_message_t a=discover(&g,i,0xfffffff0U); assert(a.node==i);
        osg_gateway_identity_t id=identity(i); assert(osg_gateway_register(&g,&id));
    }
    osg_gateway_message_t eleventh=announce(11);
    assert(receive(&g,&eleventh,0xfffffff1U,&reply)==OSG_GW_RX_FULL);
    osg_gateway_apply_layout(&g); osg_gateway_set_running(&g,true);
    osg_gateway_message_t a=discover(&g,1,0xfffffff2U),in=input(a,0xfffffffeU);
    assert(receive(&g,&in,0xfffffff3U,NULL)==OSG_GW_RX_ACCEPTED);
    in.sequence=0xffffffffU; assert(receive(&g,&in,0xfffffff4U,NULL)==OSG_GW_RX_ACCEPTED);
    in.sequence=0; assert(receive(&g,&in,0xfffffff5U,NULL)==OSG_GW_RX_ACCEPTED);
    in.sequence=0xffffffffU; assert(receive(&g,&in,0xfffffff6U,NULL)==OSG_GW_RX_REJECTED);
    osg_gateway_tick(&g,0x20); check_report(&g,0,true);
    osg_gateway_tick(&g,0xef); check_report(&g,0,false);
    osg_gateway_tick(&g,0x5dc); assert(!g.modules[0].online);
    assert(receive(&g,&eleventh,0x5dd,&reply)==OSG_GW_RX_FULL); /* registered offline reserves identity */
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
    r.slots[2]=(osg_gateway_slot_t){.occupied=true,.identity=identity(7),.profile=profile()};
    uint8_t record[500],next[500],partial[500];
    assert(osg_gateway_registry_encode(&r,record,sizeof record)==500);
    assert(osg_gateway_registry_decode(record,sizeof record,&decoded));
    assert(decoded.slots[2].occupied && !decoded.slots[0].occupied);
    /* Preserve the exact 500-byte v1 record, then write the upgraded format. */
    uint8_t legacy[500]; memcpy(legacy,record,sizeof legacy); legacy[4]=1;
    uint32_t crc=osg_gateway_crc32(legacy,496);
    for(unsigned i=0;i<4;i++) legacy[496+i]=(uint8_t)(crc>>(8*i));
    assert(osg_gateway_registry_decode(legacy,sizeof legacy,&decoded) && !decoded.slots[2].profile.hat_count);
    assert(osg_gateway_registry_encode(&decoded,legacy,sizeof legacy)==500 && legacy[4]==2);
    r.slots[2].profile.hat_count=3;
    assert(osg_gateway_registry_encode(&r,legacy,sizeof legacy)==500);
    assert(osg_gateway_registry_decode(legacy,sizeof legacy,&decoded) && decoded.slots[2].profile.hat_count==3);
    legacy[4]=1; crc=osg_gateway_crc32(legacy,496);
    for(unsigned i=0;i<4;i++) legacy[496+i]=(uint8_t)(crc>>(8*i));
    assert(!osg_gateway_registry_decode(legacy,sizeof legacy,&decoded));
    r.slots[2].profile.hat_count=0;
    r.generation=0; assert(osg_gateway_registry_encode(&r,next,sizeof next)==500);
    assert(osg_gateway_registry_select(record,500,next,500,&decoded)==1 && decoded.generation==0);
    for(unsigned i=0;i<500;i++) {
        memcpy(partial,next,500); partial[i]^=1;
        assert(!osg_gateway_registry_decode(partial,500,&decoded));
        assert(osg_gateway_registry_select(record,500,partial,500,&decoded)==0);
        memset(partial,0xff,500); memcpy(partial,next,i); /* interrupted inactive write */
        assert(osg_gateway_registry_select(record,500,partial,500,&decoded)==0);
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
    assert(empty && before[0]==0x06); /* vendor, no joystick before registration */
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
    for(unsigned i=0;i<10;i++) r.slots[i]=(osg_gateway_slot_t){.occupied=true,.identity=identity(i+1),.profile=maximum_profile()};
    osg_gateway_t g; assert(osg_gateway_init(&g,0x60000,&r));
    uint8_t descriptor[OSG_GW_DESCRIPTOR_CAPACITY],report[OSG_GW_MAX_REPORT_SIZE];
    size_t length=osg_gateway_descriptor(&g,descriptor,sizeof descriptor); assert(length && length<=sizeof descriptor);
    unsigned bit_count[11]={0},item_size=0,item_count=0,report_id=0;
    for(size_t i=0;i<length;) {
        uint8_t item=descriptor[i++]; unsigned bytes=item&3; if(bytes==3) bytes=4;
        assert(item!=0xfe && i+bytes<=length); uint32_t value=0;
        for(unsigned j=0;j<bytes;j++) value|=(uint32_t)descriptor[i+j]<<(8*j);
        if(item==0x85) { report_id=value; assert(report_id>=1 && report_id<=10); }
        if(item==0x75) item_size=value;
        if(item==0x95) item_count=value;
        if((item&0xfc)==0x80) bit_count[report_id]+=item_size*item_count;
        i+=bytes;
    }
    for(unsigned i=1;i<=10;i++) assert(bit_count[i]==34*8);
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
static bool persist(void *context) { unsigned *calls=context; (*calls)++; return *calls>1; }
static void cockpit_test(void) {
    osg_gateway_cockpit_request_t r;
    assert(osg_gateway_cockpit_parse("cockpit 4294967295 snapshot",&r) && r.id==UINT32_MAX);
    const char *invalid[]={"cockpit 4294967296 snapshot","cockpit -1 snapshot","cockpit 1 snapshot ",
        "cockpit 1 0 save","cockpit 1 save","cockpit 1 1 apply x","cockpit 1 1 register bad"};
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++) assert(!osg_gateway_cockpit_parse(invalid[i],&r));
    osg_gateway_t g; assert(osg_gateway_init(&g,0x10000,NULL)); discover(&g,1,0);
    char id[33],line[80]; osg_gateway_identity_t module=identity(1); osg_gateway_identity_format(&module,id);
    snprintf(line,sizeof line,"cockpit 2 1 register %s",id); assert(osg_gateway_cockpit_parse(line,&r));
    bool apply=true; unsigned calls=0;
    assert(!strcmp(osg_gateway_cockpit_execute(&g,&r,persist,&calls,&apply),"stale_epoch") && !apply && !calls);
    assert(!g.pending.slots[0].occupied);
    snprintf(line,sizeof line,"cockpit 2 65536 register %s",id); assert(osg_gateway_cockpit_parse(line,&r));
    assert(!osg_gateway_cockpit_execute(&g,&r,persist,&calls,&apply) && !apply);
    assert(osg_gateway_cockpit_parse("cockpit 3 65536 apply",&r));
    assert(!strcmp(osg_gateway_cockpit_execute(&g,&r,persist,&calls,&apply),"flash_save_failed") && !apply && calls==1);
    assert(!osg_gateway_cockpit_execute(&g,&r,persist,&calls,&apply) && apply && calls==2);
    assert(!g.active.slots[0].occupied); /* caller must physically detach before applying */
    char output[OSG_GW_COCKPIT_RESPONSE_CAPACITY];
    assert(osg_gateway_cockpit_response(&g,"0123456789abcdef01234567",3,"stale_epoch",output,sizeof output));
    assert(strstr(output,"\"id\":3,\"ok\":false") && strstr(output,"\"needsApply\":true"));
    assert(!osg_gateway_cockpit_response(&g,"0123456789abcdef01234567",3,NULL,output,10) && !output[0]);
}
static void snapshot(void) {
    osg_gateway_registry_t r={0};
    for(unsigned i=0;i<10;i++) r.slots[i]=(osg_gateway_slot_t){.occupied=true,.identity=identity(i+1),.profile=maximum_profile()};
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
    puts("{"); cockpit_case("discovered_unregistered",&g,1,NULL,false);
    osg_gateway_identity_t id=identity(1); assert(osg_gateway_register(&g,&id));
    cockpit_case("pending_profile",&g,2,NULL,false);
    osg_gateway_apply_layout(&g); osg_gateway_set_running(&g,true);
    msg=reply; msg.source_node=21; msg.type=OSG_GW_INPUT; msg.sequence=1;
    for(unsigned i=0;i<sizeof msg.report;i++) msg.report[i]=(uint8_t)(i+1);
    msg.report[16]=0x70; msg.report[17]=0x88;
    assert(receive(&g,&msg,1,NULL)==OSG_GW_RX_ACCEPTED);
    cockpit_case("active_report",&g,3,NULL,false);
    msg.type=OSG_GW_HEARTBEAT; msg.identity=id; msg.sequence=2; msg.direct_usb=true;
    assert(receive(&g,&msg,2,NULL)==OSG_GW_RX_ACCEPTED);
    cockpit_case("direct_usb",&g,4,NULL,false);
    osg_gateway_tick(&g,1502); cockpit_case("offline",&g,5,NULL,false);
    cockpit_case("stale_epoch",&g,6,"stale_epoch",false);
    msg=announce(1); msg.profile.hat_count=3; msg.profile.revision=2;
    assert(receive(&g,&msg,1503,&reply)==OSG_GW_RX_ASSIGNMENT);
    cockpit_case("profile_changed",&g,7,NULL,false);
    assert(osg_gateway_register(&g,&id)); cockpit_case("profile_updated_pending",&g,8,NULL,true);
    puts("}");
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--snapshot")) { snapshot(); return 0; }
    if(argc==2 && !strcmp(argv[1],"--cockpit-vectors")) { cockpit_vectors(); return 0; }
    protocol_test(); lifecycle_test(); bounds_and_wrap_test(); registry_test(); descriptor_test(); maximum_profile_test(); delivery_test(); transport_source_test(); cockpit_test();
    puts("gateway core: all tests passed"); return 0;
}
