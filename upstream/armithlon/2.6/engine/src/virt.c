#include "qrxvirt.h"
#include "qrxmedia.h"
#include "qrx_bootshell.h"
#include "qrx_block.h"
#include "qrx_hostfs.h"
#include <string.h>

/* Device register accesses must be naturally aligned and exact-width. */
static bool valid_ram(const QRXVM *v, uint32_t addr, unsigned width) {
    if (width != 1 && width != 2 && width != 4) return false;
    return v && v->ram.data && (uint64_t)addr + width <= v->ram.size;
}
static bool io_overlaps(uint32_t addr, unsigned width) {
    return (uint64_t)addr < (uint64_t)QRX_IO_BASE + QRX_IO_LENGTH &&
           (uint64_t)addr + width > QRX_IO_BASE;
}
static bool io_range(uint32_t addr, unsigned width) {
    return addr >= QRX_IO_BASE && (uint64_t)addr + width <= (uint64_t)QRX_IO_BASE + QRX_IO_LENGTH;
}
void qrx_vm_init(QRXVM *v, void *ram_data, size_t ram_size) {
    if (!v) return;
    memset(v, 0, sizeof(*v));
    v->ram.data = ram_data;
    v->ram.size = ram_size;
}
void qrx_vm_destroy(QRXVM *v) {
    if (!v) return;
    qrx_jit_release(v);
}
void qrx_vm_set_host(QRXVM *v, QRXConsoleFn console, QRXClockFn clock_ms, void *ctx) {
    if (!v) return;
    v->console = console;
    v->clock_ms = clock_ms;
    v->host_ctx = ctx;
}
static bool read_ram(const QRXVM *v, uint32_t addr, unsigned width, uint32_t *out) {
    if (!out || !valid_ram(v, addr, width)) return false;
    uint32_t x = 0;
    for (unsigned i = 0; i < width; ++i) x = (x << 8) | v->ram.data[addr + i];
    *out = x;
    return true;
}
bool qrx_vm_read(QRXVM *v, uint32_t addr, unsigned width, uint32_t *out) {
    if (!out) return false;
    if (!io_overlaps(addr,width)) return read_ram(v,addr,width,out);
    if (!io_range(addr,width)) return false;
    if (addr & (width-1)) return false;
    if (v->system && addr >= QRX_BOOT_RX && addr < QRX_BOOT_EXEC+4u)
        return qrx_shell_dev_read(v,addr,width,out);
    if (width == 4) {
        switch (addr - QRX_IO_BASE) {
            case 0x00: *out = QRX_IO_MAGIC; return true;
            case 0x04: *out = QRX_IO_VERSION; return true;
            case 0x08: *out = QRX_FEATURE_UART|QRX_FEATURE_TIMER|QRX_FEATURE_SHUTDOWN|QRX_FEATURE_RAMSIZE |
                           (v->block ? QRX_FEATURE_BLOCK : 0) |
                           (v->hostfs ? QRX_FEATURE_HOSTFS : 0) |
                           (v->media ? QRX_MEDIA_FEATURE_GFX|QRX_MEDIA_FEATURE_AUDIO|QRX_MEDIA_FEATURE_INPUT|QRX_MEDIA_FEATURE_GPU2D|QRX_MEDIA_FEATURE_3D : 0); return true;
            case 0x10: *out = v->clock_ms ? v->clock_ms(v->host_ctx) : 0; return true;
            case 0x14: *out = QRX_VCPU_ONLINE; return true;
            case 0x18: *out = QRX_VCPU_CAPACITY; return true;
            case 0x34: *out = (uint32_t)v->ram.size; return true;
            case 0x38: *out = (uint32_t)((uint64_t)v->ram.size>>32); return true;
            case 0x3c: *out = QRX_IO_BASE; return true;
        }
    }
    if (addr >= QRX_HOSTFS_BASE) return qrx_hostfs_read(v,addr,width,out);
    if (addr >= QRX_BLOCK_BASE) return qrx_block_read(v,addr,width,out);
    if (addr >= QRX_3D_BASE) return qrx_glide_read(v,addr,width,out);
    if (addr >= QRX_GPU_BASE) return qrx_gpu_read(v,addr,width,out);
    if (addr >= QRX_IO_BASE+0x100) return qrx_media_read(v,addr,width,out);
    return false;
}
bool qrx_vm_write(QRXVM *v, uint32_t addr, unsigned width, uint32_t value) {
    if (!v) return false;
    if (!io_overlaps(addr,width) && valid_ram(v, addr, width)) {
        for (unsigned i=0; i<width; ++i) v->ram.data[addr+i] = (uint8_t)(value >> (8u*(width-i-1u)));
        v->ram_write_epoch++; /* correctness over speed until write-page provenance exists */
        return true;
    }
    if (!io_range(addr, width) || (addr & (width-1))) return false;
    if (v->system && addr >= QRX_BOOT_RX && addr < QRX_BOOT_EXEC+4u)
        return qrx_shell_dev_write(v,addr,width,value);
    if (addr == QRX_IO_BASE+0x0c && width == 1) {
        if (v->console) v->console(v->host_ctx,(uint8_t)value);
        return true;
    }
    if (addr == QRX_IO_BASE+0x1c && width == 4) {
        if (value == 1) { v->cpu.halted = 1; return true; }
    }
    if (addr >= QRX_HOSTFS_BASE) return qrx_hostfs_write(v,addr,width,value);
    if (addr >= QRX_BLOCK_BASE) return qrx_block_write(v,addr,width,value);
    if (addr >= QRX_3D_BASE) return qrx_glide_write(v,addr,width,value);
    if (addr >= QRX_GPU_BASE) return qrx_gpu_write(v,addr,width,value);
    if (addr >= QRX_IO_BASE+0x100) return qrx_media_write(v,addr,width,value);
    return false;
}
bool qrx_vm_reset(QRXVM *v) {
    uint32_t sp=0,pc=0;
    if (!v || !read_ram(v,0,4,&sp) || !read_ram(v,4,4,&pc)) return false;
    if ((sp&1u) || (pc&1u) || sp > v->ram.size || pc >= v->ram.size) return false;
    qrx_jit_release(v);
    bool jit_enabled=v->jit_enabled;
    QRXRAM r = v->ram;
    QRXConsoleFn console = v->console;
    QRXClockFn clock = v->clock_ms;
    void *ctx = v->host_ctx;
    struct QRXMediaDevice *media = v->media;
    struct QRXBlock *block = v->block;
    struct QRXHostFS *hostfs = v->hostfs;
    void (*dma_write)(void *,uint32_t,unsigned)=v->dma_ram_write;
    void *dma_opaque=v->dma_opaque;
    struct QRXShell *system = v->system;
    qrx_vm_init(v,r.data,r.size);
    qrx_vm_set_host(v,console,clock,ctx);
    qrx_vm_bind_media(v,media);
    qrx_block_attach(v,block);
    qrx_hostfs_attach(v,hostfs);
    v->dma_ram_write=dma_write;v->dma_opaque=dma_opaque;
    v->system=system;
    v->jit_enabled=jit_enabled;
    v->cpu.pc = pc;
    v->cpu.a[7] = sp;
    v->cpu.ssp = sp;
    v->cpu.sr = 0x2700; /* supervisor, IPL 7 for reset */
    return true;
}
/* simplified 6-byte exception frame: SR at SP, PC at SP+2. */
bool qrx_vm_exception(QRXVM *v, unsigned vector, uint32_t return_pc) {
    if (!v || vector >= 256) return false;
    uint32_t target=0;
    uint32_t addr = v->cpu.vbr + 4u*vector;
    v->last_exception = vector;
    if ((addr&3u) || !read_ram(v,addr,4,&target) || !target || (target&1u)) goto error;
    uint32_t sp = v->cpu.a[7];
    if (!(v->cpu.sr & QRX_SR_S)) sp = v->cpu.ssp;
    if (sp < 6 || sp > v->ram.size) goto error;
    const uint32_t new_sp=sp-6;
    if (!qrx_vm_write(v,new_sp,2,v->cpu.sr) || !qrx_vm_write(v,new_sp+2,4,return_pc)) goto error;
    if (!(v->cpu.sr & QRX_SR_S)) v->cpu.usp=v->cpu.a[7];
    v->cpu.ssp=v->cpu.a[7]=new_sp;
    v->cpu.sr |= QRX_SR_S;
    v->cpu.pc=target;
    return true;
error:
    v->cpu.halted=1;
    v->cpu.fault=(uint8_t)vector;
    return false;
}
