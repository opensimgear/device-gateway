#include "opensimgear/gateway_panel.h"
#include <string.h>

static bool module_matches(const osg_gateway_module_t *module, uint8_t node, uint8_t source_node,
                           uint32_t boot, uint32_t lease, uint32_t configuration_generation) {
    return module->occupied && module->online && module->node==node &&
        module->source_node==source_node && module->boot_session==boot &&
        module->lease==lease && module->configuration_generation==configuration_generation;
}

static bool scope_current(const osg_gateway_panel_slot_t *slot,
                          const osg_gateway_module_t *module, uint32_t epoch) {
    return slot->lease && slot->gateway_epoch==epoch &&
        module_matches(module,slot->node,slot->source_node,slot->boot_session,slot->lease,
                       slot->configuration_generation) &&
        osg_gateway_identity_equal(&slot->identity,&module->identity);
}

static void withdraw(osg_gateway_panel_slot_t *slot, unsigned index,
                     osg_gateway_panel_notify_fn notify, void *context) {
    slot->valid=false;
    if(slot->loss_notified) return;
    slot->loss_notified=true;
    if(notify) notify(context,index,NULL);
}

static void expire(osg_gateway_panel_slot_t *slot, unsigned index, uint32_t now,
                   osg_gateway_panel_notify_fn notify, void *context) {
    if(slot->valid && (uint32_t)(now-slot->fresh_at_ms)>=OSG_GW_PANEL_FRESHNESS_MS)
        withdraw(slot,index,notify,context);
}

void osg_gateway_panel_tick(osg_gateway_panel_t *receiver, const osg_gateway_t *gateway,
                            uint32_t now, osg_gateway_panel_notify_fn notify, void *context) {
    if(!receiver || !gateway) return;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) {
        osg_gateway_panel_slot_t *slot=&receiver->slots[i];
        if(slot->has_layout && !scope_current(slot,&gateway->modules[i],gateway->epoch))
            withdraw(slot,i,notify,context);
        else expire(slot,i,now,notify,context);
    }
}

static int resolve(osg_gateway_panel_t *receiver, const osg_gateway_t *gateway,
                   const osg_gateway_message_t *packet, osg_gateway_panel_notify_fn notify, void *context) {
    int index=-1;
    bool ambiguous=false;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) {
        const osg_gateway_module_t *module=&gateway->modules[i];
        if(!module_matches(module,packet->node,packet->source_node,packet->boot_session,packet->lease,
                           packet->configuration_generation)) continue;
        if(index<0) { index=(int)i; continue; }
        ambiguous=true;
        osg_gateway_panel_slot_t *slot=&receiver->slots[i];
        if(slot->has_layout && scope_current(slot,module,gateway->epoch)) withdraw(slot,i,notify,context);
    }
    if(!ambiguous) return index;
    osg_gateway_panel_slot_t *slot=&receiver->slots[index];
    if(slot->has_layout && scope_current(slot,&gateway->modules[index],gateway->epoch))
        withdraw(slot,(unsigned)index,notify,context);
    return -1;
}

static bool mask_clear(const uint8_t *mask, unsigned start, unsigned end) {
    for(unsigned i=start;i<end;i++) {
        if(mask[i/8]&(1U<<(i%8))) return false;
    }
    return true;
}

static unsigned encoder_end(const osg_panel_layout_t *layout) {
    return layout->contact_count+2U*layout->encoder_count;
}

static bool layout_valid(const osg_panel_layout_t *layout, const osg_gateway_profile_t *profile) {
    unsigned controls=encoder_end(layout)+3U*layout->selector_count;
    return layout->encoder_count<=8 && layout->selector_count<=8 &&
        controls<=OSG_GW_MAX_BUTTONS && controls==profile->button_count &&
        mask_clear(layout->maintained_contacts,layout->contact_count,OSG_GW_MAX_BUTTONS);
}

static bool state_valid(const osg_panel_state_t *state, const osg_panel_layout_t *layout,
                        const osg_gateway_profile_t *profile) {
    unsigned axes=profile->axis_count;
    if(axes>OSG_GW_MAX_AXES || ((unsigned)state->axis_available>>axes) ||
       !mask_clear(state->held,profile->button_count,OSG_GW_MAX_BUTTONS) ||
       !mask_clear(state->held,layout->contact_count,encoder_end(layout))) return false;
    for(unsigned i=axes;i<OSG_GW_MAX_AXES;i++) if(state->axes[i]) return false;
    return true;
}

/* Only source-clock timestamps use serial arithmetic. Sequences and cursors
 * never wrap in a wire scope. */
static bool clock_follows(uint32_t current, uint32_t previous) {
    return (uint32_t)(current-previous)<UINT32_C(0x80000000);
}

static bool edges_valid(const osg_gateway_panel_slot_t *slot, const osg_gateway_message_t *packet,
                        const osg_gateway_profile_t *profile, uint32_t sample_age) {
    unsigned count=packet->panel_edges.count;
    if(!count || count>OSG_PANEL_MAX_EDGES || packet->panel_cursor<=slot->cursor ||
       packet->panel_cursor-slot->cursor!=count) return false;
    uint32_t previous=slot->occurrence_at_ms;
    for(unsigned i=0;i<count;i++) {
        const osg_panel_edge_t *edge=&packet->panel_edges.entries[i];
        uint32_t age=packet->panel_sampled_at_ms-edge->captured_at_ms;
        if(edge->control>=profile->button_count || !clock_follows(edge->captured_at_ms,previous) ||
           age>=OSG_GW_PANEL_FRESHNESS_MS-sample_age) return false;
        previous=edge->captured_at_ms;
    }
    return true;
}

