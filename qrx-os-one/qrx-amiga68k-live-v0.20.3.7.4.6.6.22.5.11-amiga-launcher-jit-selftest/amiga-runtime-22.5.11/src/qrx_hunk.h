/* QRX OS One 22.5.11 - portable classic Amiga HUNK / executable-format analysis.
 *
 * This header is shared by the native AROS m68k tools (QRXAmigaRun / amigacheck)
 * and the host regression tests.  It contains NO AmigaOS API calls: everything
 * here operates on a byte buffer so it can be proven on the host and reused
 * unchanged on the target.  It identifies and structurally validates a file;
 * it never claims that a program will work - only AROS' own LoadSeg() and the
 * actual run decide that.
 */
#ifndef QRX_HUNK_H
#define QRX_HUNK_H

#include <stddef.h>

typedef unsigned long qrx_u32;   /* >= 32 bit on every supported compiler */

#define QRX_HUNK_MAX_HUNKS   256
#define QRX_HUNK_MAX_REFS     32
#define QRX_HUNK_NAME_LEN     48
#define QRX_HUNK_MAX_WARN      6
#define QRX_HUNK_TEXT        160

/* Classic AmigaDOS hunk type codes (dos/doshunks.h). */
#define QRX_HUNK_UNIT         0x3E7UL
#define QRX_HUNK_NAME         0x3E8UL
#define QRX_HUNK_CODE         0x3E9UL
#define QRX_HUNK_DATA         0x3EAUL
#define QRX_HUNK_BSS          0x3EBUL
#define QRX_HUNK_RELOC32      0x3ECUL
#define QRX_HUNK_RELOC16      0x3EDUL
#define QRX_HUNK_RELOC8       0x3EEUL
#define QRX_HUNK_EXT          0x3EFUL
#define QRX_HUNK_SYMBOL       0x3F0UL
#define QRX_HUNK_DEBUG        0x3F1UL
#define QRX_HUNK_END          0x3F2UL
#define QRX_HUNK_HEADER       0x3F3UL
#define QRX_HUNK_OVERLAY      0x3F5UL
#define QRX_HUNK_BREAK        0x3F6UL
#define QRX_HUNK_DREL32       0x3F7UL   /* in executables: RELOC32SHORT (V37+) */
#define QRX_HUNK_DREL16       0x3F8UL
#define QRX_HUNK_DREL8        0x3F9UL
#define QRX_HUNK_LIB          0x3FAUL
#define QRX_HUNK_INDEX        0x3FBUL
#define QRX_HUNK_RELOC32SHORT 0x3FCUL
#define QRX_HUNK_RELRELOC32   0x3FDUL
#define QRX_HUNK_ABSRELOC16   0x3FEUL

enum qrx_bin_kind {
    QRX_BIN_UNKNOWN = 0,
    QRX_BIN_EMPTY,
    QRX_BIN_AMIGA_HUNK_EXE,     /* AmigaOS 1.x-3.x / AROS m68k load file */
    QRX_BIN_AMIGA_HUNK_OBJECT,  /* HUNK_UNIT object file - link it first */
    QRX_BIN_AMIGA_HUNK_LIBRARY, /* HUNK_LIB linker library */
    QRX_BIN_ELF_M68K_AROS,      /* relocatable ELF/m68k: AROS native executable */
    QRX_BIN_ELF_M68K_LINUX,     /* ET_EXEC/ET_DYN ELF/m68k: Linux/m68k program */
    QRX_BIN_ELF_PPC,            /* PowerPC ELF: AmigaOS 4 / MorphOS / Linux-PPC */
    QRX_BIN_ELF_OTHER,
    QRX_BIN_PE_MZ,              /* DOS/Windows MZ/PE image */
    QRX_BIN_SCRIPT              /* text: AmigaDOS/ARexx script */
};

enum qrx_ref_state { QRX_REF_UNCHECKED = 0, QRX_REF_RESIDENT, QRX_REF_FILE, QRX_REF_MISSING };

struct qrx_hunk_info {
    qrx_u32 type;          /* CODE/DATA/BSS */
    qrx_u32 alloc_bytes;   /* from the header size table */
    qrx_u32 file_bytes;    /* payload bytes present in the file (0 for BSS) */
    qrx_u32 file_offset;   /* payload offset inside the file */
    qrx_u32 memflags;      /* 0 any, 1 chip, 2 fast, 3 extended attribute */
    qrx_u32 relocs;        /* relocation entries pointing INTO other hunks */
};

struct qrx_ref {
    char name[QRX_HUNK_NAME_LEN];
    int is_device;
    int state;             /* enum qrx_ref_state, filled by the target probe */
    char where[QRX_HUNK_NAME_LEN * 2];
};

struct qrx_hunk_report {
    int kind;              /* enum qrx_bin_kind */
    int valid;             /* structure fully parsed and bounds-checked */
    int runnable;          /* format is something AROS LoadSeg can load */
    char error[QRX_HUNK_TEXT];
    qrx_u32 file_size;

    qrx_u32 hunk_count, first_hunk, last_hunk;
    struct qrx_hunk_info hunks[QRX_HUNK_MAX_HUNKS];
    qrx_u32 chip_bytes, fast_bytes, any_bytes;
    qrx_u32 reloc_total;
    int has_overlay, has_symbols, has_debug, resident_names;
    int first_is_code;

    unsigned elf_machine, elf_type;

    int warn_count;
    char warn[QRX_HUNK_MAX_WARN][QRX_HUNK_TEXT];

    int ref_count;
    struct qrx_ref refs[QRX_HUNK_MAX_REFS];
};

/* Analyse an in-memory file image.  Returns 1 when the image is a structurally
 * valid, LoadSeg-compatible program (HUNK executable or AROS m68k ELF). */
int qrx_hunk_analyze(const unsigned char *buf, qrx_u32 len, struct qrx_hunk_report *r);

const char *qrx_bin_kind_name(int kind);
const char *qrx_hunk_type_name(qrx_u32 type);

/* AmigaDOS argument quoting: wraps `in` in double quotes and escapes `*`, `"`
 * and newline with the BCPL `*` escape (`**`, `*"`, `*N`).  Returns 0 if the
 * output buffer is too small or the input contains a NUL-free control
 * character that cannot be passed safely (CR/ESC/other < 0x20 except TAB). */
int qrx_dos_quote(const char *in, char *out, size_t outsz);

/* Launcher command-line parsing.
 *   [options] <program> [program arguments...]
 * Options (before the program, case-insensitive): CHECK, -c, --check, ASYNC,
 * --async, QUIET, -q, --quiet, FORCE, --force, STRICT, --strict, NOLOAD,
 * --noload, STACK=<n>, --stack <n>, and "--" to end option parsing.  The
 * program may be AmigaDOS-quoted ("Work:My Game/run" with * escapes).  The
 * remaining text is passed to the program verbatim. */
struct qrx_launch_req {
    int check_only, async, quiet, force, strict, noload;
    unsigned long stack;
    char program[512];
    char args[1024];
    char error[QRX_HUNK_TEXT];
};
int qrx_parse_launch_line(const char *line, struct qrx_launch_req *q);

/* Build "<quoted program> <args>" for SystemTags().  Returns 0 on overflow. */
int qrx_build_command(const struct qrx_launch_req *q, char *out, size_t outsz);

/* Conventional AmigaDOS return-code class name (OK/WARN/ERROR/FAIL). */
const char *qrx_rc_class(long rc);

#endif
