/*
 * Keeps tun_probe loadable on AXIS OS < 12.10 (glibc 2.34): GCC 13 / glibc 2.38
 * C23 headers redirect strtol/strtoul/sscanf to __isoc23_* @ GLIBC_2.38, which
 * older firmware lacks. These forwarders satisfy such references with the classic
 * GLIBC_2.17 symbols. <stdlib.h>/<stdio.h> are deliberately not included: their
 * C23 redirect would turn these calls back into __isoc23_* and recurse.
 */
#include <stdarg.h>

extern long strtol(const char *nptr, char **endptr, int base);
extern unsigned long strtoul(const char *nptr, char **endptr, int base);
extern int vsscanf(const char *str, const char *format, va_list ap);

long __isoc23_strtol(const char *nptr, char **endptr, int base) {
    return strtol(nptr, endptr, base);
}

unsigned long __isoc23_strtoul(const char *nptr, char **endptr, int base) {
    return strtoul(nptr, endptr, base);
}

int __isoc23_sscanf(const char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsscanf(str, format, ap);
    va_end(ap);
    return ret;
}
