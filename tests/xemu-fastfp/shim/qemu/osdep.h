/* Minimal osdep shim for building softfloat natively in the differential test */
#ifndef SHIM_OSDEP_H
#define SHIM_OSDEP_H
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <math.h>
#include <glib.h>
#if defined(__SIZEOF_INT128__)
#define CONFIG_INT128 1
#endif
#include "qemu/compiler.h"
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif
#define QEMU_IS_ALIGNED(n, m) (((n) % (m)) == 0)
#define ROUND_UP(n, d) (((n) + (d) - 1) & -(0 ? (n) : (d)))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#endif
