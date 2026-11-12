#ifndef DV3K_PORT_H
#define DV3K_PORT_H

#include <stdint.h>

uint32_t port_read(uint8_t *p, uint32_t n);
uint32_t port_write_room(void);
void port_write(const uint8_t *p, uint32_t n);
void port_flush(void);
void port_discard(void);
void port_discard_rx(void);
void port_event(uint32_t event); /* in the USB ISR, per TinyUSB event */

void host_drop(int closed);
void host_reset(void);
void host_bootsel(void);

#endif
