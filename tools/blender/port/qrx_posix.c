/* POSIX pieces Blender wants and the AROS SDK does not have. Linked into every 68k program of
 * the build by aros_cc.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fnmatch.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dosextens.h>
#include <dos/var.h>

/* No virtual memory: a mapping is a private copy, with its length in front. */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	char *mem = malloc(len + 16);
	(void)addr; (void)prot;
	if (!mem) { errno = ENOMEM; return MAP_FAILED; }
	mem += 16;
	if (flags & MAP_ANON) {
		memset(mem, 0, len);
	}
	else {
		off_t was = lseek(fd, 0, SEEK_CUR);
		size_t got = 0;
		lseek(fd, off, SEEK_SET);
		while (got < len) {
			int n = read(fd, mem + got, len - got);
			if (n <= 0) break;
			got += n;
		}
		lseek(fd, was, SEEK_SET);
		if (got < len) memset(mem + got, 0, len - got);
	}
	return mem;
}
int munmap(void *addr, size_t len)
{
	(void)len;
	free((char *)addr - 16);
	return 0;
}

static int match(const char *p, const char *s, int fold)
{
	for (;; p++, s++) {
		char c = *p;
		if (c == '*') {
			while (*p == '*') p++;
			if (!*p) return 1;
			for (; *s; s++) if (match(p, s, fold)) return 1;
			return 0;
		}
		if (!c) return !*s;
		if (!*s) return 0;
		if (c == '?') continue;
		if (c == '[') {
			const char *q = p + 1;
			int neg = (*q == '!' || *q == '^'), hit = 0;
			if (neg) q++;
			for (; *q && *q != ']'; q++) {
				int a = fold ? tolower((unsigned char)*q) : *q, b = a;
				int t = fold ? tolower((unsigned char)*s) : *s;
				if (q[1] == '-' && q[2] && q[2] != ']') { b = fold ? tolower((unsigned char)q[2]) : q[2]; q += 2; }
				if (t >= a && t <= b) hit = 1;
			}
			if (!*q || hit == neg) return 0;
			p = q;
			continue;
		}
		if (fold ? tolower((unsigned char)c) != tolower((unsigned char)*s) : c != *s) return 0;
	}
}
int fnmatch(const char *pattern, const char *s, int flags)
{
	return match(pattern, s, flags & FNM_CASEFOLD) ? 0 : FNM_NOMATCH;
}

char *mkdtemp(char *tmpl)
{
	size_t len = strlen(tmpl);
	static unsigned counter = 1;
	if (len < 6) { errno = EINVAL; return NULL; }
	for (int tries = 0; tries < 1000; tries++, counter++) {
		unsigned v = counter * 2654435761u;
		for (int i = 1; i <= 6; i++, v >>= 4) tmpl[len - i] = "abcdefghijklmnop"[v & 15];
		if (mkdir(tmpl, 0700) == 0) return tmpl;
		if (errno != EEXIST) break;
	}
	return NULL;
}

int setenv(const char *name, const char *value, int overwrite)
{
	if (!overwrite && getenv(name)) return 0;
	return SetVar(name, value, -1, GVF_LOCAL_ONLY) ? 0 : -1;
}
int unsetenv(const char *name)
{
	DeleteVar(name, GVF_LOCAL_ONLY);
	return 0;
}

/* environ, defined here so the C library's own one is never linked. Referencing environ pulls in
 * a start-up function that asks the C library to build the array, and the library counts the
 * shell's local variables with a ScanVars hook that takes its arguments from the wrong place: it
 * adds to the variable nodes themselves. The shell outlives the program, so every later program
 * of that shell found broken variables, and CreateNewProc, which copies them, failed: pthreads,
 * so Blender's render threads, did not start after Python or Blender had run once. Same contents
 * as the library's: the shell's local variables, name=value. */
char **environ;
static void __attribute__((constructor)) qrx_environ_init(void)
{
	static char *none[1];
	struct Process *me = (struct Process *)FindTask(NULL);
	struct LocalVar *lv;
	size_t n = 0;

	environ = none;
	if (me->pr_Task.tc_Node.ln_Type != NT_PROCESS) return;
	for (lv = (struct LocalVar *)me->pr_LocalVars.mlh_Head; lv->lv_Node.ln_Succ; lv = (struct LocalVar *)lv->lv_Node.ln_Succ)
		if (lv->lv_Node.ln_Type == LV_VAR && !(lv->lv_Flags & GVF_BINARY_VAR)) n++;
	char **env = malloc((n + 1) * sizeof *env);
	if (!env) return;
	size_t i = 0;
	for (lv = (struct LocalVar *)me->pr_LocalVars.mlh_Head; lv->lv_Node.ln_Succ && i < n; lv = (struct LocalVar *)lv->lv_Node.ln_Succ) {
		if (lv->lv_Node.ln_Type != LV_VAR || (lv->lv_Flags & GVF_BINARY_VAR)) continue;
		size_t nl = strlen(lv->lv_Node.ln_Name), vl = lv->lv_Len;
		/* The shell may keep a trailing newline in the value, as GetVar strips it. */
		while (vl && (lv->lv_Value[vl - 1] == '\n' || lv->lv_Value[vl - 1] == 0)) vl--;
		char *e = malloc(nl + vl + 2);
		if (!e) break;
		memcpy(e, lv->lv_Node.ln_Name, nl);
		e[nl] = '=';
		memcpy(e + nl + 1, lv->lv_Value, vl);
		e[nl + 1 + vl] = 0;
		env[i++] = e;
	}
	env[i] = NULL;
	environ = env;
}

