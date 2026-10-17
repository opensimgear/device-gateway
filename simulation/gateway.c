#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
/* Native gateway hardware boundary. The registry, flash journal, CDC parser,
 * CAN transfer handling and HID descriptors are production firmware sources. */
#include "opensimgear/gateway_config.h"
#include "opensimgear/gateway_layout.h"
#include "opensimgear/gateway_publication.h"
#include "opensimgear/line.h"
#include "gateway_journal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct { uint8_t pages[2][OSG_JOURNAL_PAGE_SIZE]; int fault; const char *name; uint32_t erases,programs; } flash_t;
static struct {
    osg_gateway_t gateway;
    osg_cyphal_t gateway_can;
    osg_gateway_config_t config;
    osg_gateway_layout_t layout;
    uint8_t gateway_descriptor[OSG_GW_DESCRIPTOR_CAPACITY];
    size_t descriptor_length;
    flash_t registry_flash;
    osg_gateway_identity_t gateway_identity;
    uint32_t now,can_frames,ignored_config,node_collisions;
    bool disconnected,drop_reply,drop_action_reply;
    char response[OSG_GW_COCKPIT_RESPONSE_CAPACITY];
} simulation;
static const char *state_directory;
static char gateway_uid[25]="00112233445566778899aabb";
static FILE *response_output;
static volatile sig_atomic_t stopped;
static void standalone_cycle(bool wait);
static bool bus_send(const osg_gateway_frame_t *frame);
static void publish_gateway(bool publish);
static void detach_cdc(void);
static bool cdc_drained(void);

