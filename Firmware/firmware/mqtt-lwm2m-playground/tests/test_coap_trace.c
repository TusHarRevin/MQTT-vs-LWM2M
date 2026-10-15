/* CoAP decoding and LwM2M naming on hand-built messages. */
#include <string.h>

#include "check.h"
#include "coap_trace.h"

int main(void)
{
    /* CON POST /rd?ep=x  mid 0x1234, token 0xAB */
    static const uint8_t reg[] = { 0x41, 0x02, 0x12, 0x34, 0xAB, 0xB2, 'r', 'd', 0x44, 'e', 'p', '=', 'x' };
    /* ACK 2.01 mid 0x1234 */
    static const uint8_t reg_ack[] = { 0x61, 0x41, 0x12, 0x34, 0xAB };
    /* CON GET /3303/0/5700 Observe=0, mid 7 */
    static const uint8_t obs[] = { 0x40, 0x01, 0x00, 0x07, 0x60, 0x54, '3', '3', '0', '3', 0x01, '0',
                                   0x04, '5', '7', '0', '0' };
    /* NON 2.05 Observe=5 with payload "x" */
    static const uint8_t note[] = { 0x51, 0x45, 0x00, 0x09, 0xAB, 0x61, 0x05, 0xFF, 'x' };
    static const uint8_t bad[] = { 0x00, 0x00 };
    coap_info_t ci;
    coap_tracker_t t;
    char name[48];
    double lat;

    memset(&t, 0, sizeof(t));
    CHECK(coap_parse(reg, sizeof(reg), &ci) == 0);
    CHECK(ci.type == COAP_CON && ci.code == 2 && ci.mid == 0x1234 && ci.tkl == 1);
    CHECK(strcmp(ci.uri, "/rd?ep=x") == 0);
    CHECK(coap_track(&t, &ci, true, 1000, name, sizeof(name), &lat) == 1);
    CHECK(strcmp(name, "REGISTER") == 0);
    CHECK(coap_track(&t, &ci, true, 2000, name, sizeof(name), &lat) == 2); /* retransmission */

    CHECK(coap_parse(reg_ack, sizeof(reg_ack), &ci) == 0);
    coap_track(&t, &ci, false, 5000, name, sizeof(name), &lat);
    CHECK(strcmp(name, "REGISTER RESPONSE") == 0);
    CHECK_NEAR(lat, 4.0, 1e-9); /* measured from the FIRST send */

    CHECK(coap_parse(obs, sizeof(obs), &ci) == 0);
    CHECK(ci.has_observe && ci.observe == 0 && strcmp(ci.uri, "/3303/0/5700") == 0);
    coap_track(&t, &ci, false, 0, name, sizeof(name), &lat);
    CHECK(strcmp(name, "OBSERVE") == 0);

    CHECK(coap_parse(note, sizeof(note), &ci) == 0);
    CHECK(ci.payload_len == 1 && ci.observe == 5);
    coap_track(&t, &ci, true, 0, name, sizeof(name), &lat);
    CHECK(strcmp(name, "NOTIFY") == 0);

    CHECK(coap_parse(bad, sizeof(bad), &ci) == -1);
    CHECK_DONE();
}
