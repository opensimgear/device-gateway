#ifndef OSG_GATEWAY_H
#define OSG_GATEWAY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define OSG_MODULE_COUNT 8
#define OSG_MAX_REPORT_SIZE 16
#define OSG_LINE_CAPACITY 4096
#define OSG_RESPONSE_CAPACITY 16384
#define OSG_WATCHDOG_MS 10000
#define OSG_FIRMWARE_VERSION "0.1.1"
#define OSG_PROTOCOL_VERSION 1

typedef struct {
  char id[41], name[321], kind[16];
  uint8_t button_count, axis_count;
  bool online, registered;
  uint32_t buttons, applied_buttons;
  uint16_t axes[6];
  char label[321];
  uint8_t debounce;
  bool inverted, pending_buttons, axes_valid;
  uint32_t button_deadline;
} osg_module_t;

typedef struct {
  osg_module_t modules[OSG_MODULE_COUNT];
  osg_module_t layout[OSG_MODULE_COUNT];
  uint8_t module_count, layout_count;
  bool running, mounted, connected, watchdog_armed, reconnecting;
  uint32_t last_command;
  char serial[40];
} osg_gateway_t;

void osg_gateway_init(osg_gateway_t *gateway, const char *serial);
void osg_gateway_tick(osg_gateway_t *gateway, uint32_t now, bool mounted, bool connected);
void osg_gateway_disconnect(osg_gateway_t *gateway);
void osg_gateway_commit_layout(osg_gateway_t *gateway);
bool osg_gateway_needs_reconnect(const osg_gateway_t *gateway);
bool osg_gateway_command(osg_gateway_t *gateway, const char *line, size_t length, uint32_t now, char *response, size_t response_capacity);
size_t osg_gateway_report(const osg_gateway_t *gateway, unsigned index, uint8_t report[OSG_MAX_REPORT_SIZE]);
size_t osg_gateway_error(const osg_gateway_t *gateway, const char *message, char *response, size_t response_capacity);
#endif
