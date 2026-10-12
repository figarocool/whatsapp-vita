#ifndef VITA_SW_DECODER_MODULE_LOG_H
#define VITA_SW_DECODER_MODULE_LOG_H

__attribute__((format(printf, 1, 2)))
void log_printf(const char *format, ...);

#endif
