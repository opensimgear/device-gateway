#include "opensimgear/gateway.h"
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
        .boot_session=100,.profile=profile()};
}
static osg_gateway_receive_result_t receive(osg_gateway_t *g,const osg_gateway_message_t *msg,
                                            uint32_t now,osg_gateway_frame_t *reply) {
    osg_gateway_frame_t frame; assert(osg_gateway_frame_encode(msg,&frame));
    return osg_gateway_receive(g,&frame,now,reply);
}
static osg_gateway_message_t discover(osg_gateway_t *g,unsigned id,uint32_t now) {
    osg_gateway_message_t msg=announce(id),assignment; osg_gateway_frame_t reply;
    assert(receive(g,&msg,now,&reply)==OSG_GW_RX_ASSIGNMENT);
    assert(osg_gateway_frame_decode(&reply,&assignment)); return assignment;
}
static osg_gateway_message_t input(osg_gateway_message_t assignment,uint32_t seq) {
    assignment.type=OSG_GW_INPUT; assignment.gateway_epoch=0; assignment.sequence=seq;
    memset(assignment.report,0,24); assignment.report[0]=5;
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
    osg_gateway_message_t msg=announce(1),decoded; osg_gateway_frame_t frame;
    assert(osg_gateway_frame_encode(&msg,&frame));
    assert(osg_gateway_frame_decode(&frame,&decoded));
    assert(osg_gateway_identity_equal(&decoded.identity,&msg.identity));
    assert(osg_gateway_profile_equal(&decoded.profile,&msg.profile));
    for(unsigned i=0;i<frame.length;i++) {
        osg_gateway_frame_t corrupt=frame; corrupt.data[i]^=0x80;
        assert(!osg_gateway_frame_decode(&corrupt,&decoded));
    }
    for(unsigned i=0;i<64;i++) if(i!=48) {
        osg_gateway_frame_t shorter=frame; shorter.length=(uint8_t)i;
        assert(!osg_gateway_frame_decode(&shorter,&decoded));
    }
    osg_gateway_frame_t bad=frame; bad.id^=1; assert(!osg_gateway_frame_decode(&bad,&decoded));
    bad=frame; bad.extended=false; assert(!osg_gateway_frame_decode(&bad,&decoded));
    bad=frame; bad.fd=false; assert(!osg_gateway_frame_decode(&bad,&decoded));
    bad=frame; bad.brs=false; assert(!osg_gateway_frame_decode(&bad,&decoded));
    msg.profile.axis_usage[1]=0x30; assert(!osg_gateway_frame_encode(&msg,&frame));
    msg.profile=profile(); msg.profile.axis_neutral[7]=1; assert(!osg_gateway_frame_encode(&msg,&frame));
    msg.profile=profile(); msg.profile.button_count=65; assert(!osg_gateway_frame_encode(&msg,&frame));
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
    stale=in; stale.identity=identity(2); stale.sequence=8; assert(receive(&g,&stale,259,NULL)==OSG_GW_RX_REJECTED);
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
    osg_gateway_frame_t reply;
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
    osg_gateway_identity_t id=identity(1); osg_gateway_frame_t reply;
    osg_gateway_message_t changed=announce(1); changed.profile.button_count=4;
    assert(receive(&g,&changed,2001,&reply)==OSG_GW_RX_ASSIGNMENT);
    assert(osg_gateway_descriptor(&g,after,sizeof after)==n && !memcmp(before,after,n));
    assert(osg_gateway_register(&g,&id)); assert(osg_gateway_needs_apply(&g));
    osg_gateway_apply_layout(&g); assert(osg_gateway_descriptor(&g,after,sizeof after)==n);
    assert(memcmp(before,after,n));
}
static void maximum_profile_test(void) {
    osg_gateway_registry_t r={0};
    for(unsigned i=0;i<10;i++) {
        osg_gateway_profile_t p={.button_count=64,.axis_count=8,.revision=1};
        for(unsigned a=0;a<8;a++) { p.axis_usage[a]=(uint8_t)(0x30+a); p.axis_neutral[a]=(uint16_t)(a*9000); }
        r.slots[i]=(osg_gateway_slot_t){.occupied=true,.identity=identity(i+1),.profile=p};
    }
    osg_gateway_t g; assert(osg_gateway_init(&g,0x60000,&r));
    uint8_t descriptor[1024],report[24];
    size_t length=osg_gateway_descriptor(&g,descriptor,sizeof descriptor); assert(length && length<=1024);
    osg_gateway_message_t msg=announce(1); msg.profile=r.slots[0].profile;
    osg_gateway_frame_t assignment; assert(receive(&g,&msg,0,&assignment)==OSG_GW_RX_ASSIGNMENT);
    assert(osg_gateway_frame_decode(&assignment,&msg));
    msg.type=OSG_GW_INPUT; msg.sequence=1;
    for(unsigned i=0;i<24;i++) msg.report[i]=(uint8_t)(i+1);
    osg_gateway_set_running(&g,true); assert(receive(&g,&msg,1,NULL)==OSG_GW_RX_ACCEPTED);
    assert(osg_gateway_report(&g,0,report,sizeof report)==24 && !memcmp(report,msg.report,24));
    assert(!osg_gateway_report(&g,0,report,23));
    osg_gateway_tick(&g,251); assert(osg_gateway_report(&g,0,report,sizeof report)==24);
    for(unsigned i=0;i<8;i++) assert(report[i]==0);
    for(unsigned i=0;i<8;i++) {
        uint16_t actual=(uint16_t)(report[8+2*i]|((uint16_t)report[9+2*i]<<8));
        assert(actual==r.slots[0].profile.axis_neutral[i]);
    }
    /* Registered profile stays authoritative until explicit apply. */
    msg=announce(1); msg.profile=r.slots[0].profile; msg.profile.revision=2;
    assert(receive(&g,&msg,252,&assignment)==OSG_GW_RX_ASSIGNMENT);
    assert(osg_gateway_frame_decode(&assignment,&msg)); msg.type=OSG_GW_INPUT; msg.sequence=1;
    memset(msg.report,0xff,sizeof msg.report);
    assert(receive(&g,&msg,253,NULL)==OSG_GW_RX_ACCEPTED);
    assert(osg_gateway_report(&g,0,report,sizeof report)==24 && report[0]==0);
    assert(osg_gateway_register(&g,&msg.identity)); osg_gateway_apply_layout(&g); osg_gateway_set_running(&g,true);
    msg.sequence=2; assert(receive(&g,&msg,254,NULL)==OSG_GW_RX_ACCEPTED);
    assert(osg_gateway_report(&g,0,report,sizeof report)==24 && report[0]==0xff);
}
static void vectors(void) {
    osg_gateway_message_t messages[4]={0};
    messages[0]=announce(1);
    assert(osg_gateway_identity_parse("00112233445566778899aabbccddeeff",&messages[0].identity));
    messages[0].boot_session=0x10203040;
    messages[1]=(osg_gateway_message_t){.type=OSG_GW_ASSIGN,.node=1,.identity=messages[0].identity,
        .boot_session=messages[0].boot_session,.lease=0x11223344,.gateway_epoch=0x55667788};
    messages[2]=input(messages[1],9); messages[3]=messages[1];
    messages[3].type=OSG_GW_HEARTBEAT; messages[3].gateway_epoch=0; messages[3].sequence=10;
    const char *names[]={"announce","assign","input","heartbeat"}; puts("{");
    for(unsigned i=0;i<4;i++) {
        osg_gateway_frame_t frame; assert(osg_gateway_frame_encode(&messages[i],&frame));
        printf("  \"%s\": {\"id\": \"%08x\", \"length\": %u, \"data\": \"",names[i],(unsigned)frame.id,frame.length);
        for(unsigned j=0;j<frame.length;j++) printf("%02x",frame.data[j]);
        printf("\"}%s\n",i==3?"":",");
    }
    puts("}");
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--vectors")) { vectors(); return 0; }
    protocol_test(); lifecycle_test(); bounds_and_wrap_test(); registry_test(); descriptor_test(); maximum_profile_test();
    puts("gateway core: all tests passed"); return 0;
}
