/*
 * Floor-compatibility shim for AXIS OS <= 12.x (glibc 2.34).
 *
 * The ACAP Native SDK 12.10 ships OpenSSL 3.5 (libcrypto/libssl) built with the
 * GCC 13 / glibc 2.38 C23 <stdlib.h>/<stdio.h> redirect, so those libraries
 * import __isoc23_strtol / __isoc23_strtoul / __isoc23_sscanf @ GLIBC_2.38.
 * When tun_probe links OpenSSL, the linker hoists those undefined symbols into
 * tun_probe's own dynamic symbol table, giving the executable a hard GLIBC_2.38
 * dependency. glibc 2.38 is absent on AXIS OS 12.x (2.34) and older, so without
 * this shim tun_probe would fail to load below OS 12.10 with an undefined-symbol
 * error, even though the binary is otherwise OS 13 ready.
 *
 * Provide local definitions that forward to the classic (non-C23) functions.
 * The classic functions are declared here WITHOUT including <stdlib.h>/<stdio.h>
 * on purpose: that keeps the C23 header macro from rewriting these call sites
 * back into the __isoc23_* variants (which would make the wrappers recurse into
 * themselves). The plain strtol/strtoul/vsscanf references bind their default
 * GLIBC_2.17 versions, so at link time these definitions satisfy OpenSSL's
 * references and drop the GLIBC_2.38 requirement, restoring the OS 11 floor.
 * On OS 13 they are simply thin pass-throughs.
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
