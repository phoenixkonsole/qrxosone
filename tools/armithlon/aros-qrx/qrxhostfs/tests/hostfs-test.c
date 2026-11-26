#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#define LONGNAME "a-file-name-much-longer-than-thirty-characters-0123456789abcdef.sqlite-journal"
static int fails;
#define CHECK(c, ...) do { if (c) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)
int main(void) {
    struct stat st;
    CHECK(stat("/Host/from-host.txt", &st) == 0 && st.st_size == 12, "stat host file (size %ld)", (long)st.st_size);
    char buf[64] = {0};
    int fd = open("/Host/FROM-HOST.TXT", O_RDONLY);
    CHECK(fd >= 0 && read(fd, buf, sizeof buf) == 12 && !strcmp(buf, "hello world\n"), "read, name in another case");
    close(fd);
    CHECK(mkdir("/Host/newdir", 0755) == 0, "mkdir");
    CHECK(mkdir("/Host/newdir/" LONGNAME ".d", 0755) == 0, "mkdir long name (%d chars)", (int)strlen(LONGNAME ".d"));
    /* 3 MB file, written in pieces, read back */
    size_t n = 3 * 1024 * 1024 + 123;
    unsigned char *w = malloc(n), *r = malloc(n);
    for (size_t i = 0; i < n; i++) w[i] = (unsigned char)(i * 7 + (i >> 11));
    FILE *f = fopen("/Host/newdir/" LONGNAME, "wb");
    CHECK(f && fwrite(w, 1, n, f) == n && fclose(f) == 0, "write 3 MB to a long name");
    CHECK(stat("/Host/newdir/" LONGNAME, &st) == 0 && (size_t)st.st_size == n, "size after write: %ld", (long)st.st_size);
    fd = open("/Host/newdir/" LONGNAME, O_RDONLY);
    ssize_t got = 0, k;
    while (fd >= 0 && (k = read(fd, r + got, n - got)) > 0) got += k;
    CHECK((size_t)got == n && !memcmp(w, r, n), "read back 3 MB, same bytes");
    CHECK(lseek(fd, 1000000, SEEK_SET) == 1000000 && read(fd, buf, 8) == 8 && !memcmp(buf, w + 1000000, 8), "seek + read");
    close(fd);
    fd = open("/Host/newdir/" LONGNAME, O_RDWR);
    CHECK(fd >= 0 && lseek(fd, 10, SEEK_SET) == 10 && write(fd, "XYZ", 3) == 3, "overwrite in the middle");
    CHECK(ftruncate(fd, 100) == 0, "ftruncate to 100");
    close(fd);
    CHECK(stat("/Host/newdir/" LONGNAME, &st) == 0 && st.st_size == 100, "size 100");
    CHECK(rename("/Host/newdir/" LONGNAME, "/Host/newdir/renamed.bin") == 0, "rename");
    CHECK(stat("/Host/newdir/" LONGNAME, &st) != 0 && stat("/Host/newdir/renamed.bin", &st) == 0, "old name gone, new there");
    f = fopen("/Host/newdir/b.txt", "w"); fputs("bbb", f); fclose(f);
    CHECK(rename("/Host/newdir/b.txt", "/Host/newdir/renamed.bin") == 0 && stat("/Host/newdir/renamed.bin", &st) == 0 && st.st_size == 3, "rename over an existing file");
    DIR *d = opendir("/Host/newdir");
    int entries = 0, saw_long = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) { entries++; if (strstr(e->d_name, "thirty-characters")) saw_long = 1; printf("     entry %s\n", e->d_name); }
    if (d) closedir(d);
    CHECK(saw_long && entries >= 4, "readdir (%d entries with . and ..)", entries);
    CHECK(rmdir("/Host/newdir/" LONGNAME ".d") == 0, "rmdir");
    CHECK(unlink("/Host/newdir/renamed.bin") == 0 && stat("/Host/newdir/renamed.bin", &st) != 0, "unlink");
    CHECK(rmdir("/Host/newdir") == 0 && stat("/Host/newdir", &st) != 0, "rmdir of the emptied directory");
    f = fopen("/Host/written-by-guest.txt", "w"); fprintf(f, "from QRX OS One 64\n"); fclose(f);
    CHECK(stat("/Host/../etc/passwd", &st) != 0, "no way out with ..");
    printf("%s: %d failures\n", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
