/* MONOX Kernel - Graphics Support Layer
 *
 * Provides kernel-side infrastructure for:
 *   - OpenGL ES (latest, GPU-specific version queried at runtime)
 *   - Vulkan (1.3+, features depend on GPU capabilities)
 *
 * The kernel layer handles:
 *   - DRM (Direct Rendering Manager) device nodes
 *   - GEM buffer object management
 *   - Fence synchronisation
 *   - Display pipeline (KMS: mode-setting)
 *   - Userspace driver ABI (ioctl dispatch)
 *
 * Actual shader compilation and GL/VK dispatch is in userspace.
 * The kernel provides:
 *   1. Secure memory allocation (GPU zone)
 *   2. Command buffer submission
 *   3. Vsync / fence signaling
 *   4. Display scanout
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "graphics.h"
#include "../drivers/gpu/gpu_driver.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"

/* -------------------------------------------------------
 * DRM device (one per GPU)
 * -------------------------------------------------------*/
struct drm_device {
    char          name[32];
    u32           version_major;
    u32           version_minor;
    struct gpu_info gpu;

    /* Mode / display */
    u32           mode_width;
    u32           mode_height;
    u32           mode_refresh;
    u32           mode_bpp;

    /* Scanout framebuffer */
    phys_addr_t   fb_pa;
    u64           fb_va;
    size_t        fb_size;
    u32           fb_stride;

    /* Feature caps queried from GPU */
    bool          has_opengl;
    u32           opengl_major;
    u32           opengl_minor;
    bool          has_vulkan;
    u32           vulkan_variant;
    u32           vulkan_major;
    u32           vulkan_minor;
    u32           vulkan_patch;

    bool          initialized;
};

static struct drm_device g_drm;

/* -------------------------------------------------------
 * Determine OpenGL ES and Vulkan versions from GPU
 * (GPU-specific, NOT hardcoded)
 * -------------------------------------------------------*/
static void graphics_detect_versions(struct drm_device *dev)
{
    const struct gpu_info *gpu = &dev->gpu;

    switch (gpu->vendor) {
    case GPU_VENDOR_MALI: {
        /* Mali Bifrost (G52, G72, G76): OpenGL ES 3.2, Vulkan 1.1 */
        /* Mali Valhall (G57, G77, G78, G710, G715): OpenGL ES 3.2, Vulkan 1.2 */
        /* Mali 5th gen (G720, G925): OpenGL ES 3.2, Vulkan 1.3 */
        u32 part = (gpu->gpu_id >> 16) & 0xFFFF;
        if (part >= 0xB002) {
            /* 5th gen */
            dev->opengl_major = 3; dev->opengl_minor = 2;
            dev->vulkan_major = 1; dev->vulkan_minor = 3; dev->vulkan_patch = 0;
            dev->vulkan_variant = 0;
        } else if (part >= 0x9004) {
            /* Valhall */
            dev->opengl_major = 3; dev->opengl_minor = 2;
            dev->vulkan_major = 1; dev->vulkan_minor = 2; dev->vulkan_patch = 0;
        } else {
            /* Bifrost */
            dev->opengl_major = 3; dev->opengl_minor = 2;
            dev->vulkan_major = 1; dev->vulkan_minor = 1; dev->vulkan_patch = 0;
        }
        dev->has_opengl = true;
        dev->has_vulkan = true;
        break;
    }
    case GPU_VENDOR_ADRENO: {
        /* Adreno 6xx: Vulkan 1.1, OpenGL ES 3.2 */
        /* Adreno 7xx: Vulkan 1.3, OpenGL ES 3.2 */
        u32 adreno_num = gpu->gpu_id & 0xFFF;
        if (adreno_num >= 700) {
            dev->vulkan_major = 1; dev->vulkan_minor = 3;
        } else if (adreno_num >= 600) {
            dev->vulkan_major = 1; dev->vulkan_minor = 1;
        } else {
            dev->vulkan_major = 1; dev->vulkan_minor = 0;
        }
        dev->vulkan_patch   = 0;
        dev->opengl_major   = 3;
        dev->opengl_minor   = 2;
        dev->has_opengl     = true;
        dev->has_vulkan     = (adreno_num >= 500);
        break;
    }
    default:
        /* Generic: conservative */
        dev->opengl_major = 3; dev->opengl_minor = 0;
        dev->vulkan_major = 1; dev->vulkan_minor = 0;
        dev->has_opengl   = true;
        dev->has_vulkan   = false;
        break;
    }

    pr_info("GRAPHICS: OpenGL ES %u.%u, Vulkan %u.%u.%u\n",
            dev->opengl_major, dev->opengl_minor,
            dev->vulkan_major, dev->vulkan_minor, dev->vulkan_patch);
}

/* -------------------------------------------------------
 * graphics_init - called after GPU driver is up
 * -------------------------------------------------------*/
static u32 g_fb_order = 0;     /* order of the current framebuffer allocation */

