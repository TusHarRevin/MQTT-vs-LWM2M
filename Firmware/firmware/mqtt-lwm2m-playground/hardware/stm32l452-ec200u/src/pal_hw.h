#ifndef PAL_HW_H
#define PAL_HW_H

/* Board-only additions to pal.h. */

#include <stdbool.h>
#include <stdint.h>

/* Wall clock from the lab: Unix epoch ms now, and the PC's UTC offset (only
 * used to print console timestamps in local time). */
void pal_hw_set_wall(uint64_t epoch_ms, int32_t utc_offset_min);
bool pal_hw_wall_known(void);

#endif /* PAL_HW_H */
