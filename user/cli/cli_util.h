#ifndef CLI_UTIL_H
#define CLI_UTIL_H

int parse_args(int argc, char **argv, const char **cmd, const char **arg);
void log_info(const char *fmt, ...);
void log_error(const char *fmt, ...);

#endif /* CLI_UTIL_H */
