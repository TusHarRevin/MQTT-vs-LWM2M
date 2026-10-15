#ifndef FW_LOG_H
#define FW_LOG_H

#include "platform/port.h"

enum { FW_LOG_ERROR = 0, FW_LOG_WARN = 1, FW_LOG_INFO = 2, FW_LOG_DEBUG = 3 };

#ifndef FW_LOG_LEVEL
#define FW_LOG_LEVEL FW_LOG_INFO
#endif

#define FW_LOG(lvl, tag, ...)                                                  \
    do {                                                                       \
        if ((lvl) <= FW_LOG_LEVEL) port_log((lvl), (tag), __VA_ARGS__);        \
    } while (0)

#define LOG_E(tag, ...) FW_LOG(FW_LOG_ERROR, tag, __VA_ARGS__)
#define LOG_W(tag, ...) FW_LOG(FW_LOG_WARN, tag, __VA_ARGS__)
#define LOG_I(tag, ...) FW_LOG(FW_LOG_INFO, tag, __VA_ARGS__)
#define LOG_D(tag, ...) FW_LOG(FW_LOG_DEBUG, tag, __VA_ARGS__)

#endif /* FW_LOG_H */
