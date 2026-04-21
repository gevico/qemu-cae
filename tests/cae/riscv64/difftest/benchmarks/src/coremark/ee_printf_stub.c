/*
 * Stub ee_printf for baremetal CoreMark.
 * HAS_PRINTF=0 eliminates most calls, but a few code paths
 * still reference ee_printf. This no-op satisfies the linker.
 */
#include <stdarg.h>

void uart_send_char(char c)
{
    (void)c;
}

int ee_printf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}
