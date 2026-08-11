#ifndef LOG_SERVER_H
#define LOG_SERVER_H

#include <stdarg.h>

/* esp_log vprintf hook: forwards to UART + keeps a ring buffer for /log */
int log_server_vprintf(const char *fmt, va_list args);

/* Starts an HTTP server on :80 serving / (status) and /log (recent logs). */
void log_server_start(void);

#endif /* LOG_SERVER_H */