static void disk_save(flash_t *flash) {
    if(!state_directory) return;
    char path[1024]; int n=snprintf(path,sizeof path,"%s/%s.bin",state_directory,flash->name);
    assert(n>0 && (size_t)n<sizeof path);
    FILE *file=fopen(path,"wb"); assert(file);
    assert(fwrite(flash->pages,sizeof flash->pages,1,file)==1); assert(!fclose(file));
}
static void flash_init(flash_t *flash,const char *name) {
    memset(flash->pages,255,sizeof flash->pages); flash->fault=-1; flash->name=name;
    if(!state_directory) return;
    char path[1024]; int n=snprintf(path,sizeof path,"%s/%s.bin",state_directory,name);
    assert(n>0 && (size_t)n<sizeof path); FILE *file=fopen(path,"rb");
    if(file) { assert(fread(flash->pages,sizeof flash->pages,1,file)==1); assert(!fclose(file)); }
}
static const uint8_t *flash_read(void *context,unsigned page) {
    flash_t *flash=context; return page<2 ? flash->pages[page] : NULL;
}
static bool interrupted(flash_t *flash) {
    if(flash->fault<0) return false;
    if(!flash->fault) { flash->fault=-1; return true; }
    flash->fault--; return false;
}
static bool flash_erase(void *context,unsigned page) {
    flash_t *flash=context; if(page>1) return false;
    flash->erases++;
    bool fail=interrupted(flash); memset(flash->pages[page],255,fail ? OSG_JOURNAL_PAGE_SIZE/2 : OSG_JOURNAL_PAGE_SIZE);
    disk_save(flash); return !fail;
}
static bool flash_program(void *context,unsigned page,unsigned offset,const uint8_t data[8]) {
    flash_t *flash=context; if(page>1 || offset%8 || offset+8>OSG_JOURNAL_PAGE_SIZE) return false;
    flash->programs++;
    bool fail=interrupted(flash); unsigned length=fail ? 4U : 8U;
    for(unsigned i=0;i<length;i++) {
        if((flash->pages[page][offset+i]&data[i])!=data[i]) return false;
        flash->pages[page][offset+i]&=data[i];
    }
    disk_save(flash); return !fail;
}
static osg_journal_io_t io(flash_t *flash) {
    return (osg_journal_io_t){.context=flash,.read=flash_read,.erase=flash_erase,.program=flash_program};
}
static bool gateway_send(void *context,const osg_gateway_frame_t *frame) {
    (void)context; return bus_send(frame);
}
static bool persist(void *context,const osg_gateway_registry_t *registry) {
    (void)context; uint8_t record[OSG_GW_REGISTRY_RECORD_SIZE];
    size_t n=osg_gateway_registry_encode(registry,record,sizeof record);
    osg_journal_io_t flash=io(&simulation.registry_flash);
    return n && osg_journal_commit(&flash,record,n,simulation.gateway.epoch);
}
static void prepare_descriptor(void) {
    simulation.descriptor_length=osg_gateway_descriptor(&simulation.gateway,simulation.gateway_descriptor,sizeof simulation.gateway_descriptor);
    assert(osg_gateway_registry_valid(&simulation.gateway.active));
}
static void boot_gateway(void) {
    osg_cyphal_destroy(&simulation.gateway_can);
    uint8_t record[OSG_GW_REGISTRY_RECORD_SIZE]; size_t n=0; uint32_t epoch=0;
    osg_gateway_registry_t registry,*restored=NULL; osg_journal_io_t flash=io(&simulation.registry_flash);
    if(osg_journal_load(&flash,record,sizeof record,&n,&epoch)) { assert(osg_gateway_registry_decode(record,n,&registry)); restored=&registry; }
    else assert(osg_journal_erased(&flash));
    assert(epoch<=UINT32_MAX-65536U); assert(osg_gateway_init(&simulation.gateway,epoch+65536U,restored));
    assert(osg_gateway_publication_identity(&simulation.gateway,&simulation.gateway_identity));
    assert(persist(NULL,&simulation.gateway.pending)); assert(osg_cyphal_init(&simulation.gateway_can,&simulation.gateway_identity,simulation.gateway.epoch,
        "org.opensimgear.gateway",gateway_send,NULL));
    osg_gateway_config_cancel(&simulation.config); memset(&simulation.layout,0,sizeof simulation.layout);
    prepare_descriptor(); osg_gateway_set_running(&simulation.gateway,!simulation.disconnected);
}
static void receive_gateway(const osg_gateway_frame_t *frame) {
    simulation.can_frames++;
    osg_gateway_message_t m,reply;
    uint8_t source=osg_cyphal_node_id(&simulation.gateway_can);
    bool received=osg_cyphal_receive(&simulation.gateway_can,frame,simulation.now,&m);
    if(source!=osg_cyphal_node_id(&simulation.gateway_can)) {
        simulation.node_collisions++;osg_cyphal_reset(&simulation.gateway_can);
        osg_gateway_bus_reset(&simulation.gateway);return;
    }
    if(!received) return;
    if(m.type==OSG_GW_CONFIG_RESPONSE) {
        if(simulation.drop_reply || (simulation.drop_action_reply && simulation.config.action &&
            !strncmp(simulation.config.command,"action invoke ",14))) simulation.ignored_config++;
        else (void)osg_gateway_config_receive(&simulation.config,&simulation.gateway,&m);
    } else {
        if(osg_gateway_receive(&simulation.gateway,&m,simulation.now,&reply)==OSG_GW_RX_ASSIGNMENT)
            assert(osg_cyphal_send(&simulation.gateway_can,&reply,simulation.now));
    }
}
static void tick(void) {
    osg_gateway_tick(&simulation.gateway,simulation.now);
    osg_gateway_layout_event_t event=osg_gateway_layout_service(&simulation.layout,&simulation.gateway,simulation.now,
        !simulation.config.busy && cdc_drained(),persist,NULL);
    if(event==OSG_GATEWAY_LAYOUT_DETACH) {
        osg_gateway_set_running(&simulation.gateway,false);
        publish_gateway(false); detach_cdc(); osg_gateway_config_cancel(&simulation.config);
        (void)osg_gateway_layout_activate(&simulation.layout,&simulation.gateway,simulation.now); prepare_descriptor();
    } else if(event==OSG_GATEWAY_LAYOUT_ATTACH) {
        osg_gateway_set_running(&simulation.gateway,!simulation.disconnected);
        publish_gateway(true);
    }
    osg_cyphal_tick(&simulation.gateway_can,simulation.now);
    osg_gateway_config_tick(&simulation.config,&simulation.gateway,&simulation.gateway_can,simulation.now);
    if(simulation.gateway.running) for(unsigned slot=0;slot<OSG_GW_MAX_MODULES;slot++) {
        uint8_t packet[OSG_GW_MAX_REPORT_SIZE+1]; packet[0]=simulation.gateway.active.slots[slot].report_id;
        osg_gateway_delivery_t delivery; osg_gateway_message_t ack;
        size_t n=osg_gateway_delivery_prepare(&simulation.gateway,slot,packet+1,sizeof packet-1,&delivery);
        if(n && osg_gateway_delivery_complete(&simulation.gateway,&delivery,packet,n+1,&ack))
            assert(osg_cyphal_send(&simulation.gateway_can,&ack,simulation.now));
    }
}
static void advance(unsigned milliseconds) {
    uint32_t start=simulation.now;
    do { standalone_cycle(true); } while(!stopped && (uint32_t)(simulation.now-start)<milliseconds);
}
static void hex(const uint8_t *bytes,size_t n) { for(size_t i=0;i<n;i++) fprintf(response_output,"%02x",bytes[i]); }
static void inspect(uint32_t id,bool ok,const char *error) {
    fprintf(response_output,"{\"id\":%u,\"ok\":%s,\"error\":",id,ok?"true":"false");
    if(error) fprintf(response_output,"\"%s\"",error); else fprintf(response_output,"null");
    fprintf(response_output,",\"simulation\":{\"clockMs\":%u,\"gatewayEpoch\":%u,\"gatewayReconnects\":%u,\"canFrames\":%u,\"ignoredConfigChunks\":%u,\"activeGeneration\":%u,\"pendingGeneration\":%u,\"registryErases\":%u,\"registryPrograms\":%u,\"descriptorHex\":\"",
        simulation.now,simulation.gateway.epoch,simulation.layout.reconnects,
        simulation.can_frames,simulation.ignored_config,simulation.gateway.active.generation,
        simulation.gateway.pending.generation,simulation.registry_flash.erases,simulation.registry_flash.programs);
    hex(simulation.gateway_descriptor,simulation.descriptor_length);
    fprintf(response_output,"\",\"gatewayReports\":["); bool first=true;
    for(unsigned slot=0;slot<OSG_GW_MAX_MODULES;slot++) {
        uint8_t bytes[OSG_GW_MAX_REPORT_SIZE]; size_t size=osg_gateway_report(&simulation.gateway,slot,bytes,sizeof bytes);
        if(!size) continue;
        fprintf(response_output,"%s{\"reportId\":%u,\"hex\":\"",first?"":",",simulation.gateway.active.slots[slot].report_id);
        hex(bytes,size); fprintf(response_output,"\"}"); first=false;
    }
    fprintf(response_output,"],\"sourceNode\":%u,\"nodeCollisions\":%u,\"layoutDraft\":%u,\"layoutPending\":%u,\"layoutDetached\":%u,\"layoutApplyAt\":%u,\"layoutReattachAt\":%u,\"layoutTargetGeneration\":%u,\"configurationBusy\":%u}}\n",
        osg_cyphal_node_id(&simulation.gateway_can),simulation.node_collisions,
        simulation.layout.draft,simulation.layout.pending,simulation.layout.detached,
        simulation.layout.apply_at,simulation.layout.reattach_at,simulation.layout.target.generation,simulation.config.busy);
    fflush(response_output);
}
static void admin(const char *line) {
    unsigned id=0,n=0,a=0; bool ok=true; const char *error=NULL;
    if(sscanf(line,"sim %u %n",&id,&n)!=1 || !n) { inspect(id,false,"invalid_request"); return; }
    const char *command=line+n;
    if(!strcmp(command,"inspect")) {}
    else if(!strcmp(command,"reboot")) { boot_gateway(); publish_gateway(true); advance(600); }
    else if(sscanf(command,"fault %u",&a)==1 && a<=100000) simulation.registry_flash.fault=(int)a;
    else if(sscanf(command,"advance %u",&a)==1 && a<=60000) advance(a);
    else if(sscanf(command,"disconnect %u",&a)==1 && a<=1) {
        simulation.disconnected=a!=0;
        if(simulation.disconnected) osg_gateway_publication_transport_lost(&simulation.gateway,simulation.now);
        osg_gateway_set_running(&simulation.gateway,!simulation.disconnected && !simulation.layout.detached);
        advance(30);
    } else if(sscanf(command,"drop_reply %u",&a)==1 && a<=1) simulation.drop_reply=a!=0;
    else if(sscanf(command,"drop_action_reply %u",&a)==1 && a<=1) simulation.drop_action_reply=a!=0;
    else { ok=false; error="invalid_request"; }
    inspect(id,ok,error);
}
static void command(const char *line) {
    if(!strcmp(line,"identity") || !strcmp(line,"hello") || !strcmp(line,"info")) {
        fprintf(response_output,"gatewayType=stm32-can-gateway protocol=3 canProtocol=3 firmware=0.4.0 board=native-gateway identity=%s epoch=%u maxModules=%u\r\n",gateway_uid,simulation.gateway.epoch,OSG_GW_MAX_MODULES); fflush(response_output); return;
    }
    osg_gateway_cockpit_request_t request={0}; const char *error=NULL,*json=NULL; bool apply=false;
    if(!osg_gateway_cockpit_parse(line,&request)) error="invalid_request";
    else if(request.action==OSG_GW_COCKPIT_CONFIG || request.action==OSG_GW_COCKPIT_ACTION || request.action==OSG_GW_COCKPIT_EVENTS) {
        error=osg_gateway_config_start(&simulation.config,&simulation.gateway,&request,simulation.now);
        if(!error) {
            while(simulation.config.busy && !simulation.config.complete && !stopped) {
                standalone_cycle(true);
            }
            if(stopped) return;
            assert(osg_gateway_config_result(&simulation.config,&error,&json));
        }
    } else error=osg_gateway_cockpit_execute_at(&simulation.gateway,&request,NULL,NULL,&apply,simulation.now);
    size_t n=request.action==OSG_GW_COCKPIT_EVENTS ?
        osg_gateway_cockpit_events_response(&simulation.gateway,gateway_uid,request.id,error,json,simulation.response,sizeof simulation.response) : request.action==OSG_GW_COCKPIT_ACTION ?
        osg_gateway_cockpit_action_response(&simulation.gateway,gateway_uid,request.id,error,json,simulation.response,sizeof simulation.response) :
        request.action==OSG_GW_COCKPIT_CONFIG ? osg_gateway_cockpit_config_response(&simulation.gateway,gateway_uid,request.id,error,json,simulation.response,sizeof simulation.response) :
        osg_gateway_cockpit_request_response(&simulation.gateway,gateway_uid,&request,error,simulation.response,sizeof simulation.response);
    if(n) fputs(simulation.response,response_output);
    else fprintf(response_output,"{\"id\":%u,\"ok\":false,\"error\":\"response_overflow\",\"state\":null}\n",request.id);
    fflush(response_output);
}
/* The local bus carries complete CAN frames, never application JSON or host
 * memory structs. Local transmit deadlines are evaluated before transmission.
 * Bytes: magic[4], version, kind, flags, length, little-endian CAN ID/epoch[4],
 * join identity[16], and CAN payload[64] (or one-byte admission status). */
