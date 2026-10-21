/* MONOX Kernel - GPU Driver (GPUDrive)
 *
 * Generic GPU kernel driver supporting:
 *   - ARM Mali (Bifrost/Valhall/5th-gen)
 *   - Qualcomm Adreno
 *   - PowerVR
 *   - Any GPU with DRM-compatible interface
 *
 * Responsibilities:
 *   - MMIO registration from DTB
 *   - GPU memory management (ZONE_GPU from PMM)
 *   - Command queue submission
 *   - Fence / sync primitives
 *   - OpenGL & Vulkan userspace ABI bridge
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "gpu_driver.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"

/* gpu_vendor_t is declared in gpu_driver.h (the duplicate enum that used
 * to be here was a redefinition error) */

/* -------------------------------------------------------
 * GPU memory object
 * -------------------------------------------------------*/
struct gpu_mem_obj {
    phys_addr_t  pa;
    u64          gpu_va;   /* GPU virtual address (IOVA) */
    u64          cpu_va;   /* kernel VA, 0 if not mapped */
    size_t       size;
    u32          order;
    u32          flags;
    u32          handle;
    bool         valid;
    struct list_head node;
};

/* -------------------------------------------------------
 * Command buffer
 * -------------------------------------------------------*/
#define CMD_BUF_SIZE (64 * 1024)   /* 64 KiB command buffer */

struct gpu_cmd_buf {
    u8          *data;
    size_t       used;
    size_t       capacity;
    u64          gpu_va;
    phys_addr_t  pa;
};

/* -------------------------------------------------------
 * Fence / synchronisation
 * -------------------------------------------------------*/
struct gpu_fence {
    atomic_t     signaled;
    u64          timeline_value;
    struct list_head waiters;
    spinlock_t   lock;
};

/* -------------------------------------------------------
 * GPU device
 * -------------------------------------------------------*/
#define MAX_GPU_CONTEXTS 64
#define MAX_GPU_MEM_OBJS 4096

struct gpu_device {
    /* Hardware */
    volatile u8  *mmio_base;    /* GPU MMIO VA                  */
    size_t        mmio_size;
    u32           irq;
    gpu_vendor_t  vendor;
    u32           gpu_id;       /* vendor-specific ID           */
    u32           num_cores;
    u32           shader_cores;
    u64           gpu_freq_hz;

    /* Memory */
    phys_addr_t   gpu_zone_base;/* from ZONE_GPU in PMM         */
    size_t        gpu_zone_size;
    u64           iova_next;    /* next GPU virtual address     */
    spinlock_t    mem_lock;
    struct list_head mem_list;
    u32           next_handle;

    /* Queues */
    struct gpu_cmd_buf  cmd_buf;
    spinlock_t          queue_lock;

    /* Fences */
    atomic64_t    timeline;
    struct gpu_fence fences[16];

    bool          initialized;
    char          name[64];
};

static struct gpu_device g_gpu;

/* -------------------------------------------------------
 * Mali-specific register offsets (read from DTB, not hardcoded values)
 * -------------------------------------------------------*/
#define MALI_GPU_ID       0x000
#define MALI_GPU_IRQ_RAWSTAT 0x020
#define MALI_GPU_IRQ_CLEAR   0x024
#define MALI_GPU_IRQ_MASK    0x028
#define MALI_GPU_IRQ_STATUS  0x02C
#define MALI_GPU_COMMAND     0x030
#define MALI_GPU_STATUS      0x034
#define MALI_GPU_SHADER_PRESENT 0x100
#define MALI_GPU_TEXTURE_FEATURES0 0x11C
#define MALI_L2_PRESENT    0x120
#define MALI_STACK_PRESENT 0x128

/* GPU command values */
#define MALI_GPU_CMD_SOFT_RESET   0x01
#define MALI_GPU_CMD_HARD_RESET   0x02
#define MALI_GPU_CMD_CACHE_CLN    0x04

/* -------------------------------------------------------
 * Adreno-specific registers
 * -------------------------------------------------------*/
#define ADRENO_RBBM_HW_VERSION    0x0000
#define ADRENO_RBBM_STATUS        0x04E0
#define ADRENO_RBBM_SW_RESET_CMD  0x0528
#define ADRENO_RBBM_BLOCK_SW_RESET_CMD 0x09C0

