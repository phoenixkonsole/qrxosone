/* Included before every NSS source on AROS (build.sh, -include): functions libqrxaroscompat has
 * and AROS's headers do not declare (or hide in their KERNEL parts). */
#ifndef QRX_AROS_NSS_H
#define QRX_AROS_NSS_H
int getpagesize(void);
void syslog(int priority, const char *format, ...);
#endif
