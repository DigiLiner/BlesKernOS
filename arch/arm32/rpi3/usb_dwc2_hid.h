#ifndef BLES_ARCH_ARM32_RPI3_USB_DWC2_HID_H
#define BLES_ARCH_ARM32_RPI3_USB_DWC2_HID_H

#include "../../../kernel/include/types.h"

/* Stage 9 polling host driver.  It deliberately targets the USB HID boot
 * subset first: keyboard/mouse and one tier of ordinary USB 1.1 hubs. */
int rpi3_dwc2_hid_init(void);
void rpi3_dwc2_hid_poll(void);
bool rpi3_dwc2_hid_online(void);
uint32_t rpi3_dwc2_hid_device_count(void);

#endif
