/* Payload padding hits the requested size exactly. */
#include <string.h>

#include "check.h"
#include "telemetry.h"

int main(void)
{
    char buf[4096];
    SensorData_t d = { 27.4f, 62.1f, 1012.3f, 87 };
    int natural = telemetry_build(buf, sizeof(buf), "dev", 1, 1700000000000ULL, &d, 0);

    CHECK(natural > 50);
    CHECK(buf[0] == '{' && buf[natural - 1] == '}');
    CHECK(strstr(buf, "\"temperature\":27.40") != NULL);
    CHECK(strstr(buf, "\"battery\":87") != NULL);

    for (int target = natural + 11; target < 3000; target += 97) {
        int n = telemetry_build(buf, sizeof(buf), "dev", 1, 1700000000000ULL, &d, (size_t)target);
        CHECK(n == target);
        CHECK((int)strlen(buf) == n);
        CHECK(buf[n - 1] == '}' && buf[n - 2] == '"');
    }
    /* smaller than natural: natural size is used */
    CHECK(telemetry_build(buf, sizeof(buf), "dev", 1, 0, &d, 10) < 200);
    /* too small a buffer is reported */
    CHECK(telemetry_build(buf, 20, "dev", 1, 0, &d, 0) == -1);
    CHECK_DONE();
}
