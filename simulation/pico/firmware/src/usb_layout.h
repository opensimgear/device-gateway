#ifndef OSG_USB_LAYOUT_H
#define OSG_USB_LAYOUT_H
#include "gateway.h"
#define OSG_USB_VID 0xcafe
#define OSG_USB_PID 0x4010
#define OSG_CONFIGURATION_CAPACITY (9 + 66 + 25 * OSG_MODULE_COUNT)
#define OSG_REPORT_DESCRIPTOR_CAPACITY 96
#define OSG_USB_STRING_CAPACITY 127 /* Header + 126 UTF-16 units, at most 254 bytes. */
/* These buffers change only while USB is electrically detached. */
typedef struct {
  uint8_t configuration[OSG_CONFIGURATION_CAPACITY];
  uint8_t reports[OSG_MODULE_COUNT][OSG_REPORT_DESCRIPTOR_CAPACITY];
  uint16_t configuration_length, report_lengths[OSG_MODULE_COUNT];
} osg_usb_layout_t;
void osg_usb_layout_build(osg_usb_layout_t *layout, const osg_gateway_t *gateway);
size_t osg_hid_descriptor(unsigned button_count, unsigned axis_count, uint8_t *out);
/* Returns UTF-16 units including the descriptor header, or zero for an unknown string.
   HID interface names come from the committed layout, never mutable inventory. */
size_t osg_usb_string_descriptor(const osg_gateway_t *gateway, uint8_t index, uint16_t *out, size_t capacity);
#endif
