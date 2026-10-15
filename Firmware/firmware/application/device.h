#ifndef APP_DEVICE_H
#define APP_DEVICE_H

#include "common/fw_types.h"

/* Top-level application. Call device_init() once, then device_step() forever
 * from the main loop (bare metal) or a task (RTOS). */
int device_init(void);
void device_step(void);
const char *device_id(void);

#endif /* APP_DEVICE_H */
