/* The string functions Blender uses often, compiled into the program. The SDK has them in a shared
 * library, which core 1 cannot call, and whose stubs return pointers in the wrong register. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void *memcpy(void *restrict d, const void *restrict s, size_t n)
{
	char *dp = d;
	const char *sp = s;
	if (n >= 16 && !(((uintptr_t)dp ^ (uintptr_t)sp) & 1)) {
		if ((uintptr_t)dp & 1) { *dp++ = *sp++; n--; }
		uint32_t *dl = (uint32_t *)dp;
		const uint32_t *sl = (const uint32_t *)sp;
		size_t words = n >> 2;
		n &= 3;
		while (words >= 4) {
			dl[0] = sl[0]; dl[1] = sl[1]; dl[2] = sl[2]; dl[3] = sl[3];
			dl += 4; sl += 4; words -= 4;
		}
		while (words--) *dl++ = *sl++;
		dp = (char *)dl;
		sp = (const char *)sl;
	}
	while (n--) *dp++ = *sp++;
	return d;
}
void *memmove(void *d, const void *s, size_t n)
{
	char *dp = d;
	const char *sp = s;
	if (dp <= sp || dp >= sp + n) {
		if (dp + n <= sp || dp >= sp + n) return memcpy(d, s, n);
		while (n--) *dp++ = *sp++;
	}
	else {
		dp += n; sp += n;
		while (n--) *--dp = *--sp;
	}
	return d;
}
void *memset(void *d, int c, size_t n)
{
	unsigned char *dp = d;
	if (n >= 16) {
		uint32_t v = (unsigned char)c;
		v |= v << 8; v |= v << 16;
		if ((uintptr_t)dp & 1) { *dp++ = (unsigned char)c; n--; }
		uint32_t *dl = (uint32_t *)dp;
		size_t words = n >> 2;
		n &= 3;
		while (words--) *dl++ = v;
		dp = (unsigned char *)dl;
	}
	while (n--) *dp++ = (unsigned char)c;
	return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;
	for (; n--; x++, y++) if (*x != *y) return *x - *y;
	return 0;
}
void *memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;
	for (; n--; p++) if (*p == (unsigned char)c) return (void *)p;
	return NULL;
}
size_t strlen(const char *s)
{
	const char *p = s;
	while (*p) p++;
	return p - s;
}
size_t strnlen(const char *s, size_t max)
{
	size_t n = 0;
	while (n < max && s[n]) n++;
	return n;
}
int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *(const unsigned char *)a - *(const unsigned char *)b;
}
int strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b) return *(const unsigned char *)a - *(const unsigned char *)b;
		if (!*a) break;
	}
	return 0;
}
char *strcpy(char *restrict d, const char *restrict s)
{
	char *r = d;
	while ((*d++ = *s++)) {}
	return r;
}
char *strncpy(char *restrict d, const char *restrict s, size_t n)
{
	char *r = d;
	for (; n && *s; n--) *d++ = *s++;
	while (n--) *d++ = 0;
	return r;
}
char *strcat(char *restrict d, const char *restrict s)
{
	strcpy(d + strlen(d), s);
	return d;
}
char *strncat(char *restrict d, const char *restrict s, size_t n)
{
	char *p = d + strlen(d);
	while (n-- && *s) *p++ = *s++;
	*p = 0;
	return d;
}
char *strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c) return (char *)s;
		if (!*s) return NULL;
	}
}
char *strrchr(const char *s, int c)
{
	const char *r = NULL;
	for (;; s++) {
		if (*s == (char)c) r = s;
		if (!*s) return (char *)r;
	}
}
char *strstr(const char *h, const char *n)
{
	size_t l = strlen(n);
	if (!l) return (char *)h;
	for (; *h; h++) if (*h == *n && !strncmp(h, n, l)) return (char *)h;
	return NULL;
}

/* Multibyte conversion: UTF-8 everywhere. The SDK's wcstombs failed even on plain ASCII. */
#include <wchar.h>
#include <errno.h>
static size_t utf8_decode(const char *s, size_t n, wchar_t *out)
{
	const unsigned char *p = (const unsigned char *)s;
	wchar_t c;
	size_t need;
	if (p[0] < 0x80) { *out = p[0]; return 1; }
	if ((p[0] & 0xE0) == 0xC0) { c = p[0] & 0x1F; need = 2; }
	else if ((p[0] & 0xF0) == 0xE0) { c = p[0] & 0x0F; need = 3; }
	else if ((p[0] & 0xF8) == 0xF0) { c = p[0] & 0x07; need = 4; }
	else return (size_t)-1;
	if (need > n) return (size_t)-2;
	for (size_t i = 1; i < need; i++) {
		if ((p[i] & 0xC0) != 0x80) return (size_t)-1;
		c = (c << 6) | (p[i] & 0x3F);
	}
	*out = c;
	return need;
}
static size_t utf8_encode(wchar_t c, char *out)
{
	unsigned long u = (unsigned long)c;
	if (u < 0x80) { out[0] = (char)u; return 1; }
	if (u < 0x800) { out[0] = (char)(0xC0 | (u >> 6)); out[1] = (char)(0x80 | (u & 0x3F)); return 2; }
	if (u >= 0xD800 && u <= 0xDFFF) return (size_t)-1;
	if (u < 0x10000) { out[0] = (char)(0xE0 | (u >> 12)); out[1] = (char)(0x80 | ((u >> 6) & 0x3F)); out[2] = (char)(0x80 | (u & 0x3F)); return 3; }
	if (u < 0x110000) { out[0] = (char)(0xF0 | (u >> 18)); out[1] = (char)(0x80 | ((u >> 12) & 0x3F)); out[2] = (char)(0x80 | ((u >> 6) & 0x3F)); out[3] = (char)(0x80 | (u & 0x3F)); return 4; }
	return (size_t)-1;
}
size_t mbstowcs(wchar_t *restrict d, const char *restrict s, size_t n)
{
	size_t count = 0;
	while (*s && (!d || count < n)) {
		wchar_t c;
		size_t used = utf8_decode(s, 4, &c);
		if (used == (size_t)-1 || used == (size_t)-2) { errno = EILSEQ; return (size_t)-1; }
		if (d) d[count] = c;
		count++;
		s += used;
	}
	if (d && count < n) d[count] = 0;
	return count;
}
size_t wcstombs(char *restrict d, const wchar_t *restrict s, size_t n)
{
	size_t count = 0;
	for (; *s; s++) {
		char buf[4];
		size_t len = utf8_encode(*s, buf);
		if (len == (size_t)-1) { errno = EILSEQ; return (size_t)-1; }
		if (d) {
			if (count + len > n) return count;
			memcpy(d + count, buf, len);
		}
		count += len;
	}
	if (d && count < n) d[count] = 0;
	return count;
}
size_t mbrtowc(wchar_t *restrict d, const char *restrict s, size_t n, mbstate_t *restrict st)
{
	wchar_t c;
	size_t used;
	(void)st;
	if (!s) return 0;
	if (!n) return (size_t)-2;
	used = utf8_decode(s, n, &c);
	if (used == (size_t)-1) { errno = EILSEQ; return used; }
	if (used == (size_t)-2) return used;
	if (d) *d = c;
	return c ? used : 0;
}
size_t wcrtomb(char *restrict s, wchar_t c, mbstate_t *restrict st)
{
	char buf[4];
	size_t len;
	(void)st;
	if (!s) return 1;
	len = utf8_encode(c, buf);
	if (len == (size_t)-1) { errno = EILSEQ; return len; }
	memcpy(s, buf, len);
	return len;
}
