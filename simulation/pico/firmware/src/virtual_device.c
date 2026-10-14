#include "virtual_device.h"
#include <stdio.h>
#include <string.h>

const char *const osg_debug_presets[]={"shifter","pedals","handbrake","buttons","master-caution","radio","all-inputs","output-only"};
const unsigned osg_debug_preset_count=sizeof osg_debug_presets/sizeof osg_debug_presets[0];
static const char *const names[]={"Sequential shifter","Pedals","Handbrake","Button panel","Master Caution","Radio panel","All inputs","Output only rev lights"};
static bool display_write(void *context,const char *text) {
    osg_virtual_display_t *s=context;
    if(strlen(text)>=sizeof s->text) return false;
    strcpy(s->text,text); return true;
}
static bool display_read(void *context,char *out,size_t capacity) {
    osg_virtual_display_t *s=context;
    if(strlen(s->text)>=capacity) return false;
    strcpy(out,s->text);return true;
}
static void contact(osg_virtual_device_t *d,unsigned input,bool closed) {
    if(input<24) {
        if(closed) d->sample.digital_closed|=UINT32_C(1)<<input;
        else d->sample.digital_closed&=~(UINT32_C(1)<<input);
    } else {
        unsigned bank=(input-24)/16,pin=(input-24)%16;
        if(closed) d->sample.expansion_closed[bank]|=(uint16_t)(1U<<pin);
        else d->sample.expansion_closed[bank]&=(uint16_t)~(1U<<pin);
    }
}
static void transform(void *context,const osg_profile_state_t *state,uint8_t *report) {
    osg_runtime_transform_report(context,state,report);
}
static void neutral_axes(osg_virtual_device_t *d) {
    for(unsigned i=0;i<d->runtime.active.axis_count;i++) {
        const osg_profile_axis_t *axis=&d->runtime.active.axes[i];
        uint32_t value=d->runtime.active.axis_options[i].inverted?65535U-axis->neutral:axis->neutral;
        d->sample.axes[axis->channel]=(uint16_t)(axis->minimum+((uint32_t)(axis->maximum-axis->minimum)*value+32767U)/65535U);
    }
}
static bool defaults(unsigned preset,osg_runtime_definition_t *p) {
    if(preset>=osg_debug_preset_count) return false;
    memset(p,0,sizeof *p);strcpy(p->name,names[preset]);
    switch(preset) {
    case 0: p->button_count=3;break;
    case 1: p->axis_count=3;break;
    case 2: p->axis_count=1;break;
    case 3: p->button_count=16;break;
    case 4: p->button_count=1;break;
    case 5:
        p->button_count=3;p->axis_count=1;p->encoder_count=2;
        p->buttons[0].maintained=p->buttons[1].maintained=true;
        p->encoders[0]=(osg_profile_encoder_t){3,4};p->encoders[1]=(osg_profile_encoder_t){5,6};break;
    case 6:
        p->button_count=121;p->axis_count=8;p->hat_count=4;p->encoder_count=2;p->selector_count=1;p->expander_mask=255;
        for(unsigned i=0;i<4;i++) for(unsigned j=0;j<4;j++) p->hats[i].inputs[j]=(uint8_t)(i*4+j);
        p->encoders[0]=(osg_profile_encoder_t){16,17};p->encoders[1]=(osg_profile_encoder_t){18,19};
        p->selectors[0]=(osg_profile_selector_t){20,21};break;
    case 7:break;
    default:return false;
    }
    for(unsigned i=0;i<p->button_count;i++) p->buttons[i].input=(uint8_t)(preset==6 ? i+22 : i);
    if(preset==6) p->buttons[1].maintained=true;
    for(unsigned i=0;i<p->axis_count;i++) p->axes[i]=(osg_profile_axis_t){(uint8_t)i,(uint8_t)(48+i),preset==5?100:0,preset==5?3900:4095,0};
    return true;
}
void osg_virtual_rearm(osg_virtual_device_t *d,uint32_t now,bool reset_events) {
    (void)osg_profile_init(&d->acquisition,&d->runtime.profile,&d->sample,now);
    osg_profile_neutral(&d->acquisition,d->report);
    memset(d->turn_count,0,sizeof d->turn_count);memset(d->turn_phase,0,sizeof d->turn_phase);memset(d->turn_head,0,sizeof d->turn_head);
    if(reset_events) {
        uint32_t stream=d->events.stream+1;
        if(!stream) stream=1;
        (void)osg_events_init(&d->events,&d->runtime.profile,&d->sample,now,stream);
        osg_events_set_transform(&d->events,transform,&d->runtime);
    }
}
bool osg_virtual_init(osg_virtual_device_t *d,unsigned slot,unsigned preset,uint32_t boot,uint32_t now,const osg_journal_io_t *io) {
    osg_runtime_definition_t definition;
    if(!d || slot>=OSG_DEBUG_DEVICE_COUNT || !boot || !defaults(preset,&definition)) return false;
    memset(d,0,sizeof *d);d->online=true;d->preset=(uint8_t)preset;d->boot=boot;
    /* Fixed fixture identities are scoped by the physical gateway in Cockpit. */
    d->identity.bytes[0]=0x50;d->identity.bytes[1]=0x49;d->identity.bytes[2]=0x43;d->identity.bytes[3]=0x4f;d->identity.bytes[15]=(uint8_t)(slot+1);
    if(!osg_runtime_init_definition(&d->runtime,&definition,io)) return false;
    d->sample.expansion_available=255;d->sample.axes_fresh=255;
    neutral_axes(d);
    static const char *const ids[]={"display.frequency.set_text","display.preset.set_text","display.text.set_text"};
    static const char *const labels[]={"Frequency","Preset","Text display"};
    static const char *const endpoints[]={"display.frequency","display.preset","display.text"};
    static const uint8_t columns[]={7,2,16},bytes[]={7,2,63};
    for(unsigned i=0;i<3;i++) d->display_ports[i]=(osg_device_display_port_t){
        .id=ids[i],.label=labels[i],.endpoint=endpoints[i],.display={columns[i],1,i==2?OSG_ACTION_DISPLAY_UTF8:OSG_ACTION_DISPLAY_ASCII},
        .max_bytes=bytes[i],.write=display_write,.read=display_read,.context=&d->displays[i]};
    if(!osg_device_actions_init_outputs(&d->actions,preset!=4 && preset!=5,d->display_ports,preset==5 || preset==6?3:0)) return false;
    osg_virtual_rearm(d,now,true);d->occupied=true;return true;
}
static osg_gateway_profile_t profile(const osg_virtual_device_t *d) {
    osg_gateway_profile_t p={.button_count=d->runtime.layout.buttons,.axis_count=d->runtime.layout.axes,
        .hat_count=d->runtime.layout.hats,.revision=1};
    for(unsigned i=0;i<p.axis_count;i++) { p.axis_usage[i]=d->runtime.profile.axes[i].usage;p.axis_neutral[i]=d->runtime.profile.axes[i].neutral; }
    return p;
}
void osg_virtual_tick(osg_virtual_device_t *d,osg_gateway_t *g,uint32_t now) {
    if(!d->occupied) return;
    osg_actions_tick(&d->actions.registry,now);
    if(!d->online) return;
    osg_runtime_tick(&d->runtime,now);
    if((uint32_t)(now-d->turn_at)>=2) {
        static const uint8_t sequence[2][4]={{2,0,1,3},{1,0,2,3}};
        for(unsigned i=0;i<d->runtime.profile.encoder_count;i++) if(d->turn_count[i]) {
            int direction=d->turns[i][d->turn_head[i]];
            uint8_t ab=sequence[direction<0][d->turn_phase[i]];
            contact(d,d->runtime.profile.encoders[i].a,(ab&1)==0);contact(d,d->runtime.profile.encoders[i].b,(ab&2)==0);
            if(++d->turn_phase[i]==4) { d->turn_phase[i]=0;d->turn_head[i]=(uint8_t)((d->turn_head[i]+1)%64);d->turn_count[i]--; }
        }
        d->turn_at=now;
    }
    osg_events_observe(&d->events,&d->sample,now);
    int index=osg_gateway_find_module(g,&d->identity);
    bool announce=index<0 || !g->modules[index].online || g->modules[index].boot_session!=d->boot ||
        g->modules[index].configuration_generation!=d->runtime.generation || (uint32_t)(now-d->last_announce)>=500;
    if(announce) {
        osg_gateway_message_t m={.type=OSG_GW_ANNOUNCE,.source_node=(uint8_t)(d->identity.bytes[15]+64),.identity=d->identity,
            .boot_session=d->boot,.direct_usb=d->direct,.configuration_generation=d->runtime.generation,.profile=profile(d)},reply;
        if(osg_gateway_receive(g,&m,now,&reply)==OSG_GW_RX_ASSIGNMENT) {
            if(d->lease!=reply.lease) { osg_actions_invalidate_scope(&d->actions.registry,OSG_ACTION_SCOPE_CAN,now);d->lease=reply.lease;d->node=reply.node;d->sequence=0;osg_virtual_rearm(d,now,false); }
        }
        d->last_announce=now;
    }
    index=osg_gateway_find_module(g,&d->identity);
    if(index>=0 && g->modules[index].lease!=d->lease) {
        osg_actions_invalidate_scope(&d->actions.registry,OSG_ACTION_SCOPE_CAN,now);
        d->lease=g->modules[index].lease;d->node=g->modules[index].node;d->sequence=0;osg_virtual_rearm(d,now,false);
    }
    osg_profile_update(&d->acquisition,&d->sample,now,d->lease!=0 && !d->direct,d->report);
    osg_runtime_transform_report(&d->runtime,&d->acquisition,d->report);
    if(d->lease && (uint32_t)(now-d->last_input)>=10) {
        osg_gateway_message_t m={.type=d->direct || !d->runtime.layout.length?OSG_GW_HEARTBEAT:OSG_GW_INPUT,.node=d->node,
            .source_node=(uint8_t)(d->identity.bytes[15]+64),.identity=d->identity,.boot_session=d->boot,.lease=d->lease,
            .gateway_epoch=g->epoch,.sequence=++d->sequence,.direct_usb=d->direct};
        memcpy(m.report,d->report,d->runtime.layout.length);(void)osg_gateway_receive(g,&m,now,NULL);d->last_input=now;
    }
}
void osg_virtual_ack(osg_virtual_device_t *d,const osg_gateway_message_t *ack,uint32_t now) {
    if(d->occupied && d->online && !d->direct && d->boot==ack->boot_session && d->lease==ack->lease &&
       osg_gateway_identity_equal(&d->identity,&ack->identity)) osg_profile_reported(&d->acquisition,ack->report,now);
}
const char *osg_virtual_command(osg_virtual_device_t *d,const char *command,uint32_t now,char *out,size_t capacity) {
    if(!strncmp(command,"action ",7)) return osg_actions_command_at(&d->actions.registry,command,now,OSG_ACTION_SCOPE_CAN,out,capacity)?NULL:"invalid_request";
    if(!strncmp(command,"events ",7)) return osg_events_command(&d->events,command,out,capacity)?NULL:"invalid_request";
    bool apply=false;const char *error=osg_runtime_command(&d->runtime,command,now,&apply);
    if(apply && !error) {
        if(!osg_runtime_finish_apply(&d->runtime)) error="apply_failed";
        else { osg_actions_invalidate_scope(&d->actions.registry,OSG_ACTION_SCOPE_CAN,now);d->lease=0;d->node=0;d->last_announce=now-500;osg_virtual_rearm(d,now,true); }
    }
    if(!osg_runtime_snapshot(&d->runtime,!strcmp(command,"get_active"),out,capacity)) return "response_overflow";
    return error;
}
const char *osg_virtual_debug(osg_virtual_device_t *d,const char *op,int a,int b,uint32_t now) {
    if(!strcmp(op,"online")) {
        if(a<0 || a>1) return "invalid_request";
        osg_actions_invalidate_scope(&d->actions.registry,OSG_ACTION_SCOPE_CAN,now);
        d->online=a!=0;d->lease=0;d->node=0;d->last_announce=now-500;osg_virtual_rearm(d,now,true);return NULL;
    }
    if(!strcmp(op,"reboot")) {
        if(d->boot==UINT32_MAX) return "boot_exhausted";
        osg_journal_io_t settings=d->runtime.settings;unsigned slot=d->identity.bytes[15]-1,preset=d->preset;uint32_t boot=d->boot+1;
        return osg_virtual_init(d,slot,preset,boot,now,settings.read?&settings:NULL)?NULL:"flash_load_failed";
    }
    if(!strcmp(op,"neutral")) {
        d->sample.digital_closed=0;memset(d->sample.expansion_closed,0,sizeof d->sample.expansion_closed);
        neutral_axes(d);
        /* Cancel an incomplete quadrature cycle without turning its final
         * return-to-open edge into a completed DCS detent. Contact acquisition
         * still observes ordinary releases for presses already forwarded. */
        for(unsigned i=0;i<d->runtime.profile.encoder_count;i++) osg_encoder_rearm(&d->events.decoders[i],3);
        osg_virtual_rearm(d,now,false);return NULL;
    }
    if(!d->online) return "module_offline";
    if(!strcmp(op,"contact")) {
        if(a<0 || a>=152 || b<0 || b>1) return "invalid_request";
        contact(d,(unsigned)a,b!=0);
    } else if(!strcmp(op,"axis")) {
        if(a<0 || a>=8 || b<0 || b>4095) return "invalid_request";
        d->sample.axes[a]=(uint16_t)b;
    } else if(!strcmp(op,"turn")) {
        if(a<0 || a>=d->runtime.profile.encoder_count || !b || b<-32 || b>32) return "invalid_request";
        unsigned count=(unsigned)(b<0?-b:b);
        if(d->turn_count[a]+count>64) return "input_queue_full";
        for(unsigned i=0;i<count;i++) d->turns[a][(d->turn_head[a]+d->turn_count[a]++)%64]=(int8_t)(b<0?-1:1);
    } else if(!strcmp(op,"hat")) {
        if(a<0 || a>=d->runtime.profile.hat_count || b<-1 || b>7) return "invalid_request";
        static const uint8_t masks[]={1,3,2,6,4,12,8,9};uint8_t mask=b<0?0:masks[b];
        for(unsigned i=0;i<4;i++) contact(d,d->runtime.profile.hats[a].inputs[i],(mask&(1U<<i))!=0);
    } else if(!strcmp(op,"selector")) {
        if(a<0 || a>=d->runtime.profile.selector_count || b<0 || b>2) return "invalid_request";
        contact(d,d->runtime.profile.selectors[a].low,b==0);contact(d,d->runtime.profile.selectors[a].high,b==2);
    } else if(!strcmp(op,"direct")) {
        if(a<0 || a>1) return "invalid_request";
        d->direct=a!=0;d->last_announce=now-500;osg_virtual_rearm(d,now,true);
    } else if(!strcmp(op,"expansion")) {
        if(a<0 || a>=8 || b<0 || b>1) return "invalid_request";
        if(b) d->sample.expansion_available|=(uint8_t)(1U<<a);else d->sample.expansion_available&=(uint8_t)~(1U<<a);
    } else return "invalid_request";
    return NULL;
}
