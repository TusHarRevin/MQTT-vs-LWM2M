#ifndef CHECK_H
#define CHECK_H
/* Minimal test helpers: no framework needed. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static int check_failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); check_failures++; } } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) <= (eps))
#define CHECK_DONE() do { if (check_failures) { printf("%d check(s) failed\n", check_failures); return 1; } printf("all checks passed\n"); return 0; } while (0)
#endif
