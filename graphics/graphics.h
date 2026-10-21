/* MONOX Kernel - Graphics Interface */

#ifndef _MONOX_GRAPHICS_H
#define _MONOX_GRAPHICS_H

#include <kernel/types.h>

struct drm_fb_info {
    phys_addr_t pa;
    u64         va;
    u32         width;
    u32         height;
    u32         stride;
    u32         bpp;
    size_t      size;
};

struct drm_caps {
    bool has_opengl;
    u32  opengl_major;
    u32  opengl_minor;
    bool has_vulkan;
    u32  vulkan_major;
    u32  vulkan_minor;
    u32  vulkan_patch;
    u32  max_width;
    u32  max_height;
    u32  max_textures;
};

int  graphics_init(u32 display_w, u32 display_h, u32 refresh_hz, u32 bpp);
void drm_fb_get(struct drm_fb_info *info);
int  drm_submit_flip(phys_addr_t new_fb_pa);
void drm_get_caps(struct drm_caps *caps);

#endif /* _MONOX_GRAPHICS_H */
