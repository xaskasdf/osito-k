/* OsitoK shim — syslog.h
 * Mesa src/util/log.c uses syslog when running as a non-root daemon.
 * On OsitoK we always print to console so syslog is a no-op. */
#ifndef OSITO_SYSLOG_H
#define OSITO_SYSLOG_H 1
#include <stdarg.h>
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7
#define LOG_USER    (1 << 3)
#define LOG_PID     0x01
#define LOG_NDELAY  0x08
static inline void openlog(const char *id, int o, int f)  { (void)id; (void)o; (void)f; }
static inline void closelog(void)                          { }
static inline void syslog(int p, const char *fmt, ...)     { (void)p; (void)fmt; }
static inline void vsyslog(int p, const char *fmt, va_list a) { (void)p; (void)fmt; (void)a; }
#endif
