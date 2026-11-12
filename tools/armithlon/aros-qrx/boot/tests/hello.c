/* A C program for 64-bit AROS on the 68100EC64: C library, soft float, files, arguments. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    long v[] = { 42, -7, 1000000000000L, 3, 0 };
    char line[128];
    double d = strtod("3.25", NULL) * 4.0;
    char *p = malloc(1 << 20);
    FILE *f;
    int i;

    printf("hello from 64-bit AROS, sizeof(long) %d, sizeof(void *) %d\n", (int)sizeof(long), (int)sizeof(void *));
    printf("argc %d:", argc);
    for (i = 0; i < argc; i++)
        printf(" [%s]", argv[i]);
    printf("\n");
    qsort(v, 5, sizeof(long), cmp);
    printf("sorted: %ld %ld %ld %ld %ld\n", v[0], v[1], v[2], v[3], v[4]);
    printf("double: %.3f, %g, int %d\n", d, d / 3.0, (int)(d * 10));
    if (!p) { printf("malloc failed\n"); return 20; }
    memset(p, 0x5A, 1 << 20);
    printf("malloc 1 MB at %p, last byte %02x\n", (void *)p, (unsigned char)p[(1 << 20) - 1]);
    free(p);

    f = fopen("SYS:hello.txt", "w");
    if (!f) { printf("fopen for writing failed\n"); return 20; }
    fprintf(f, "written by hello, %ld\n", v[4]);
    fclose(f);
    f = fopen("SYS:hello.txt", "r");
    if (!f || !fgets(line, sizeof line, f)) { printf("reading back failed\n"); return 20; }
    fclose(f);
    printf("read back: %s", line);
    return 0;
}
