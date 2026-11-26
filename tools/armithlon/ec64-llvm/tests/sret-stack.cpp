// Struct return (sret) on m68k64: the caller's result buffer may sit at its own sp + 0, right above
// the return address its call pushes. A callee that pops part of the stack on return and moves its
// return address up there (the m68k 32-bit habit of popping the 4-byte sret pointer) overwrites
// the first half of the result. Seen in Gecko: nsDNSService::GetXPCOMSingleton, a lambda returning
// already_AddRefed into a local nsCOMPtr, got a pointer with a code address in its upper 32 bits.
#include <cstdio>
#include <cstdint>
struct Ref {                     // like already_AddRefed: non-trivial destructor, so returned via sret
    void *p;
    ~Ref() { if (p) puts("leak"); }
};
struct Holder {                  // like nsCOMPtr
    void *p;
    Holder(Ref &&r) : p(r.p) { r.p = nullptr; }
    ~Holder() {}
    Ref forget() { Ref r{p}; p = nullptr; return r; }
};
static int obj, inited;
__attribute__((noinline)) Ref single() { return Ref{&obj}; }
// internal and not inlined: LLVM gives it the fast calling convention, which passes the hidden
// pointer in a register; the caller's buffer is then at its sp + 0
static __attribute__((noinline)) Ref helper2() {
    if (inited > 5) return Ref{nullptr};
    return single();
}
__attribute__((noinline)) void *viahelper() {
    Holder h = helper2();
    return h.p;
}
__attribute__((noinline)) Ref getit() {
    auto helper = []() -> Ref {
        if (inited > 5) return Ref{nullptr};
        return single();
    };
    if (inited == 7) return helper();
    Holder h = helper();
    if (h.p) inited = 1;
    return h.forget();
}
struct Big { uint64_t a, b, c; };
__attribute__((noinline)) Big big(uint64_t x) { return Big{x, x + 1, x + 2}; }
__attribute__((noinline)) uint64_t usebig(uint64_t x) { Big b = big(x); return b.a ^ b.b ^ b.c; }
int main() {
    void *w = viahelper();
    printf("viahelper() = %p, want %p\n", w, (void *)&obj);
    Ref r = getit();
    void *v = r.p;
    r.p = nullptr;
    printf("getit() = %p, want %p\n", v, (void *)&obj);
    uint64_t u = usebig(0x123456789abcdef0ull);
    uint64_t want = 0x123456789abcdef0ull ^ 0x123456789abcdef1ull ^ 0x123456789abcdef2ull;
    printf("usebig = %llx, want %llx\n", (unsigned long long)u, (unsigned long long)want);
    puts(v == &obj && w == &obj && u == want ? "SRET PASS" : "SRET FAIL");
}