/* -------------------------------------------------------
 * MMIO helpers
 * -------------------------------------------------------*/
static __always_inline u32 gpu_read32(u32 off)
{
    return READ_ONCE(*(volatile u32 *)(g_gpu.mmio_base + off));
}

static __always_inline void gpu_write32(u32 off, u32 val)
{
    WRITE_ONCE(*(volatile u32 *)(g_gpu.mmio_base + off), val);
    dsb(sy);
}

/* -------------------------------------------------------
 * Vendor detection
 * -------------------------------------------------------*/
static gpu_vendor_t gpu_detect_vendor(const char *compatible)
{
    if (!compatible) return GPU_VENDOR_UNKNOWN;
    if (strncmp(compatible, "arm,mali",   8) == 0) return GPU_VENDOR_MALI;
    if (strncmp(compatible, "qcom,adreno",11) == 0) return GPU_VENDOR_ADRENO;
    if (strncmp(compatible, "img,powervr",11) == 0) return GPU_VENDOR_POWERVR;
    if (strncmp(compatible, "vivante,gc",  10) == 0) return GPU_VENDOR_VIVANTE;
    return GPU_VENDOR_UNKNOWN;
}

/* -------------------------------------------------------
 * GPU init - called with DTB-parsed values
 * -------------------------------------------------------*/
int gpu_init(phys_addr_t mmio_phys, size_t mmio_size,
             u32 irq, const char *compatible)
{
    /* a second init used to memset the live device away, leaking the MMIO
     * mapping and the command buffer */
    if (g_gpu.initialized) return MX_EBUSY;
    if (!mmio_phys) return MX_EINVAL;

    memset(&g_gpu, 0, sizeof(g_gpu));
    spin_lock_init(&g_gpu.mem_lock);
    spin_lock_init(&g_gpu.queue_lock);
    list_init(&g_gpu.mem_list);
    atomic64_set(&g_gpu.timeline, 0);

    /* Map GPU MMIO */
    u64 va = vmm_map_phys(mmio_phys, mmio_size, 0);
    if (!va) return MX_ENOMEM;

    g_gpu.mmio_base  = (volatile u8 *)va;
    g_gpu.mmio_size  = mmio_size;
    g_gpu.irq        = irq;
    g_gpu.vendor     = gpu_detect_vendor(compatible);
    g_gpu.next_handle= 1;
    g_gpu.iova_next  = 0x1000; /* start IOVA at 4 KiB */

    /* Claim GPU memory zone from PMM */
    g_gpu.gpu_zone_base = pmm_gpu_zone_base();
    g_gpu.gpu_zone_size = pmm_gpu_zone_size();

    /* Vendor-specific init */
    switch (g_gpu.vendor) {
    case GPU_VENDOR_MALI: {
        u32 gpu_id = gpu_read32(MALI_GPU_ID);
        g_gpu.gpu_id = gpu_id;
        strncpy(g_gpu.name, "Mali", 63);

        /* Count shader cores */
        u64 shader_present = ((u64)gpu_read32(MALI_GPU_SHADER_PRESENT + 4) << 32)
                           | gpu_read32(MALI_GPU_SHADER_PRESENT);
        g_gpu.shader_cores = 0;
        while (shader_present) {
            g_gpu.shader_cores += shader_present & 1;
            shader_present >>= 1;
        }

        /* Soft reset */
        gpu_write32(MALI_GPU_COMMAND, MALI_GPU_CMD_SOFT_RESET);
        u32 timeout = 10000;
        while ((gpu_read32(MALI_GPU_STATUS) & 0x01) && timeout--)
            cpu_relax();

        /* Enable interrupts */
        gpu_write32(MALI_GPU_IRQ_MASK, 0xFFFFFFFF);
        break;
    }
    case GPU_VENDOR_ADRENO: {
        u32 ver = gpu_read32(ADRENO_RBBM_HW_VERSION);
        g_gpu.gpu_id = ver;
        strncpy(g_gpu.name, "Adreno", 63);

        /* Soft reset */
        gpu_write32(ADRENO_RBBM_SW_RESET_CMD, 1);
        u32 timeout = 10000;
        while (gpu_read32(ADRENO_RBBM_SW_RESET_CMD) && timeout--)
            cpu_relax();
        break;
    }
    default:
        strncpy(g_gpu.name, "Unknown GPU", 63);
        break;
    }

    /* Allocate command buffer from GPU zone */
    phys_addr_t cmd_pa = pmm_alloc_pages(4, GFP_GPU | __GFP_ZERO);
    if (!cmd_pa) {
        vmm_unmap(va, mmio_size);            /* don't leak the MMIO window */
        g_gpu.mmio_base = NULL;
        return MX_ENOMEM;
    }

    g_gpu.cmd_buf.pa       = cmd_pa;
    g_gpu.cmd_buf.data     = (u8 *)phys_to_virt_pmm(cmd_pa);
    g_gpu.cmd_buf.capacity = CMD_BUF_SIZE;
    g_gpu.cmd_buf.used     = 0;
    g_gpu.cmd_buf.gpu_va   = g_gpu.iova_next;
    g_gpu.iova_next       += CMD_BUF_SIZE;

    /* Register IRQ */
    /* IRQ 0 = "no interrupt in the DTB" (it is also SGI 0, reserved for IPIs) */
    extern int irq_register(u32, void (*)(u32, void *), void *);
    if (irq) {
        int rr = irq_register(irq, gpu_irq_handler, &g_gpu);
        if (rr != MX_OK)
            pr_warn("GPU: cannot register IRQ %u (%d), running without interrupts\n", irq, rr);
    }

    g_gpu.initialized = true;

    pr_info("GPU: %s id=0x%08x shader_cores=%u mmio=0x%llx GPU_zone=%llu MiB\n",
            g_gpu.name, g_gpu.gpu_id, g_gpu.shader_cores,
            mmio_phys, g_gpu.gpu_zone_size / (1024 * 1024));

    return MX_OK;
}

