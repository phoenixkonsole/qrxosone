/* With -fstack-protector-strong the return block is split off after the guard check. A pointer
 * result must still be in both a0 and d0 at the rts (LowerReturn's dual return): the copy into the
 * second register was lost once (SpiderMonkey's GC got back the zone pointer instead of a cell).
 * Checked by ssp-return.sh on the assembly. */
#include <stdint.h>
struct span { uint16_t first, last; };
void use(char *);
void *take(struct span *s, int c)
{
    char buf[32];
    use(buf);
    if (c) return 0;
    uintptr_t t = (uintptr_t)s + s->first;
    s->first += 8;
    return (void *)t;
}
