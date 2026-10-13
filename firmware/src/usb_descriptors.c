#include "tusb.h"
#include "opensimgear/usb_descriptor.h"
#include "gateway_usb.h"
#include "board.h"
#include <string.h>

static uint8_t report_descriptor[OSG_GW_DESCRIPTOR_CAPACITY];
static uint16_t report_descriptor_size;
bool gateway_usb_prepare(const osg_gateway_t *gateway) {
    size_t length=osg_gateway_descriptor(gateway,report_descriptor,sizeof report_descriptor);
    if(!length || length>UINT16_MAX) return false;
    report_descriptor_size=(uint16_t)length; return true;
}
const uint8_t *tud_descriptor_device_cb(void) {
    static tusb_desc_device_t descriptor;
    descriptor=osg_usb_device_descriptor(OSG_USB_VID,OSG_USB_PID,0x0200);
    return (const uint8_t *)&descriptor;
}
const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    if(index) return NULL;
    static uint8_t descriptor[TUD_CONFIG_DESC_LEN+TUD_CDC_DESC_LEN+TUD_HID_DESC_LEN];
    osg_usb_hid_cdc_configuration(descriptor,sizeof descriptor,report_descriptor_size);
    return descriptor;
}
const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
    return instance ? NULL : report_descriptor;
}
const uint16_t *tud_descriptor_string_cb(uint8_t index,uint16_t langid) {
    (void)langid;
    static uint16_t descriptor[64]; static char uid[25]; const char *text=NULL;
    if(index==0) { descriptor[0]=(TUSB_DESC_STRING<<8)|4; descriptor[1]=0x0409; return descriptor; }
    switch(index) {
        case 1: text="OpenSimGear"; break;
        case 2: text="CAN Gateway v0.1.0"; break;
        case 3: board_uid_hex(uid); text=uid; break;
        case 4: text="Gateway Cockpit Console"; break;
        case 5: text="Registered CAN Controllers"; break;
        default: return NULL;
    }
    size_t length=strlen(text); if(length>63) length=63;
    for(size_t i=0;i<length;i++) descriptor[i+1]=(uint8_t)text[i];
    descriptor[0]=(uint16_t)((TUSB_DESC_STRING<<8)|(2*length+2)); return descriptor;
}