/* -------------------------------------------------------
 * GPU memory allocation
 * -------------------------------------------------------*/
u32 gpu_mem_alloc(size_t size, u32 flags)
{
    if (!g_gpu.initialized || !size) return 0;

    size = ALIGN(size, 4096);
    u32 order = 0;
    size_t sz = 4096;
    while (sz < size && order < PMM_MAX_ORDER - 1) { sz <<= 1; order++; }
    if (sz < size) return 0;                   /* larger than the biggest block */

    gfp_t gfp = GFP_GPU | __GFP_ZERO;
    phys_addr_t pa = pmm_alloc_pages(order, gfp);
    if (!pa) return 0;

    struct gpu_mem_obj *obj = kzalloc(sizeof(*obj));
    if (!obj) { pmm_free_pages(pa, order); return 0; }

    u32 handle;
    spin_lock(&g_gpu.mem_lock);
    obj->pa      = pa;
    obj->gpu_va  = g_gpu.iova_next;
    g_gpu.iova_next = ALIGN(g_gpu.iova_next + sz, 4096);   /* the whole block: ranges must not overlap */
    obj->cpu_va  = (u64)phys_to_virt_pmm(pa);
    obj->size    = size;
    obj->order   = order;
    obj->flags   = flags;
    if (g_gpu.next_handle == 0) g_gpu.next_handle = 1;     /* 0 means failure */
    obj->handle  = g_gpu.next_handle++;
    obj->valid   = true;
    handle       = obj->handle;                            /* read BEFORE unlocking: */
    list_add_tail(&obj->node, &g_gpu.mem_list);            /* gpu_mem_free() may free obj */
    spin_unlock(&g_gpu.mem_lock);

    return handle;
}

/* -------------------------------------------------------
 * GPU memory free
 * -------------------------------------------------------*/
void gpu_mem_free(u32 handle)
{
    spin_lock(&g_gpu.mem_lock);
    struct list_head *pos, *n;
    list_for_each_safe(pos, n, &g_gpu.mem_list) {
        struct gpu_mem_obj *obj = list_entry(pos, struct gpu_mem_obj, node);
        if (obj->handle == handle) {
            list_del(pos);
            spin_unlock(&g_gpu.mem_lock);
            pmm_free_pages(obj->pa, obj->order);
            kfree(obj);
            return;
        }
    }
    spin_unlock(&g_gpu.mem_lock);
}

