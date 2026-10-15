#include "application/device.h"

int main(void)
{
    if (device_init() != FW_OK) {
        return 1;
    }
    for (;;) {
        device_step();
    }
}
