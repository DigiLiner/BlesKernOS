#ifndef BLES_ARCH_ARM32_RPI3_MAILBOX_H
#define BLES_ARCH_ARM32_RPI3_MAILBOX_H

#include "../../../kernel/include/types.h"

#define RPI3_MBOX_PROP_CHANNEL 8U
#define RPI3_CLOCK_EMMC        1U

int rpi3_mailbox_property_call(volatile uint32_t *message, uint32_t bytes);
int rpi3_mailbox_get_clock_rate(uint32_t clock_id, uint32_t *rate_hz);
int rpi3_mailbox_get_arm_memory(uint32_t *base, uint32_t *bytes);
int rpi3_mailbox_set_power_state(uint32_t device_id, bool on, bool wait);

/* VideoCore bus aliases for SDRAM differ from ARM physical addresses. */
uint32_t rpi3_vc_bus_to_arm(uint32_t bus_addr);

#endif