int graphics_init(u32 display_w, u32 display_h,
                  u32 refresh_hz, u32 bpp)
{
    if (!display_w || !display_h || (bpp && (bpp % 8 || bpp > 64)))
        return MX_EINVAL;

    /* A second call used to memset the live state away, leaking the
     * framebuffer pages and their kernel mapping. */
    if (g_drm.initialized) {
        if (g_drm.fb_va) vmm_unmap(g_drm.fb_va, g_drm.fb_size);
        if (g_drm.fb_pa) pmm_free_pages(g_drm.fb_pa, g_fb_order);
    }
    memset(&g_drm, 0, sizeof(g_drm));
    strncpy(g_drm.name, "monox-drm", 31);
    g_drm.version_major = 1;
    g_drm.version_minor = 0;

    /* Query GPU info */
    gpu_get_info(&g_drm.gpu);
    if (!g_drm.gpu.initialized) {
        pr_warn("GRAPHICS: GPU not initialized, DRM deferred\n");
        return MX_ENODEV;
    }

    /* Display mode */
    g_drm.mode_width   = display_w;
    g_drm.mode_height  = display_h;
    g_drm.mode_refresh = refresh_hz;
    g_drm.mode_bpp     = bpp ? bpp : 32;

    /* Allocate primary framebuffer from GPU memory zone */
    u32 bytes_per_pixel = g_drm.mode_bpp / 8;
    g_drm.fb_stride = ALIGN(display_w * bytes_per_pixel, 64);
    g_drm.fb_size   = (size_t)g_drm.fb_stride * display_h;

    u32 order = 0;
    size_t sz = 4096;
    /* PMM_MAX_ORDER now allows 32 MiB; the old hard-coded "11" capped the
     * framebuffer at 4 MiB (a 1080x2340 panel needs ~10 MB) */
    while (sz < g_drm.fb_size && order < PMM_MAX_ORDER - 1) { sz <<= 1; order++; }
    if (sz < g_drm.fb_size) {
        pr_err("GRAPHICS: framebuffer of %zu bytes exceeds the largest contiguous block\n",
               g_drm.fb_size);
        return MX_ENOMEM;
    }

    g_drm.fb_pa = pmm_alloc_pages(order, GFP_GPU | __GFP_ZERO);
    if (!g_drm.fb_pa) {
        pr_err("GRAPHICS: failed to allocate framebuffer (%zu bytes)\n",
               g_drm.fb_size);
        return MX_ENOMEM;
    }

    g_drm.fb_va = vmm_map_phys(g_drm.fb_pa, g_drm.fb_size,
                                 VM_READ | VM_WRITE);
    if (!g_drm.fb_va) {
        pmm_free_pages(g_drm.fb_pa, order);
        g_drm.fb_pa = 0;
        return MX_ENOMEM;
    }
    g_fb_order = order;

    /* Detect API versions from GPU */
    graphics_detect_versions(&g_drm);

    g_drm.initialized = true;

    pr_info("GRAPHICS: DRM ready %ux%u@%uHz %ubpp FB=0x%llx (%zu KiB)\n",
            display_w, display_h, refresh_hz, g_drm.mode_bpp,
            g_drm.fb_pa, g_drm.fb_size / 1024);

    return MX_OK;
}

/* -------------------------------------------------------
 * drm_fb_get - return framebuffer info to caller
 * -------------------------------------------------------*/
void drm_fb_get(struct drm_fb_info *info)
{
    if (!info) return;
    info->pa     = g_drm.fb_pa;
    info->va     = g_drm.fb_va;
    info->width  = g_drm.mode_width;
    info->height = g_drm.mode_height;
    info->stride = g_drm.fb_stride;
    info->bpp    = g_drm.mode_bpp;
    info->size   = g_drm.fb_size;
}

/* -------------------------------------------------------
 * drm_submit_flip - request scanout of new framebuffer
 * (simplified: in real KMS this goes through CRTC pipeline)
 * -------------------------------------------------------*/
int drm_submit_flip(phys_addr_t new_fb_pa)
{
    if (!g_drm.initialized) return MX_ENODEV;

    /* Wait for GPU to finish rendering */
    gpu_wait_idle(100);

    /* Update scanout address in display controller */
    /* Display controller would read from new_fb_pa at next vsync */
    (void)new_fb_pa;

    return MX_OK;
}

/* -------------------------------------------------------
 * drm_get_caps - report supported graphics APIs
 * -------------------------------------------------------*/
void drm_get_caps(struct drm_caps *caps)
{
    if (!caps) return;
    caps->has_opengl    = g_drm.has_opengl;
    caps->opengl_major  = g_drm.opengl_major;
    caps->opengl_minor  = g_drm.opengl_minor;
    caps->has_vulkan    = g_drm.has_vulkan;
    caps->vulkan_major  = g_drm.vulkan_major;
    caps->vulkan_minor  = g_drm.vulkan_minor;
    caps->vulkan_patch  = g_drm.vulkan_patch;
    caps->max_width     = g_drm.gpu.shader_cores * 256;
    caps->max_height    = 16384;
    caps->max_textures  = 256;
}
