#include "coap_trace.h"

#include <stdio.h>
#include <string.h>

const char *coap_type_name(uint8_t type)
{
    static const char *n[] = { "CON", "NON", "ACK", "RST" };
    return n[type & 3];
}

void coap_code_str(uint8_t code, char out[8])
{
    snprintf(out, 8, "%u.%02u", (unsigned)(code >> 5), (unsigned)(code & 0x1f));
}

const char *coap_method_name(uint8_t code)
{
    switch (code) {
    case 1: return "GET";
    case 2: return "POST";
    case 3: return "PUT";
    case 4: return "DELETE";
    case 5: return "FETCH";
    case 6: return "PATCH";
    case 7: return "iPATCH";
    default: return NULL;
    }
}

const char *coap_format_name(int cf)
{
    switch (cf) {
    case 0: return "text/plain";
    case 40: return "link-format";
    case 42: return "octet-stream";
    case 60: return "cbor";
    case 110: return "senml+json";
    case 112: return "senml+cbor";
    case 1541: case 11542: return "lwm2m+tlv";
    case 1543: case 11543: return "lwm2m+json";
    default: return "unknown";
    }
}

static uint32_t opt_uint(const uint8_t *p, size_t len)
{
    uint32_t v = 0;
    for (size_t i = 0; i < len && i < 4; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

static void append(char *dst, size_t cap, const char *sep, const uint8_t *p, size_t len)
{
    size_t o = strlen(dst);
    int n = snprintf(dst + o, cap - o, "%s%.*s", sep, (int)len, (const char *)p);
    (void)n;
}

int coap_parse(const uint8_t *buf, size_t len, coap_info_t *ci)
{
    size_t i;
    unsigned opt = 0;
    bool first_query = true;

    memset(ci, 0, sizeof(*ci));
    ci->content_format = -1;
    ci->accept = -1;
    if (len < 4 || (buf[0] >> 6) != 1) {
        return -1;
    }
    ci->type = (buf[0] >> 4) & 3;
    ci->tkl = buf[0] & 0x0f;
    ci->code = buf[1];
    ci->mid = (uint16_t)((buf[2] << 8) | buf[3]);
    if (ci->tkl > 8 || 4u + ci->tkl > len) {
        return -1;
    }
    memcpy(ci->token, buf + 4, ci->tkl);
    i = 4u + ci->tkl;

    while (i < len) {
        unsigned delta, olen;
        if (buf[i] == 0xff) {
            ci->payload_off = i + 1;
            ci->payload_len = len - i - 1;
            break;
        }
        delta = buf[i] >> 4;
        olen = buf[i] & 0x0f;
        i++;
        if (delta == 13) {
            if (i >= len) return -1;
            delta = buf[i++] + 13u;
        } else if (delta == 14) {
            if (i + 1 >= len) return -1;
            delta = ((unsigned)buf[i] << 8 | buf[i + 1]) + 269u;
            i += 2;
        } else if (delta == 15) {
            return -1;
        }
        if (olen == 13) {
            if (i >= len) return -1;
            olen = buf[i++] + 13u;
        } else if (olen == 14) {
            if (i + 1 >= len) return -1;
            olen = ((unsigned)buf[i] << 8 | buf[i + 1]) + 269u;
            i += 2;
        } else if (olen == 15) {
            return -1;
        }
        if (i + olen > len) {
            return -1;
        }
        opt += delta;
        switch (opt) {
        case 6: /* Observe */
            ci->has_observe = true;
            ci->observe = opt_uint(buf + i, olen);
            break;
        case 11: /* Uri-Path */
            append(ci->uri, sizeof(ci->uri), "/", buf + i, olen);
            if (ci->path_depth == 0) {
                snprintf(ci->first_seg, sizeof(ci->first_seg), "%.*s", (int)olen, (const char *)buf + i);
            }
            ci->path_depth++;
            break;
        case 12: /* Content-Format */
            ci->content_format = (int)opt_uint(buf + i, olen);
            break;
        case 15: /* Uri-Query */
            append(ci->uri, sizeof(ci->uri), first_query ? "?" : "&", buf + i, olen);
            first_query = false;
            break;
        case 17: /* Accept */
            ci->accept = (int)opt_uint(buf + i, olen);
            break;
        default:
            break;
        }
        i += olen;
    }
    return 0;
}

/* LwM2M name of a request. */
static const char *request_name(const coap_info_t *ci, bool from_device)
{
    const char *m = coap_method_name(ci->code);
    bool rd = strcmp(ci->first_seg, "rd") == 0;

    if (from_device) {
        if (ci->code == 2 && rd && ci->path_depth == 1) return "REGISTER";
        if (ci->code == 2 && rd) return "UPDATE";
        if (ci->code == 4 && rd) return "DEREGISTER";
        if (ci->code == 2 && strcmp(ci->first_seg, "dp") == 0) return "SEND";
        if (ci->code == 2 && strcmp(ci->first_seg, "bs") == 0) return "BOOTSTRAP-REQUEST";
        return m ? m : "REQUEST";
    }
    switch (ci->code) {
    case 1:
        if (ci->has_observe && ci->observe == 0) return "OBSERVE";
        if (ci->has_observe && ci->observe == 1) return "CANCEL OBSERVE";
        if (ci->accept == 40) return "DISCOVER";
        return "READ";
    case 2:
        return ci->path_depth >= 3 ? "EXECUTE" : (ci->path_depth == 1 ? "CREATE" : "WRITE (update)");
    case 3:
        return (ci->payload_len == 0 && strchr(ci->uri, '?')) ? "WRITE-ATTRIBUTES" : "WRITE";
    case 4:
        return "DELETE";
    case 5:
        return "READ-COMPOSITE";
    case 7:
        return "WRITE-COMPOSITE";
    default:
        return m ? m : "REQUEST";
    }
}

int coap_track(coap_tracker_t *t, const coap_info_t *ci, bool from_device, uint64_t now_us,
               char *name, size_t name_cap, double *lat_ms)
{
    /* Requests we send are matched against tx[], requests we receive against rx[]. */
    coap_pending_t *own = from_device ? t->tx : t->rx;
    coap_pending_t *peer = from_device ? t->rx : t->tx;
    *lat_ms = -1.0;

    /* Notifications: 2.05 with Observe, not a piggybacked response. */
    if (from_device && ci->code == 69 && ci->has_observe && ci->type != COAP_ACK) {
        snprintf(name, name_cap, "NOTIFY");
    } else if (ci->code != 0 && ci->code < 32) {
        snprintf(name, name_cap, "%s", request_name(ci, from_device));
    } else if (ci->type == COAP_ACK || ci->type == COAP_RST || ci->code >= 64) {
        /* response or empty ACK/RST: find the request it answers */
        for (int i = 0; i < 32; i++) {
            if (peer[i].used && peer[i].mid == ci->mid &&
                (ci->type == COAP_ACK || ci->type == COAP_RST)) {
                if (ci->type == COAP_RST) {
                    snprintf(name, name_cap, "RESET (%s)", peer[i].name);
                } else if (ci->code == 0) {
                    snprintf(name, name_cap, "ACK");
                } else {
                    snprintf(name, name_cap, "%s RESPONSE", peer[i].name);
                }
                *lat_ms = (double)(now_us - peer[i].t_us) / 1000.0;
                peer[i].used = false;
                return 0;
            }
        }
        snprintf(name, name_cap, "%s", ci->type == COAP_RST ? "RESET" : (ci->code == 0 ? "ACK" : "RESPONSE"));
        return 0;
    } else {
        snprintf(name, name_cap, "%s", ci->code == 0 ? "PING" : "MESSAGE");
    }

    /* Remember confirmable requests (and notifications) for matching. */
    if (ci->type == COAP_CON) {
        int free_slot = -1;
        for (int i = 0; i < 32; i++) {
            if (own[i].used && own[i].mid == ci->mid) {
                return ++own[i].sends; /* retransmission: keep first send time */
            }
            if (!own[i].used && free_slot < 0) {
                free_slot = i;
            }
        }
        if (free_slot < 0) {
            /* table full: evict the oldest */
            free_slot = 0;
            for (int i = 1; i < 32; i++) {
                if (own[i].t_us < own[free_slot].t_us) {
                    free_slot = i;
                }
            }
        }
        own[free_slot].used = true;
        own[free_slot].mid = ci->mid;
        own[free_slot].t_us = now_us;
        own[free_slot].sends = 1;
        snprintf(own[free_slot].name, sizeof(own[free_slot].name), "%s", name);
        return 1;
    }
    return 1;
}
