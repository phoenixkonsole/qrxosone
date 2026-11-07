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