enum { BUS_PACKET_SIZE=92, BUS_HELLO=1, BUS_ACK=2, BUS_FRAME=3 };
typedef struct {
    bool used;
    struct sockaddr_in address;
    osg_gateway_identity_t identity;
    uint32_t seen;
    pid_t pid;
} peer_t;
static struct {
    int bus,listener,cdc,lock,state_lock;
    char root[96],bus_manifest[160],advertisement[160];
    uint16_t cdc_port;
    peer_t peers[OSG_GW_MAX_MODULES];
    bool stdin_open;
    struct timespec started;
    char input[OSG_GW_CONFIG_COMMAND_CAPACITY+128],cdc_input[OSG_GW_CONFIG_COMMAND_CAPACITY+128];
    osg_line_t input_line,cdc_line;
    char output[OSG_GW_COCKPIT_RESPONSE_CAPACITY+1024];
    size_t output_length,output_offset;
} local={.bus=-1,.listener=-1,.cdc=-1,.lock=-1,.state_lock=-1,.stdin_open=true};
static void die(const char *message) { fprintf(stderr,"%s: %s\n",message,strerror(errno)); exit(1); }
static void stop_signal(int signal_number) { (void)signal_number; stopped=1; }
static void owned_directory(const char *path) {
    struct stat info;
    if(mkdir(path,0700)<0 && errno!=EEXIST) die("Could not create local gateway directory");
    if(lstat(path,&info)<0) die("Could not inspect local gateway directory");
    if(!S_ISDIR(info.st_mode) || info.st_uid!=geteuid() || (info.st_mode&0777)!=0700) {
        errno=EPERM; die("Local gateway directories must be private and owned by the current user");
    }
}
static void make_state_directory(const char *path) {
    char scratch[1024]; size_t length=strlen(path);
    if(!length || length>=sizeof scratch) { errno=ENAMETOOLONG;die("Invalid state directory"); }
    memcpy(scratch,path,length+1);
    for(char *c=scratch+1;*c;c++) if(*c=='/') { *c=0; if(mkdir(scratch,0700)<0 && errno!=EEXIST) die("Could not create state parent"); *c='/'; }
    if(mkdir(scratch,0700)<0 && errno!=EEXIST) die("Could not create state directory");
    struct stat info;if(lstat(scratch,&info)<0 || !S_ISDIR(info.st_mode) || info.st_uid!=geteuid()) { errno=EPERM;die("State directory must belong to current user"); }
}
static void lock_state(void) {
    char path[1100];int n=snprintf(path,sizeof path,"%s/.lock",state_directory);
    if(n<0 || (size_t)n>=sizeof path) {errno=ENAMETOOLONG;die("State lock path too long");}
    local.state_lock=open(path,O_RDWR|O_CREAT|O_NOFOLLOW,0600);
    if(local.state_lock<0)die("Could not open flash state lock");
    struct stat info;
    if(fstat(local.state_lock,&info)<0 || !S_ISREG(info.st_mode) || info.st_uid!=geteuid() || (info.st_mode&0777)!=0600) {errno=EPERM;die("Flash state lock must be private and owned by current user");}
    if(flock(local.state_lock,LOCK_EX|LOCK_NB)<0)die("Flash state is already in use");
    if(fcntl(local.state_lock,F_SETFD,FD_CLOEXEC)<0)die("Could not configure flash state lock");
}
static void nonblocking(int fd) {
    int flags=fcntl(fd,F_GETFL); if(flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0) die("Could not configure local socket");
    if(fcntl(fd,F_SETFD,FD_CLOEXEC)<0) die("Could not configure close-on-exec");
}
static uint32_t wall_ms(void) {
    struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now)<0) die("Monotonic clock unavailable");
    int64_t elapsed=(int64_t)(now.tv_sec-local.started.tv_sec)*1000+(now.tv_nsec-local.started.tv_nsec)/1000000;
    return (uint32_t)elapsed;
}
static void put32(uint8_t *p,uint32_t value) { for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(value>>(8*i)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void packet_init(uint8_t packet[BUS_PACKET_SIZE],uint8_t kind) {
    memset(packet,0,BUS_PACKET_SIZE);memcpy(packet,"OSGB",4);packet[4]=1;packet[5]=kind;
}
static bool packet_valid(const uint8_t *packet,size_t length) {
    return length==BUS_PACKET_SIZE && !memcmp(packet,"OSGB",4) && packet[4]==1 &&
        packet[5]>=BUS_HELLO && packet[5]<=BUS_FRAME;
}
static bool send_packet(const struct sockaddr_in *address,const uint8_t packet[BUS_PACKET_SIZE]) {
    return sendto(local.bus,packet,BUS_PACKET_SIZE,MSG_DONTWAIT,(const struct sockaddr *)address,sizeof *address)==BUS_PACKET_SIZE;
}
static bool broadcast(const uint8_t packet[BUS_PACKET_SIZE],int except) {
    bool accepted=true;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(local.peers[i].used && (int)i!=except) {
        if(!send_packet(&local.peers[i].address,packet)) {
            if(errno==ENOENT || errno==ECONNREFUSED)local.peers[i].used=false;
            else accepted=false;
        }
    }
    return accepted;
}
static bool bus_send(const osg_gateway_frame_t *frame) {
    if(frame->deadline_valid && (int32_t)(simulation.now-frame->deadline_ms)>=0) return true;
    if(frame->length>OSG_GW_FRAME_CAPACITY) return false;
    uint8_t packet[BUS_PACKET_SIZE];packet_init(packet,BUS_FRAME);
    packet[6]=(uint8_t)((frame->extended?1:0)|(frame->fd?2:0)|(frame->brs?4:0));
    packet[7]=frame->length;put32(packet+8,frame->id);memcpy(packet+28,frame->data,frame->length);
    return broadcast(packet,-1);
}
static int peer_index(const struct sockaddr_in *address) {
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) if(local.peers[i].used && address->sin_port==local.peers[i].address.sin_port && address->sin_addr.s_addr==local.peers[i].address.sin_addr.s_addr)return (int)i;
    return -1;
}
static void gateway_packet(const struct sockaddr_in *address,const uint8_t *packet) {
    if(address->sin_addr.s_addr!=htonl(INADDR_LOOPBACK))return;
    int index=peer_index(address);
    if(packet[5]==BUS_HELLO) {
        osg_gateway_identity_t identity;memcpy(identity.bytes,packet+12,sizeof identity.bytes);
        uint32_t pid=get32(packet+28);
        if(!osg_gateway_identity_valid(&identity) || pid==0 || pid>INT_MAX || kill((pid_t)pid,0)<0)return;
        uint8_t reply[BUS_PACKET_SIZE];packet_init(reply,BUS_ACK);put32(reply+8,simulation.gateway.epoch);
        int empty=-1;bool duplicate=false;
        for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) {
            peer_t *peer=&local.peers[i];
            if(peer->used && ((uint32_t)(simulation.now-peer->seen)>2000U || (kill(peer->pid,0)<0 && errno==ESRCH)))peer->used=false;
            if(peer->used && (int)i!=index && osg_gateway_identity_equal(&identity,&peer->identity))duplicate=true;
            if(!peer->used && empty<0) empty=(int)i;
        }
        if(index<0)index=empty;
        if(duplicate) reply[28]=1;
        else if(index<0) {
            reply[28]=2; simulation.gateway.rejected_announcements++;
            simulation.gateway.rejected_identity=identity; simulation.gateway.admission_error="capacity_exceeded";
        }
        else {
            local.peers[index]=(peer_t){.used=true,.address=*address,.identity=identity,.seen=simulation.now,.pid=(pid_t)pid};
        }
        (void)send_packet(address,reply);return;
    }
    if(packet[5]!=BUS_FRAME || index<0 || packet[7]>OSG_GW_FRAME_CAPACITY || packet[6]&~7U) return;
    local.peers[index].seen=simulation.now;
    osg_gateway_frame_t frame={.id=get32(packet+8),.length=packet[7],.extended=(packet[6]&1)!=0,
        .fd=(packet[6]&2)!=0,.brs=(packet[6]&4)!=0};memcpy(frame.data,packet+28,frame.length);
    broadcast(packet,index);receive_gateway(&frame);
}
static void receive_bus(void) {
    for(unsigned i=0;i<512;i++) {
        uint8_t packet[BUS_PACKET_SIZE+1];struct sockaddr_in address;memset(&address,0,sizeof address);
        socklen_t address_length=sizeof address;
        ssize_t n=recvfrom(local.bus,packet,sizeof packet,MSG_DONTWAIT,(struct sockaddr *)&address,&address_length);
        if(n<0) {if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) die("Local CAN receive failed");break;}
        if(!packet_valid(packet,(size_t)n)) continue;
        gateway_packet(&address,packet);
    }
}
static bool cdc_drained(void) {return local.output_offset==local.output_length;}
static void detach_cdc(void) {
    if(local.cdc>=0) {close(local.cdc);local.cdc=-1;}
    if(!simulation.layout.detached && !simulation.gateway.publication.expected_reconnect) osg_gateway_publication_transport_lost(&simulation.gateway,simulation.now);
    local.output_offset=local.output_length=0;osg_line_init(&local.cdc_line,local.cdc_input,sizeof local.cdc_input);
}
static void publish_gateway(bool publish) {
    if(!publish) {if(unlink(local.advertisement)<0 && errno!=ENOENT) die("Could not remove gateway advertisement");return;}
    char temporary[200];int n=snprintf(temporary,sizeof temporary,"%s.%ld.tmp",local.advertisement,(long)getpid());
    assert(n>0 && (size_t)n<sizeof temporary);
    int fd=open(temporary,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0) die("Could not create gateway advertisement");
    FILE *file=fdopen(fd,"w");if(!file) die("Could not write gateway advertisement");
    if(fprintf(file,"{\"protocol\":3,\"role\":\"gateway\",\"serialNumber\":\"%s\",\"manufacturer\":\"OpenSimGear\",\"friendlyName\":\"OpenSimGear CAN gateway\",\"endpoint\":\"%s.sock\",\"tcpPort\":%u,\"pid\":%ld}\n",gateway_uid,gateway_uid,local.cdc_port,(long)getpid())<0 || fclose(file)!=0) die("Could not finish gateway advertisement");
    if(rename(temporary,local.advertisement)<0) die("Could not publish gateway advertisement");
}
static void cleanup(void) {
    if(*local.advertisement)(void)unlink(local.advertisement);
    if(*local.bus_manifest)(void)unlink(local.bus_manifest);
    if(local.bus>=0)close(local.bus);
    if(local.listener>=0)close(local.listener);
    if(local.cdc>=0)close(local.cdc);
    if(local.lock>=0)close(local.lock);
    if(local.state_lock>=0)close(local.state_lock);
}
static void standalone_cycle(bool wait) {
    if(wait) {struct pollfd p={.fd=local.bus,.events=POLLIN};(void)poll(&p,1,1);}
    simulation.now=wall_ms();receive_bus();tick();
}
static void cdc_reply(const char *line) {
    char *bytes=NULL;size_t length=0;FILE *memory=open_memstream(&bytes,&length);if(!memory)die("Could not allocate gateway reply");
    response_output=memory;
    if(!strncmp(line,"sim ",4))fprintf(memory,"{\"id\":0,\"ok\":false,\"error\":\"invalid_request\",\"state\":null}\n");
    else command(line);
    if(fclose(memory)!=0)die("Could not finish gateway reply");
    response_output=stdout;
    if(length>sizeof local.output) {free(bytes);detach_cdc();return;}
    memcpy(local.output,bytes,length);local.output_length=length;local.output_offset=0;free(bytes);
}
static void serve_cdc(void) {
    osg_gateway_publication_reconnect(&simulation.gateway,simulation.now,local.cdc>=0);
    if(local.cdc<0 && !simulation.layout.detached) {
        int fd=accept(local.listener,NULL,NULL);
        if(fd>=0) {nonblocking(fd);local.cdc=fd;osg_line_init(&local.cdc_line,local.cdc_input,sizeof local.cdc_input);}
        else if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) die("Could not accept local CDC client");
    }
    if(local.cdc<0)return;
    if(!cdc_drained()) {
        ssize_t n=send(local.cdc,local.output+local.output_offset,local.output_length-local.output_offset,MSG_DONTWAIT);
        if(n>0)local.output_offset+=(size_t)n;
        else if(n==0 || (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR))detach_cdc();
        return;
    }
    for(unsigned i=0;i<1024;i++) {
        char byte;ssize_t n=recv(local.cdc,&byte,1,MSG_DONTWAIT);
        if(n==0) {detach_cdc();return;}
        if(n<0) {if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)detach_cdc();return;}
        osg_line_result_t result=osg_line_feed(&local.cdc_line,byte);
        if(result==OSG_LINE_READY) {cdc_reply(local.cdc_input);return;}
        if(result==OSG_LINE_TOO_LONG) {cdc_reply("");return;}
    }
}
static void serve_stdin(void) {
    if(!local.stdin_open)return;
    for(unsigned i=0;i<1024;i++) {
        struct pollfd input={.fd=STDIN_FILENO,.events=POLLIN};
        if(poll(&input,1,0)<=0)return;
        char byte;ssize_t n=read(STDIN_FILENO,&byte,1);
        if(n==0) {local.stdin_open=false;return;}
        if(n<0) {if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)local.stdin_open=false;return;}
        osg_line_result_t result=osg_line_feed(&local.input_line,byte);
        if(result==OSG_LINE_READY) {
            if(!strncmp(local.input,"sim ",4))admin(local.input);
            else fprintf(stderr,"Use sim <id> <operation> for terminal administration.\n");
            return;
        }
        if(result==OSG_LINE_TOO_LONG) {inspect(0,false,"invalid_request");return;}
    }
}
static bool namespace_valid(const char *name) {
    if(!name || !*name || strlen(name)>31) return false;
    for(const char *p=name;*p;p++) {
        char c=*p;
        if(!((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') ||
             c=='.' || c=='_' || c=='-')) return false;
    }
    return true;
}
static void init_local(void) {
    const char *namespace=getenv("OSG_SIMULATION_NAMESPACE");
    if(namespace && !namespace_valid(namespace)) {errno=EINVAL;die("Invalid simulation namespace");}
    char parent[80];snprintf(parent,sizeof parent,"/tmp/opensimgear-%lu",(unsigned long)geteuid());owned_directory(parent);
    int root_length=snprintf(local.root,sizeof local.root,"%s/gateways%s%s",parent,namespace ? "-":"",namespace ? namespace:"");
    if(root_length<0 || (size_t)root_length>=sizeof local.root) {errno=ENAMETOOLONG;die("Simulation namespace path too long");}
    owned_directory(local.root);
    snprintf(local.bus_manifest,sizeof local.bus_manifest,"%s/%s.can.json",local.root,gateway_uid);
    {
        snprintf(local.advertisement,sizeof local.advertisement,"%s/%s.json",local.root,gateway_uid);
        char lock_path[160];snprintf(lock_path,sizeof lock_path,"%s/%s.lock",local.root,gateway_uid);
        local.lock=open(lock_path,O_RDWR|O_CREAT|O_NOFOLLOW,0600);if(local.lock<0)die("Could not open gateway identity lock");
        struct stat info;
        if(fstat(local.lock,&info)<0 || !S_ISREG(info.st_mode) || info.st_uid!=geteuid() || (info.st_mode&0777)!=0600) {errno=EPERM;die("Gateway identity lock must be private and owned by current user");}
        if(flock(local.lock,LOCK_EX|LOCK_NB)<0)die("Gateway identity is already running");
        if(fcntl(local.lock,F_SETFD,FD_CLOEXEC)<0)die("Could not configure identity lock");
        /* A loopback stream preserves CDC framing without Darwin AF_UNIX
         * reciprocal socket locking during unread-client disconnects. */
        local.listener=socket(AF_INET,SOCK_STREAM,0);if(local.listener<0)die("Could not create local CDC stream endpoint");
        struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
        if(bind(local.listener,(struct sockaddr *)&address,sizeof address)<0)die("Could not bind local CDC stream endpoint");
        socklen_t address_length=sizeof address;
        if(getsockname(local.listener,(struct sockaddr *)&address,&address_length)<0)die("Could not inspect local CDC stream endpoint");
        local.cdc_port=ntohs(address.sin_port);nonblocking(local.listener);
        if(listen(local.listener,4)<0)die("Could not listen on gateway CDC endpoint");
    }
    /* Loopback UDP carries CAN frames without AF_UNIX reciprocal datagram
     * locking; payload and routing still belong entirely to production C. */
    local.bus=socket(AF_INET,SOCK_DGRAM,0);if(local.bus<0)die("Could not create local CAN datagram endpoint");
    struct sockaddr_in broker=(struct sockaddr_in){.sin_family=AF_INET,.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    if(bind(local.bus,(struct sockaddr *)&broker,sizeof broker)<0)die("Could not bind local CAN datagram endpoint");
    nonblocking(local.bus);
    {
        socklen_t length=sizeof broker;if(getsockname(local.bus,(struct sockaddr *)&broker,&length)<0)die("Could not inspect local CAN endpoint");
        char temporary[200];snprintf(temporary,sizeof temporary,"%s.%ld.tmp",local.bus_manifest,(long)getpid());
        int fd=open(temporary,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)die("Could not create local CAN advertisement");
        FILE *file=fdopen(fd,"w");if(!file)die("Could not write local CAN advertisement");
        if(fprintf(file,"{\"protocol\":1,\"pid\":%ld,\"port\":%u}\n",(long)getpid(),ntohs(broker.sin_port))<0 || fclose(file)!=0)die("Could not finish local CAN advertisement");
        if(rename(temporary,local.bus_manifest)<0)die("Could not publish local CAN advertisement");
    }
    if(clock_gettime(CLOCK_MONOTONIC,&local.started)<0)die("Monotonic clock unavailable");
    osg_line_init(&local.input_line,local.input,sizeof local.input);osg_line_init(&local.cdc_line,local.cdc_input,sizeof local.cdc_input);
    atexit(cleanup);
    struct sigaction action={0};action.sa_handler=stop_signal;sigemptyset(&action.sa_mask);
    if(sigaction(SIGINT,&action,NULL)<0 || sigaction(SIGTERM,&action,NULL)<0)die("Could not install shutdown handler");
    signal(SIGPIPE,SIG_IGN);
}
static bool uid_valid(const char *uid) {
    if(strlen(uid)!=24)return false;
    bool nonzero=false,nonff=false;
    for(unsigned i=0;i<24;i++) {char c=uid[i];if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;nonzero|=c!='0';nonff|=c!='f';}
    return nonzero&&nonff;
}
static void usage(const char *program) {
    fprintf(stderr,"Usage: %s [--identity UID24] [--state-dir PATH]\n",program);
    exit(2);
}
int main(int argc,char **argv) {
    char state_path[1024]; response_output=stdout;
    for(int i=1;i<argc;i++) {
        if(i+1>=argc)usage(argv[0]);
        if(!strcmp(argv[i],"--state-dir"))state_directory=argv[++i];
        else if(!strcmp(argv[i],"--identity")) {
            if(!uid_valid(argv[i+1]))usage(argv[0]);
            strcpy(gateway_uid,argv[++i]);
        } else usage(argv[0]);
    }
    if(!state_directory) {
        char *binary=realpath(argv[0],NULL);if(!binary)die("Could not resolve gateway executable");
        char *slash=strrchr(binary,'/');assert(slash);*slash=0;
        int n=snprintf(state_path,sizeof state_path,"%s/state/gateway-%s",binary,gateway_uid);
        free(binary);if(n<0 || (size_t)n>=sizeof state_path)usage(argv[0]);state_directory=state_path;
    }
    const char *namespace=getenv("OSG_SIMULATION_NAMESPACE");
    if(namespace && !namespace_valid(namespace)) {errno=EINVAL;die("Invalid simulation namespace");}
    make_state_directory(state_directory);lock_state();init_local();
    flash_init(&simulation.registry_flash,"registry");
    char can_uid[33];snprintf(can_uid,sizeof can_uid,"%s47575459",gateway_uid);
    assert(osg_gateway_identity_parse(can_uid,&simulation.gateway_identity));
    boot_gateway();publish_gateway(true);
    fprintf(stderr,"OpenSimGear gateway %s ready at 127.0.0.1:%u\n",gateway_uid,local.cdc_port);
    while(!stopped) {standalone_cycle(true);serve_cdc();serve_stdin();}
    osg_cyphal_destroy(&simulation.gateway_can);return 0;
}
