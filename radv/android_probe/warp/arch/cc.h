/* lwIP's platform header for the in-title WARP gateway (x86-64, PS5 and the
 * Linux host test build). */
#ifndef WARP_ARCH_CC_H
#define WARP_ARCH_CC_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/time.h>

#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif
/* the host libc provides htons and friends */
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1
#define LWIP_TIMEVAL_PRIVATE 0
#define LWIP_ERRNO_STDINCLUDE 1

void warp_log(const char *fmt, ...);
#define LWIP_PLATFORM_DIAG(x) warp_log x
#define LWIP_PLATFORM_ASSERT(x) warp_log("lwip assert: %s (%s:%d)", x, __FILE__, __LINE__)

#endif
