#include <stdarg.h>

#include "module_log.h"

/* Standalone users can provide their own log_printf implementation.
 * When they do not, this weak sink keeps diagnostics optional and gives the
 * static module no dependency on an application-specific logging service. */
__attribute__((weak, format(printf, 1, 2)))
void log_printf(const char *format, ...) {
	(void)format;
}
