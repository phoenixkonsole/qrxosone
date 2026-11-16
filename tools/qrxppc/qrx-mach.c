/* Inspect or run a restricted static PPC32 Mach-O executable. */
#include "qrx_macho.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_GUEST_MEMORY (128u * 1024u * 1024u)
#define STACK_SIZE (64u * 1024u)
#define RUN_BUDGET 1000000u

static int write_output(void *ctx, uint32_t fd, const uint8_t *bytes, uint32_t len)
{
    FILE *out = fd == 1u ? stdout : stderr;
    (void)ctx;
    return fwrite(bytes, 1u, len, out) == len ? 0 : -1;
}

int main(int argc, char **argv)
{
    FILE *file;
    long length;
    uint8_t *data;
    qrx_macho_t image;
    qrx_macho_status_t status;
    uint32_t i;
    int map = argc == 3 && strcmp(argv[1], "--map") == 0;
    int run = argc == 3 && strcmp(argv[1], "--run") == 0;
    const char *path;

    if (argc != 2 && !map && !run) {
        fprintf(stderr, "usage: %s [--map|--run] <Mach-O-or-universal-binary>\n", argv[0]);
        return 2;
    }
    path = argv[(map || run) ? 2 : 1];
    file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        (unsigned long)length > 0xffffffffUL || fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot determine image size\n");
        fclose(file);
        return 1;
    }
    data = (uint8_t *)malloc(length ? (size_t)length : 1u);
    if (!data) {
        fprintf(stderr, "cannot allocate image buffer\n");
        fclose(file);
        return 1;
    }
    if (fread(data, 1u, (size_t)length, file) != (size_t)length) {
        fprintf(stderr, "cannot read image\n");
        free(data);
        fclose(file);
        return 1;
    }
    fclose(file);
    status = qrx_macho_parse(&image, data, (uint32_t)length);
    if (status != QRX_MACHO_OK) {
        fprintf(stderr, "Mach-O rejected: %s\n", qrx_macho_status_string(status));
        free(data);
        return 1;
    }
    printf("PPC32 slice: offset 0x%08x, size %u, subtype %u, flags 0x%08x\n",
           (unsigned)image.slice_offset, (unsigned)image.slice_size,
           (unsigned)image.cpu_subtype, (unsigned)image.flags);
    printf("LC_UNIXTHREAD entry: 0x%08x; dyld: %s; dylib commands: %u\n",
           (unsigned)image.entry, image.has_dylinker ? "yes" : "no",
           (unsigned)image.dylib_count);
    for (i = 0; i < image.segment_count; i++) {
        const qrx_macho_segment_t *s = &image.segments[i];
        printf("%-16s vm=0x%08x+0x%x file=0x%08x+0x%x prot=%u/%u\n",
               s->name, (unsigned)s->vmaddr, (unsigned)s->vmsize,
               (unsigned)s->fileoff, (unsigned)s->filesize,
               (unsigned)s->initprot, (unsigned)s->maxprot);
    }
    if (map || run) {
        uint64_t required = 0u;
        uint8_t *memory;
        for (i = 0; i < image.segment_count; i++) {
            const qrx_macho_segment_t *s = &image.segments[i];
            uint64_t end;
            if (s->filesize == 0u && s->maxprot == 0u && s->initprot == 0u)
                continue;
            end = (uint64_t)s->vmaddr + s->vmsize;
            if (end > required)
                required = end;
        }
        if (run && !image.static_start_supported) {
            fprintf(stderr, "process stopped: unsupported dynamic or non-synthetic image\n");
            free(data);
            return 1;
        }
        if (run)
            required = ((required + 15u) & ~(uint64_t)15u) + STACK_SIZE;
        if (required == 0u || required > MAX_GUEST_MEMORY) {
            fprintf(stderr, "mapping exceeds prototype guest memory limit (%u bytes)\n",
                    (unsigned)MAX_GUEST_MEMORY);
            free(data);
            return 1;
        }
        memory = (uint8_t *)calloc(1u, (size_t)required);
        if (!memory) {
            fprintf(stderr, "cannot allocate guest memory\n");
            free(data);
            return 1;
        }
        if (run) {
            qrx_macho_run_result_t result;
            qrx_macho_run_status_t rs = qrx_macho_run(
                &image, memory, (uint32_t)required,
                (uint32_t)required - STACK_SIZE, RUN_BUDGET,
                write_output, NULL, &result);
            free(memory);
            if (rs != QRX_MACHO_RUN_EXITED) {
                fprintf(stderr, "process stopped: %s (pc=0x%08x, fault=0x%08x, syscall=%u)\n",
                        qrx_macho_run_status_string(rs), (unsigned)result.pc,
                        (unsigned)result.fault_addr, (unsigned)result.syscall);
                free(data);
                return 1;
            }
            printf("process exited: %u (%u instructions)\n",
                   (unsigned)result.exit_code, (unsigned)result.instructions);
            free(data);
            return (int)result.exit_code;
        }
        status = qrx_macho_map(&image, memory, (uint32_t)required);
        free(memory);
        if (status != QRX_MACHO_OK) {
            fprintf(stderr, "mapping failed: %s\n", qrx_macho_status_string(status));
            free(data);
            return 1;
        }
        printf("Mapped segments in %u bytes of temporary guest memory.\n",
               (unsigned)required);
    }
    puts("No dyld, general Darwin ABI, or relocation.");
    free(data);
    return 0;
}
