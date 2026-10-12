/* Stable ctypes ABI for the SocketCAN peer. All wire processing uses the same
 * generated DSDL and libcanard transport as the two firmware applications. */
#include "opensimgear/cyphal.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t kind,boot,lease,epoch,sequence;
    uint8_t identity[16],node,source,direct,buttons,axes,hats;
    uint16_t revision;
    uint8_t usages[8];
    uint16_t neutrals[8];
    uint8_t report[34];
} peer_message_t;
typedef struct {
    uint32_t id;
    uint8_t length,extended,fd,brs,data[64];
    uint32_t deadline_ms;
    uint8_t deadline_valid;
} peer_frame_t;
enum { PEER_QUEUE_CAPACITY=64 };
typedef struct {
    osg_cyphal_t transport;
    peer_frame_t frames[PEER_QUEUE_CAPACITY];
    unsigned head,count;
} peer_t;

static bool enqueue(void *context,const osg_gateway_frame_t *frame) {
    peer_t *peer=context;
    if(peer->count==PEER_QUEUE_CAPACITY) return false;
    peer_frame_t *out=&peer->frames[(peer->head+peer->count)%PEER_QUEUE_CAPACITY];
    *out=(peer_frame_t){.id=frame->id,.length=frame->length,
        .extended=frame->extended,.fd=frame->fd,.brs=frame->brs,
        .deadline_ms=frame->deadline_ms,.deadline_valid=frame->deadline_valid};
    memcpy(out->data,frame->data,frame->length); peer->count++; return true;
}
static osg_gateway_message_t to_message(const peer_message_t *source) {
    osg_gateway_message_t out={.type=(osg_gateway_message_type_t)source->kind,
        .node=source->node,.source_node=source->source,.direct_usb=source->direct!=0,
        .boot_session=source->boot,.lease=source->lease,.gateway_epoch=source->epoch,
        .sequence=source->sequence,.profile={.button_count=source->buttons,
            .axis_count=source->axes,.hat_count=source->hats,.revision=source->revision}};
    memcpy(out.identity.bytes,source->identity,sizeof out.identity.bytes);
    memcpy(out.profile.axis_usage,source->usages,sizeof out.profile.axis_usage);
    memcpy(out.profile.axis_neutral,source->neutrals,sizeof out.profile.axis_neutral);
    memcpy(out.report,source->report,sizeof out.report); return out;
}
static void from_message(peer_message_t *out,const osg_gateway_message_t *source) {
    *out=(peer_message_t){.kind=source->type,.boot=source->boot_session,.lease=source->lease,
        .epoch=source->gateway_epoch,.sequence=source->sequence,.node=source->node,
        .source=source->source_node,.direct=source->direct_usb,.buttons=source->profile.button_count,
        .axes=source->profile.axis_count,.hats=source->profile.hat_count,.revision=source->profile.revision};
    memcpy(out->identity,source->identity.bytes,sizeof out->identity);
    memcpy(out->usages,source->profile.axis_usage,sizeof out->usages);
    memcpy(out->neutrals,source->profile.axis_neutral,sizeof out->neutrals);
    memcpy(out->report,source->report,sizeof out->report);
}
size_t peer_message_size(void) { return sizeof(peer_message_t); }
size_t peer_frame_size(void) { return sizeof(peer_frame_t); }
void *peer_new(const uint8_t identity[16],uint32_t boot,const char *name) {
    if(!identity) return NULL;
    peer_t *peer=calloc(1,sizeof *peer);
    if(!peer) return NULL;
    osg_gateway_identity_t unique_id; memcpy(unique_id.bytes,identity,sizeof unique_id.bytes);
    if(!osg_cyphal_init(&peer->transport,&unique_id,boot,name,enqueue,peer)) { free(peer); return NULL; }
    return peer;
}
void peer_delete(void *context) {
    peer_t *peer=context;
    if(peer) { osg_cyphal_destroy(&peer->transport); free(peer); }
}
unsigned peer_node(void *context) {
    return context ? osg_cyphal_node_id(&((peer_t *)context)->transport) : 255;
}
void peer_tick(void *context,uint32_t now) {
    if(context) osg_cyphal_tick(&((peer_t *)context)->transport,now);
}
void peer_reset(void *context) {
    peer_t *peer=context;
    if(peer) { peer->head=peer->count=0; osg_cyphal_reset(&peer->transport); }
}
int peer_send(void *context,const peer_message_t *message,uint32_t now) {
    if(!context || !message) return 0;
    osg_gateway_message_t outgoing=to_message(message);
    return osg_cyphal_send(&((peer_t *)context)->transport,&outgoing,now);
}
int peer_receive(void *context,const peer_frame_t *frame,uint32_t now,peer_message_t *out) {
    if(!context || !frame || !out || frame->length>64) return 0;
    osg_gateway_frame_t incoming={.id=frame->id,.length=frame->length,
        .extended=frame->extended!=0,.fd=frame->fd!=0,.brs=frame->brs!=0};
    memcpy(incoming.data,frame->data,frame->length);
    osg_gateway_message_t message;
    if(!osg_cyphal_receive(&((peer_t *)context)->transport,&incoming,now,&message)) return 0;
    from_message(out,&message); return 1;
}
int peer_pop(void *context,peer_frame_t *out) {
    peer_t *peer=context;
    if(!peer || !out || !peer->count) return 0;
    *out=peer->frames[peer->head]; peer->head=(peer->head+1)%PEER_QUEUE_CAPACITY; peer->count--; return 1;
}
