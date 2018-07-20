#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "logging.h"


void log_va(FILE *ostream, const char *format, va_list args) {
    vfprintf(stderr, format, args);
    fprintf(ostream, "\n");

    va_end(args);
    fflush(ostream);
}

void log_level(int level) {
    FILE *ostream;
    char level_str[8] = {0};

    switch (level) {
        default:
        case LOG_LINFO:
            ostream = stdout;
            strcpy(level_str, "INFO");
            break;
        case LOG_LDEBUG:
            ostream = stdout;
            strcpy(level_str, "DEBUG");
            break;
        case LOG_LWARN:
            ostream = stdout;
            strcpy(level_str, "WARNING");
            break;
        case LOG_LERR:
            ostream = stderr;
            strcpy(level_str, "ERROR");
            break;
    }

    fprintf(ostream, "[%-8s] ", level_str);
}

void log_info(const char *format, ...) {
    log_level(LOG_LINFO);
    fflush(stdout);

    va_list args;
    va_start(args, format);
    log_va(stdout, format, args);
}

void log_warn(const char *filename, int line, const char *format, ...) {
    log_level(LOG_LWARN);
    fprintf(stderr, "<%s:%d> ", filename, line);

    va_list args;
    va_start(args, format);
    log_va(stderr, format, args);
}

void log_err(const char *filename, int line, const char *format, ...) {
    log_level(LOG_LERR);
    fprintf(stderr, "<%s:%d> ", filename, line);

    va_list args;
    va_start(args, format);
    log_va(stderr, format, args);
}

void log_debug(const char *filename, int line, const char *format, ...) {
    log_level(LOG_LDEBUG);
    fprintf(stdout, "<%s:%d> ", filename, line);
    fflush(stdout);

    va_list args;
    va_start(args, format);
    log_va(stdout, format, args);
}
