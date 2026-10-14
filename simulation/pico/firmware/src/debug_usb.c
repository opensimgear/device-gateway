#include "debug_usb.h"
#include "opensimgear/usb_descriptor.h"
#include "tusb.h"
#include <string.h>

static uint8_t reports[OSG_GW_DESCRIPTOR_CAPACITY];
static uint16_t report_size;
static char usb_serial[40];

bool osg_debug_usb_prepare(const osg_gateway_t *gateway,const char *serial) {
    if(!gateway || !serial || strlen(serial)>=sizeof usb_serial ||
       !osg_gateway_registry_valid(&gateway->active)) return false;
    size_t size=osg_gateway_descriptor(gateway,reports,sizeof reports);
    bool any=false;
    for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++)
        any|=gateway->active.slots[i].occupied && gateway->active.slots[i].enabled;
    if((any && !size) || size>UINT16_MAX) return false;
    strcpy(usb_serial,serial);report_size=(uint16_t)size;return true;
}
const uint8_t *tud_descriptor_device_cb(void) {
    static tusb_desc_device_t descriptor;
    descriptor=osg_usb_device_descriptor(0xcafe,0x4010,0x0200);
    return (const uint8_t *)&descriptor;
}
const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    if(index) return NULL;
    static uint8_t descriptor[TUD_CONFIG_DESC_LEN+TUD_CDC_DESC_LEN+TUD_HID_DESC_LEN];
    if(report_size) osg_usb_hid_cdc_configuration(descriptor,sizeof descriptor,report_size);
    else osg_usb_cdc_configuration(descriptor,sizeof descriptor);
    return descriptor;
}
const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
    return !instance && report_size ? reports : NULL;
}
const uint16_t *tud_descriptor_string_cb(uint8_t index,uint16_t langid) {
    (void)langid;static uint16_t descriptor[64];const char *text=NULL;
    if(!index) { descriptor[0]=0x0304;descriptor[1]=0x0409;return descriptor; }
    switch(index) {
        case 1: text="OpenSimGear";break;
        case 2: text="OpenSimGear Pico Debug Gateway";break;
        case 3: text=usb_serial;break;
        case 4: text="Cockpit debug console";break;
        case 5: text="Debug controllers";break;
        default: return NULL;
    }
    size_t size=strlen(text);if(size>63) size=63;
    for(unsigned i=0;i<size;i++) descriptor[i+1]=(uint8_t)text[i];
    descriptor[0]=(uint16_t)(0x0300|((size+1)*2));return descriptor;
}
