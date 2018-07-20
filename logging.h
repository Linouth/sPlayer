#ifndef LOGGING_H_
#define LOGGING_H_

enum log_levels {
    LOG_LINFO,
    LOG_LWARN,
    LOG_LERR,
    LOG_LDEBUG
};

void log_info(const char *format, ...);
void log_warn(const char *filename, int line, const char *format, ...);
void log_err(const char *filename, int line, const char *format, ...);
void log_debug(const char *filename, int line, const char *format, ...);

#define LOG_WARN(...) log_warn(__FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERR(...) log_err(__FILE__, __LINE__, __VA_ARGS__)
#ifdef DEBUG
#define LOG_DEBUG(...) log_debug(__FILE__, __LINE__, __VA_ARGS__)
#else
#define LOG_DEBUG(...)
#endif

#endif /* LOGGING_H_ */