/* -------------------------------------------------------
 * GPU command submission (simplified ring model)
 * -------------------------------------------------------*/
int gpu_submit_commands(const u8 *cmds, size_t len)
{
    if (!g_gpu.initialized) return MX_ENODEV;
    if (!cmds || !len) return MX_EINVAL;
    /* A command larger than the whole buffer used to be memcpy'd past its
     * end (heap overflow) after the "reset" below. */
    if (len > g_gpu.cmd_buf.capacity) return MX_EINVAL;

    spin_lock(&g_gpu.queue_lock);

    if (g_gpu.cmd_buf.used + len > g_gpu.cmd_buf.capacity) {
        /* Wrap: the GPU must be done reading the old contents first */
        if (gpu_wait_idle(100) != MX_OK) {
            spin_unlock(&g_gpu.queue_lock);
            return MX_EBUSY;
        }
        g_gpu.cmd_buf.used = 0;
    }

    memcpy(g_gpu.cmd_buf.data + g_gpu.cmd_buf.used, cmds, len);
    /* make the commands visible to the (possibly non-coherent) GPU */
    cache_flush_range(g_gpu.cmd_buf.data + g_gpu.cmd_buf.used, len);
    g_gpu.cmd_buf.used += len;

    /* Ring doorbell (vendor-specific) */
    switch (g_gpu.vendor) {
    case GPU_VENDOR_MALI:
        /* was 0x06 = HARD_RESET|CACHE_CLN: it reset the GPU on every submit */
        gpu_write32(MALI_GPU_COMMAND, MALI_GPU_CMD_CACHE_CLN);
        break;
    case GPU_VENDOR_ADRENO:
        gpu_write32(0x0110, 1); /* CP_RB_WPTR increment */
        break;
    default:
        break;
    }

    spin_unlock(&g_gpu.queue_lock);
    return MX_OK;
}

/* -------------------------------------------------------
 * gpu_wait_idle
 * -------------------------------------------------------*/
int gpu_wait_idle(u32 timeout_ms)
{
    if (!g_gpu.initialized) return MX_ENODEV;
    u64 ticks = (u64)timeout_ms * 1000;      /* 64-bit: no wrap for large timeouts */
    while (ticks--) {
        u32 status;
        switch (g_gpu.vendor) {
        case GPU_VENDOR_MALI:
            status = gpu_read32(MALI_GPU_STATUS);
            if (!(status & 0x01)) return MX_OK;
            break;
        case GPU_VENDOR_ADRENO:
            status = gpu_read32(ADRENO_RBBM_STATUS);
            if (!(status & (1 << 31))) return MX_OK;
            break;
        default:
            return MX_OK;
        }
        cpu_relax();
    }
    return MX_ETIMEDOUT;
}

/* -------------------------------------------------------
 * IRQ handler
 * -------------------------------------------------------*/
void gpu_irq_handler(u32 irq, void *data)
{
    (void)irq;
    struct gpu_device *g = (struct gpu_device *)data;
    if (!g->initialized) return;

    switch (g->vendor) {
    case GPU_VENDOR_MALI: {
        u32 rawstat = gpu_read32(MALI_GPU_IRQ_RAWSTAT);
        gpu_write32(MALI_GPU_IRQ_CLEAR, rawstat);
        /* Signal timeline fence */
        atomic64_add(1, &g->timeline);
        break;
    }
    case GPU_VENDOR_ADRENO:
        /* Read and clear RBBM interrupt status */
        gpu_read32(0x0050); /* RBBM_INT_CLEAR_CMD */
        atomic64_add(1, &g->timeline);
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------
 * GPU info query
 * -------------------------------------------------------*/
void gpu_get_info(struct gpu_info *info)
{
    if (!info) return;
    info->vendor       = g_gpu.vendor;
    info->gpu_id       = g_gpu.gpu_id;
    info->shader_cores = g_gpu.shader_cores;
    info->gpu_freq_hz  = g_gpu.gpu_freq_hz;
    info->vram_total   = g_gpu.gpu_zone_size;
    info->initialized  = g_gpu.initialized;
    strncpy(info->name, g_gpu.name, 63);
}
