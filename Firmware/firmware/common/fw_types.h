#ifndef FW_TYPES_H
#define FW_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Every fallible function returns FW_OK (0), a positive byte count where
 * documented, or one of these negative codes. */
typedef enum {
    FW_OK              = 0,
    FW_ERR             = -1,
    FW_ERR_PARAM       = -2,
    FW_ERR_TIMEOUT     = -3,
    FW_ERR_IO          = -4,
    FW_ERR_NOMEM       = -5,
    FW_ERR_UNSUPPORTED = -6,
    FW_ERR_STATE       = -7,
    FW_ERR_CLOSED      = -8,
    FW_ERR_NOT_FOUND   = -9,
    FW_ERR_CORRUPT     = -10,
    FW_ERR_SECURITY    = -11,
} fw_err_t;

/* Timeout value meaning "block until something happens". */
#define FW_WAIT_FOREVER UINT32_MAX

#define FW_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define FW_UNUSED(x)    ((void)(x))
#define FW_MIN(a, b)    ((a) < (b) ? (a) : (b))

#endif /* FW_TYPES_H */
