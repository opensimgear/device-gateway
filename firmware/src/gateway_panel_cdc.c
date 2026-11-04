#include "opensimgear/gateway_panel_cdc.h"
#include <stdio.h>

static const char reset_line[]="{\"panelStream\":\"reset\"}\r\n";

static void reset(osg_gateway_panel_cdc_t *stream) {
    stream->head=stream->count=0;
    stream->reset_pending=stream->enabled;
}

void osg_gateway_panel_cdc_enable(osg_gateway_panel_cdc_t *stream, bool enabled) {
    stream->enabled=enabled;
    reset(stream);
}

void osg_gateway_panel_cdc_notify(void *context, unsigned slot, const osg_gateway_message_t *packet) {
    osg_gateway_panel_cdc_t *stream=context;
    if(!stream->enabled) return;
    if(stream->count==OSG_GW_PANEL_CDC_DEPTH) reset(stream);
    unsigned index=(stream->head+stream->count)%OSG_GW_PANEL_CDC_DEPTH;
    char *line=stream->entries[index].line;
    char identity[33];
    osg_gateway_identity_format(&stream->receiver->slots[slot].identity,identity);
    int size;
    if(!packet) {
        size=snprintf(line,OSG_GW_PANEL_CDC_LINE,
            "{\"panelStream\":\"loss\",\"moduleId\":\"%s\"}\r\n",identity);
    } else {
        uint8_t payload[OSG_GW_FRAME_CAPACITY];
        size_t length=osg_gateway_payload_encode(packet,payload,sizeof payload);
        if(!length) { reset(stream); return; }
        char hex[2*OSG_GW_FRAME_CAPACITY+1];
        static const char digits[]="0123456789abcdef";
        for(size_t i=0;i<length;i++) {
            hex[2*i]=digits[payload[i]>>4];
            hex[2*i+1]=digits[payload[i]&15U];
        }
        hex[2*length]='\0';
        size=snprintf(line,OSG_GW_PANEL_CDC_LINE,
            "{\"panelStream\":\"event\",\"moduleId\":\"%s\",\"sourceNode\":%u,\"payload\":\"%s\"}\r\n",
            identity,packet->source_node,hex);
    }
    if(size<=0 || size>=OSG_GW_PANEL_CDC_LINE) { reset(stream); return; }
    stream->entries[index].size=(size_t)size;
    stream->entries[index].queued_at=stream->now;
    stream->count++;
}

const char *osg_gateway_panel_cdc_peek(osg_gateway_panel_cdc_t *stream, size_t *size) {
    if(stream->count && (uint32_t)(stream->now-stream->entries[stream->head].queued_at)>=OSG_GW_PANEL_FRESHNESS_MS)
        reset(stream);
    if(stream->reset_pending) {
        *size=sizeof reset_line-1;
        return reset_line;
    }
    *size=0;
    if(!stream->count) return NULL;
    *size=stream->entries[stream->head].size;
    return stream->entries[stream->head].line;
}

void osg_gateway_panel_cdc_pop(osg_gateway_panel_cdc_t *stream) {
    if(stream->reset_pending) stream->reset_pending=false;
    else if(stream->count) {
        stream->head=(stream->head+1)%OSG_GW_PANEL_CDC_DEPTH;
        stream->count--;
    }
}
