#ifndef LOGGING_H_
#define LOGGING_H_

enum log_levels {
    LOG_LINFO,
    LOG_LWARN,
    LOG_LERR
};

void log_info(const char *format, ...);
void log_warn(const char *filename, int line, const char *format, ...);
void log_err(const char *filename, int line, const char *format, ...);

#define LOG_WARN(...) log_warn(__FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERR(...) log_err(__FILE__, __LINE__, __VA_ARGS__)

#endif /* LOGGING_H_ */