static void apply_edges(osg_gateway_panel_slot_t *slot, const osg_panel_edges_t *edges) {
    unsigned end=encoder_end(&slot->layout);
    for(unsigned i=0;i<edges->count;i++) {
        const osg_panel_edge_t *edge=&edges->entries[i];
        unsigned control=edge->control;
        if(control>=slot->layout.contact_count && control<end) continue;
        uint8_t bit=(uint8_t)(1U<<(control%8));
        if(edge->pressed) slot->state.held[control/8]|=bit;
        else slot->state.held[control/8]&=(uint8_t)~bit;
    }
    slot->occurrence_at_ms=edges->entries[edges->count-1].captured_at_ms;
}

static bool accept_input(osg_gateway_panel_slot_t *slot, const osg_gateway_message_t *packet,
                         const osg_gateway_profile_t *profile, uint32_t now) {
    if(!slot->has_layout) return false;
    uint32_t sample=packet->panel_sampled_at_ms;
    if(!clock_follows(sample,slot->sampled_at_ms)) return false;
    uint32_t advance=sample-slot->sampled_at_ms;
    /* Source and gateway clocks have independent origins. Relative progress
     * still detects a late sample against the preceding observed sample. */
    uint32_t elapsed=now-slot->fresh_at_ms;
    uint32_t sample_age=advance>elapsed?0:elapsed-advance;
    if(sample_age>=OSG_GW_PANEL_FRESHNESS_MS) return false;
    if(packet->panel_kind==OSG_PANEL_STATE) {
        if((!slot->valid && slot->has_input_sample && (!advance || packet->panel_cursor<slot->cursor)) ||
           (slot->valid && packet->panel_cursor!=slot->cursor) ||
           !state_valid(&packet->panel_state,&slot->layout,profile)) return false;
        slot->state=packet->panel_state;
        slot->state_sampled_at_ms=sample;
        slot->occurrence_at_ms=sample;
        slot->valid=true;
        slot->loss_notified=false;
    } else {
        if(!slot->valid || !edges_valid(slot,packet,profile,sample_age)) return false;
        apply_edges(slot,&packet->panel_edges);
    }
    if(advance) {
        slot->sample_received_at_ms=now;
        slot->fresh_at_ms=now-sample_age;
    }
    slot->has_input_sample=true;
    slot->sampled_at_ms=sample;
    slot->received_at_ms=now;
    slot->cursor=packet->panel_cursor;
    return true;
}

bool osg_gateway_panel_receive(osg_gateway_panel_t *receiver, const osg_gateway_t *gateway,
                               const osg_gateway_message_t *packet, uint32_t now,
                               osg_gateway_panel_notify_fn notify, void *context) {
    if(!receiver || !gateway || !packet || packet->type!=OSG_GW_PANEL_EVENT || packet->direct_usb ||
       !packet->node || packet->node>OSG_GW_MAX_MODULES || packet->source_node>127 ||
       !packet->boot_session || !packet->lease || !packet->gateway_epoch ||
       packet->gateway_epoch!=gateway->epoch || !packet->configuration_generation || !packet->sequence) return false;
    int resolved=resolve(receiver,gateway,packet,notify,context);
    if(resolved<0) return false;
    unsigned index=(unsigned)resolved;
    const osg_gateway_module_t *module=&gateway->modules[index];
    osg_gateway_panel_slot_t *slot=&receiver->slots[index];
    if(!scope_current(slot,module,gateway->epoch)) {
        if(slot->has_layout) withdraw(slot,index,notify,context);
        *slot=(osg_gateway_panel_slot_t){.identity=module->identity,.node=module->node,
            .source_node=module->source_node,.boot_session=module->boot_session,.lease=module->lease,
            .gateway_epoch=gateway->epoch,.configuration_generation=module->configuration_generation};
    }
    expire(slot,index,now,notify,context);
    if(slot->exhausted || packet->sequence<=slot->sequence) return false;
    bool skipped=slot->sequence && packet->sequence-slot->sequence!=1;
    slot->sequence=packet->sequence;

    bool terminal=packet->panel_kind==OSG_PANEL_GAP && packet->panel_gap==OSG_PANEL_GAP_EXHAUSTED;
    if(terminal || packet->sequence==UINT32_MAX) {
        slot->exhausted=true;
        withdraw(slot,index,notify,context);
        return terminal;
    }
    if(skipped) withdraw(slot,index,notify,context);
    if(packet->panel_kind==OSG_PANEL_GAP && packet->panel_gap==OSG_PANEL_GAP_RING) {
        withdraw(slot,index,notify,context);
        return true;
    }

    bool accepted=false;
    switch(packet->panel_kind) {
    case OSG_PANEL_LAYOUT:
        accepted=layout_valid(&packet->panel_layout,&module->profile) &&
            (!slot->has_layout || memcmp(&slot->layout,&packet->panel_layout,sizeof slot->layout)==0);
        if(accepted) {
            if(!slot->has_layout) {
                slot->sampled_at_ms=packet->panel_sampled_at_ms;
                slot->sample_received_at_ms=now;
                slot->fresh_at_ms=now;
            }
            slot->layout=packet->panel_layout;
            slot->has_layout=true;
        }
        break;
    case OSG_PANEL_STATE:
    case OSG_PANEL_EDGES:
        accepted=accept_input(slot,packet,&module->profile,now);
        break;
    default: break;
    }
    if(accepted) {
        if(notify) notify(context,index,packet);
    } else if(slot->valid) withdraw(slot,index,notify,context);
    return accepted;
}
