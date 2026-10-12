#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t id,deadline_ms;
    uint8_t size;
    bool fd,brs,deadline_valid;
    uint8_t data[64];
} board_can_frame_t;
typedef struct {
    uint32_t rx, tx, rx_dropped, tx_dropped, tx_failed, recoveries;
    uint32_t hal_error;
    uint8_t tx_queue, rx_error, tx_error;
    bool bus_off,recovering;
} board_can_diagnostics_t;

void board_init(void);
uint32_t board_millis(void);
void board_led(bool on);
bool board_usb_present(void);
void board_uid_hex(char out[25]);
void board_fault(void);
bool board_can_receive(board_can_frame_t *frame);
bool board_can_enqueue(const board_can_frame_t *frame);
void board_can_task(uint32_t now);
void board_can_reset(uint32_t now); /* Flush software/controller state after CAN node-ID migration. */
board_can_diagnostics_t board_can_diagnostics(void);

/* Opaque portable registry payload in a committed, CRC-protected two-page journal. */
#define BOARD_REGISTRY_CAPACITY 1984U
bool board_registry_load(uint8_t *payload, size_t capacity, size_t *length, uint32_t *epoch);
bool board_registry_commit(const uint8_t *payload, size_t length, uint32_t epoch);
