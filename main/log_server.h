#ifndef LOG_SERVER_H
#define LOG_SERVER_H

#include <stdarg.h>

/* esp_log vprintf hook: forwards to UART + keeps a ring buffer for /log */
int log_server_vprintf(const char *fmt, va_list args);

/* Starts an HTTP server on :80 serving / (status), /log (recent logs),
 * and POST /ota (triggers the over-the-air update). */
void log_server_start(void);

/* Callback invoked when POST /ota is received (set by the app layer). */
void log_server_set_ota_trigger(void (*cb)(void));

#endif /* LOG_SERVER_H */