/* Without a screen the shell has no console for stderr, so a batch job can ask for it in stdout. */
static void __attribute__((constructor)) qrx_stderr_to_stdout(void)
{
	/* No "insert volume" requesters: with no screen they wait forever, and Blender probes paths. */
	((struct Process *)FindTask(NULL))->pr_WindowPtr = (APTR)-1;
	if (getenv("QRX_STDERR_TO_STDOUT")) {
		extern void qrx_print_base(void);
		dup2(1, 2);
		qrx_print_base();
	}
}

#include <proto/exec.h>
/* One line to the engine's log, usable anywhere on core 0: for debugging a guest with no screen. */
void qrx_log(const char *s)
{
	while (*s) RawPutChar(*s++);
	RawPutChar('\n');
}

#include <stdarg.h>
/* printf to the engine's log, for debugging a guest with no screen. */
int qrx_printf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	qrx_log(buf);
	return n;
}
void abort(void)
{
	qrx_log("abort() called");
	exit(20);
}

/* The C++ the build keeps uses only these from a C++ runtime. */
void *_Znwm(unsigned long n) { return malloc(n); }
void *_Znam(unsigned long n) { return malloc(n); }
void _ZdlPv(void *p) { free(p); }
void _ZdaPv(void *p) { free(p); }
void _ZdlPvm(void *p, unsigned long n) { (void)n; free(p); }
void _ZdaPvm(void *p, unsigned long n) { (void)n; free(p); }
void __cxa_pure_virtual(void) { qrx_log("pure virtual call"); exit(20); }

/* What the first stage leaves out ends the program with its name, see gen_missing.py. */
void qrx_missing(const char *name)
{
	fprintf(stderr, "Blender on QRX OS One: %s is not part of this build\n", name);
	qrx_printf("missing function called: %s", name);
	exit(20);
}

/* For guest_stack.py: where the program was loaded. */
extern int main(int, char **);
void qrx_print_base(void)
{
	qrx_printf("main addr %p", (void *)main);
}

/* What libstdc++ wants from a C library and the SDK does not have. */
#include <sys/uio.h>
void *__dso_handle = NULL;
static struct { void (*fn)(void *); void *arg; } exit_list[128];
static int exit_count;
static void run_exit_list(void)
{
	while (exit_count > 0) {
		exit_count--;
		exit_list[exit_count].fn(exit_list[exit_count].arg);
	}
}
int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
	(void)dso;
	if (exit_count == 128) return -1;
	if (exit_count == 0) atexit(run_exit_list);
	exit_list[exit_count].fn = fn;
	exit_list[exit_count++].arg = arg;
	return 0;
}
ssize_t writev(int fd, const struct iovec *iov, int count)
{
	ssize_t total = 0;
	for (int i = 0; i < count; i++) {
		ssize_t n = write(fd, iov[i].iov_base, iov[i].iov_len);
		if (n < 0) return total ? total : n;
		total += n;
		if ((size_t)n < iov[i].iov_len) break;
	}
	return total;
}
/* libstdc++ refers to the transactional memory runtime weakly, and collect-aros refuses any undefined
 * symbol. Nothing here uses transactions. */
void _ITM_RU1(void) { qrx_missing("_ITM_RU1"); }
void _ITM_RU4(void) { qrx_missing("_ITM_RU4"); }
void _ITM_addUserCommitAction(void) { qrx_missing("_ITM_addUserCommitAction"); }
void _ITM_memcpyRnWt(void) { qrx_missing("_ITM_memcpyRnWt"); }
void _ITM_memcpyRtWn(void) { qrx_missing("_ITM_memcpyRtWn"); }
void _ZGTtdlPv(void) { qrx_missing("transaction delete"); }
void _ZGTtnam(void) { qrx_missing("transaction new"); }
void _ZGTtnaX(void) { qrx_missing("transaction new"); }

/* AROS's startup registers the unwind tables, and libgcc's version allocates memory. Inside the startup
 * code AvailMem did not come back. Programs here have no exceptions: a throw finds no table and terminates.
 * This replaces all of libgcc's unwind-dw2-fde, so that member is never linked. */
void __register_frame(void *begin) { (void)begin; }
void *__deregister_frame(void *begin) { (void)begin; return NULL; }
void __register_frame_info(void *begin, void *ob) { (void)begin; (void)ob; }
void __register_frame_info_bases(void *begin, void *ob, void *t, void *d) { (void)begin; (void)ob; (void)t; (void)d; }
void __register_frame_table(void *begin) { (void)begin; }
void __register_frame_info_table(void *begin, void *ob) { (void)begin; (void)ob; }
void __register_frame_info_table_bases(void *begin, void *ob, void *t, void *d) { (void)begin; (void)ob; (void)t; (void)d; }
void *__deregister_frame_info(void *begin) { (void)begin; return NULL; }
void *__deregister_frame_info_bases(void *begin) { (void)begin; return NULL; }
void *_Unwind_Find_FDE(void *pc, void *bases) { (void)pc; (void)bases; return NULL; }

/* FLT_ROUNDS in the SDK's float.h: round to nearest is all that runs here. */
int __flt_rounds(void) { return 1; }
