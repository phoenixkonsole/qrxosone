#ifndef QRX_FNMATCH_H
#define QRX_FNMATCH_H
#define FNM_NOMATCH 1
#define FNM_CASEFOLD 16
#define FNM_PERIOD 4
int fnmatch(const char *pattern, const char *s, int flags);
#endif
