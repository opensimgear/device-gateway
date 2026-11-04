#include "opensimgear/gateway_panel.h"
#include <assert.h>

typedef struct {
    const osg_gateway_panel_t *receiver;
    unsigned count;
    char kinds[24];
} notices_t;

static void notify(void *context, unsigned slot, const osg_gateway_message_t *packet) {
    notices_t *log=context;
    assert(slot==3 && log->count<sizeof log->kinds);
    const osg_gateway_panel_slot_t *state=&log->receiver->slots[slot];
    if(!packet) {
        assert(!state->valid && state->lease);
        log->kinds[log->count++]='-';
        return;
    }
    assert(packet->sequence==state->sequence);
    log->kinds[log->count++]="LSE?"[packet->panel_kind];
}

static osg_gateway_message_t packet(osg_panel_kind_t kind, uint32_t sequence, uint32_t sample) {
    return (osg_gateway_message_t){.type=OSG_GW_PANEL_EVENT,.node=7,.source_node=42,
        .boot_session=4,.lease=5,.gateway_epoch=6,.configuration_generation=8,
        .sequence=sequence,.panel_kind=kind,.panel_sampled_at_ms=sample};
}

int main(void) {
    osg_gateway_t gateway={.epoch=6};
    osg_gateway_module_t *module=&gateway.modules[3];
    *module=(osg_gateway_module_t){.occupied=true,.online=true,.node=7,.source_node=42,
        .boot_session=4,.lease=5,.configuration_generation=8,.profile={.button_count=3,.axis_count=1}};
    osg_gateway_panel_t receiver={0};
    const osg_gateway_panel_slot_t *slot=&receiver.slots[3];
    notices_t log={.receiver=&receiver};
    const uint32_t base=UINT32_MAX-10;
    osg_gateway_message_t layout=packet(OSG_PANEL_LAYOUT,1,base);
    layout.panel_layout=(osg_panel_layout_t){.contact_count=1,.encoder_count=1,.maintained_contacts={1}};
    layout.source_node=43;
    assert(!osg_gateway_panel_receive(&receiver,&gateway,&layout,100,notify,&log));
    layout.source_node=42;
    assert(osg_gateway_panel_receive(&receiver,&gateway,&layout,100,notify,&log));

    osg_gateway_message_t state=packet(OSG_PANEL_STATE,2,base);
    state.panel_cursor=19;
    state.panel_state=(osg_panel_state_t){.axis_available=1,.axes={123}};
    assert(osg_gateway_panel_receive(&receiver,&gateway,&state,101,notify,&log));
    assert(slot->valid && slot->cursor==19 && slot->state.axes[0]==123);

    osg_gateway_message_t edges=packet(OSG_PANEL_EDGES,3,base+12);
    edges.panel_cursor=21;
    edges.panel_edges=(osg_panel_edges_t){.count=2,.entries={
        {.control=0,.pressed=true,.captured_at_ms=base+4},
        {.control=1,.pressed=true,.captured_at_ms=base+11},
    }};
    assert(osg_gateway_panel_receive(&receiver,&gateway,&edges,112,notify,&log));
    assert(slot->cursor==21 && slot->state.held[0]==1 && slot->occurrence_at_ms==0);
    assert(!osg_gateway_panel_receive(&receiver,&gateway,&edges,113,notify,&log));
    assert(slot->valid && log.count==3);

    state.sequence=5;
    state.panel_sampled_at_ms=base+16;
    state.panel_cursor=20;
    assert(!osg_gateway_panel_receive(&receiver,&gateway,&state,116,notify,&log));
    assert(!slot->valid && log.kinds[3]=='-');
    state.sequence=6;
    state.panel_cursor=22;
    assert(osg_gateway_panel_receive(&receiver,&gateway,&state,116,notify,&log));
    assert(slot->valid && slot->state.held[0]==0 && log.kinds[4]=='S');

    state.sequence=7;
    assert(osg_gateway_panel_receive(&receiver,&gateway,&state,300,notify,&log));
    osg_gateway_panel_tick(&receiver,&gateway,366,notify,&log);
    assert(!slot->valid && log.kinds[6]=='-');
    state.sequence=8;
    state.panel_sampled_at_ms++;
    assert(!osg_gateway_panel_receive(&receiver,&gateway,&state,367,notify,&log));
    state.sequence=9;
    state.panel_sampled_at_ms=base+268;
    assert(osg_gateway_panel_receive(&receiver,&gateway,&state,368,notify,&log));

    osg_gateway_message_t gap=packet(OSG_PANEL_GAP,10,base+269);
    gap.panel_gap=OSG_PANEL_GAP_EXHAUSTED;
    assert(osg_gateway_panel_receive(&receiver,&gateway,&gap,369,notify,&log));
    assert(slot->exhausted && !slot->valid && log.kinds[8]=='-');
    state.sequence=11;
    assert(!osg_gateway_panel_receive(&receiver,&gateway,&state,370,notify,&log));
    module->lease=9;
    layout.lease=9;
    layout.panel_sampled_at_ms=base+270;
    assert(osg_gateway_panel_receive(&receiver,&gateway,&layout,370,notify,&log));
    assert(!slot->exhausted && slot->lease==9);
    state.lease=9;
    state.sequence=2;
    state.panel_sampled_at_ms=base+270;
    assert(osg_gateway_panel_receive(&receiver,&gateway,&state,370,notify,&log));
    module->online=false;
    osg_gateway_panel_tick(&receiver,&gateway,371,notify,&log);
    osg_gateway_panel_tick(&receiver,&gateway,372,notify,&log);
    assert(!slot->valid && log.count==12 && log.kinds[11]=='-');

    module->online=true;
    state.sequence=3;
    state.panel_sampled_at_ms++;
    state.panel_state.axis_available=2;
    assert(!osg_gateway_panel_receive(&receiver,&gateway,&state,373,notify,&log));
    state.sequence=4;
    state.panel_state.axis_available=1;
    assert(osg_gateway_panel_receive(&receiver,&gateway,&state,373,notify,&log));
    gateway.modules[5]=*module;
    state.sequence=5;
    assert(!osg_gateway_panel_receive(&receiver,&gateway,&state,374,notify,&log));
    assert(!slot->valid && log.kinds[13]=='-');
    gateway.modules[5].occupied=false;
    state.panel_sampled_at_ms++;
    assert(osg_gateway_panel_receive(&receiver,&gateway,&state,375,notify,&log));
    edges.lease=9;
    edges.sequence=6;
    edges.panel_cursor=24;
    edges.panel_sampled_at_ms=state.panel_sampled_at_ms+1;
    assert(!osg_gateway_panel_receive(&receiver,&gateway,&edges,376,notify,&log));
    assert(!slot->valid && log.count==16 && log.kinds[15]=='-');
    return 0;
}
