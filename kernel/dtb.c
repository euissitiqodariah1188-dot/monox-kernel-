/* MONOX Kernel - Device Tree Blob (DTB) Parser
 *
 * Parses the Flattened Device Tree (FDT) passed from the bootloader.
 * Extracts: memory regions, UART, GIC, GPU, USB, WiFi, touchscreen.
 * All hardware addresses come from here - ZERO hardcoded MMIO values.
 *
 * FDT spec: https://github.com/devicetree-org/devicetree-specification
 *
 * Bug-fix pass (the parser runs with the MMU OFF, so it must be strictly
 * alignment-safe and must never read past the blob):
 *   - ts_dtb_props[32] was indexed up to [60] => stack/struct overflow.
 *   - 64-bit reads through (u64*) at 4-byte-aligned property data caused
 *     alignment faults with the MMU off. All reads are byte-wise now.
 *   - #address-cells/#size-cells were restored from the wrong stack level
 *     after any child node, so /memory reg was mis-sized whenever another
 *     node (e.g. /cpus) preceded it. Cells now follow the spec: a node's
 *     reg is decoded with its PARENT's cells.
 *   - Nodes deeper than 16 levels desynchronised the token stream and
 *     indexed node_stack out of bounds.
 *   - No bounds checks => a truncated/malicious DTB read past the blob.
 *   - 'interrupts' used the raw SPI number; the GIC INTID is SPI+32 (PPI+16).
 *   - Numeric properties handed to the touchscreen driver were raw
 *     big-endian binary, but the driver parses text => addr/irq/size were
 *     always 0. They are now converted to text.
 *   - Any "interrupt-controller@" node overwrote the GIC addresses.
 *   - /reserved-memory and /memreserve/ were ignored, so the kernel handed
 *     firmware carve-outs to the allocator and then touched them.
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "dtb.h"

/* -------------------------------------------------------
 * FDT magic and structure
 * -------------------------------------------------------*/
#define FDT_MAGIC         0xD00DFEEDU
#define FDT_BEGIN_NODE    0x00000001U
#define FDT_END_NODE      0x00000002U
#define FDT_PROP          0x00000003U
#define FDT_NOP           0x00000004U
#define FDT_END           0x00000009U

#define FDT_HDR_SIZE      40U

/* -------------------------------------------------------
 * Alignment-safe big-endian accessors (byte wise)
 * -------------------------------------------------------*/
static __always_inline u32 rd_be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static __always_inline u64 rd_be64(const u8 *p)
{
    return ((u64)rd_be32(p) << 32) | rd_be32(p + 4);
}

/* a "cells" value: 1 cell = 32 bit, >=2 cells = low 64 bits */
static u64 rd_cells(const u8 *p, u32 cells)
{
    if (cells == 0) return 0;
    if (cells == 1) return rd_be32(p);
    return rd_be64(p + (cells - 2) * 4);   /* high cells beyond 64 bit dropped */
}

/* -------------------------------------------------------
 * FDT context
 * -------------------------------------------------------*/
struct fdt_ctx {
    const u8  *base;
    const u8  *struct_base;
    const char *strings_base;
    u32        struct_size;   /* bytes */
    u32        strings_size;  /* bytes */
    u32        pos;           /* current byte offset in struct section */
};

/* -------------------------------------------------------
 * Small text helpers (no libc)
 * -------------------------------------------------------*/
static void put_dec(char *out, u32 outsz, u64 v)
{
    char tmp[24]; u32 n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    u32 i = 0;
    while (n && i + 1 < outsz) out[i++] = tmp[--n];
    out[i] = '\0';
}

static void put_hex(char *out, u32 outsz, u64 v)
{
    char tmp[20]; u32 n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = "0123456789abcdef"[v & 0xF]; v >>= 4; }
    u32 i = 0;
    if (i + 2 < outsz) { out[i++] = '0'; out[i++] = 'x'; }
    while (n && i + 1 < outsz) out[i++] = tmp[--n];
    out[i] = '\0';
}

/* GIC INTID from an "interrupts" specifier <type number flags> */
static u32 fdt_intid(const u8 *p, u32 len)
{
    if (len >= 12) {
        u32 type = rd_be32(p), num = rd_be32(p + 4);
        return (type == 0) ? num + 32 : (type == 1) ? num + 16 : num;
    }
    if (len >= 8)                       /* <number flags> style */
        return rd_be32(p);
    if (len >= 4)
        return rd_be32(p);
    return 0;
}

/* does the NUL-separated string list 'p' (len bytes) contain a string that
 * starts with 'prefix'? (used for "compatible") */
static bool compat_has(const u8 *p, u32 len, const char *prefix)
{
    u32 plen = (u32)strlen(prefix);
    u32 i = 0;
    while (i < len) {
        const char *s = (const char *)p + i;
        u32 sl = 0;
        while (i + sl < len && s[sl]) sl++;
        if (sl >= plen && strncmp(s, prefix, plen) == 0) return true;
        i += sl + 1;
    }
    return false;
}

/* exact match of one entry of a NUL-separated string list */
static bool compat_eq(const u8 *p, u32 len, const char *word)
{
    u32 wl = (u32)strlen(word);
    u32 i = 0;
    while (i < len) {
        const char *s = (const char *)p + i;
        u32 sl = 0;
        while (i + sl < len && s[sl]) sl++;
        if (sl == wl && strncmp(s, word, wl) == 0) return true;
        i += sl + 1;
    }
    return false;
}

/* copy a property string with a hard bound (never trusts a terminator) */
static void copy_prop_str(char *dst, u32 dstsz, const u8 *p, u32 len)
{
    u32 n = len < dstsz - 1 ? len : dstsz - 1;
    u32 i = 0;
    for (; i < n && p[i]; i++) dst[i] = (char)p[i];
    dst[i] = '\0';
}

/* -------------------------------------------------------
 * Node bookkeeping
 * -------------------------------------------------------*/
enum node_kind {
    NK_NONE = 0, NK_MEMORY, NK_GIC, NK_GPU, NK_XHCI, NK_WIFI, NK_TS,
    NK_CHOSEN, NK_CPU, NK_UART, NK_RESV_PARENT, NK_RESV_CHILD, NK_PSCI
};

#define MAX_DEPTH 16

struct parse_node {
    char name[64];
    u8   kind;
    u32  addr_cells;     /* cells used by THIS node's children */
    u32  size_cells;

    /* Captured while the node's properties stream by and evaluated at
     * END_NODE, because property ORDER is arbitrary ("reg" may precede
     * "compatible"). Detection is by compatible string, not by node name:
     * QEMU names its GICv3 "intc@8000000". */
    u8   reg[64];
    u32  reg_len, reg_pac, reg_psc;
    bool has_compat;
    bool is_gic;         /* compatible == arm,gic-v3 / arm,gic-v4 (exact)   */
    bool is_pl011;       /* compatible contains arm,pl011                   */
    bool name_gic;       /* node name looks like a GIC (fallback only)      */
    u32  stride;
};

static bool add_region(struct board_info *out, u64 base, u64 size, mem_type_t type)
{
    if (!size) return true;
    if (out->nr_regions >= (sizeof(out->regions) / sizeof(out->regions[0]))) {
        pr_warn("DTB: region table full, dropping 0x%llx+0x%llx\n", base, size);
        return false;
    }
    out->regions[out->nr_regions].base = base;
    out->regions[out->nr_regions].size = (size_t)size;
    out->regions[out->nr_regions].type = type;
    out->nr_regions++;
    return true;
}

/* -------------------------------------------------------
 * dtb_parse - main DTB parser
 * -------------------------------------------------------*/
void dtb_parse(phys_addr_t dtb_phys, struct board_info *out)
{
    /* The MMU is off: phys_to_virt_early() is the identity here. */
    extern void *phys_to_virt_early(phys_addr_t pa);
    const u8 *fdt = (const u8 *)phys_to_virt_early(dtb_phys);
    if (!dtb_phys || !fdt) {
        pr_warn("DTB: invalid DTB physical address 0x%llx\n", dtb_phys);
        return;
    }
    if (dtb_phys & 7) {
        pr_warn("DTB: blob at 0x%llx is not 8-byte aligned\n", dtb_phys);
        return;
    }

    if (rd_be32(fdt) != FDT_MAGIC) {
        pr_warn("DTB: bad magic 0x%08x (expected 0x%08x)\n", rd_be32(fdt), FDT_MAGIC);
        return;
    }

    u32 totalsize   = rd_be32(fdt + 4);
    if (totalsize < FDT_HDR_SIZE) {            /* before touching the rest of the header */
        pr_warn("DTB: totalsize %u smaller than the header\n", totalsize);
        return;
    }
    u32 off_struct  = rd_be32(fdt + 8);
    u32 off_strings = rd_be32(fdt + 12);
    u32 off_rsvmap  = rd_be32(fdt + 16);
    u32 version     = rd_be32(fdt + 20);
    u32 size_strings= rd_be32(fdt + 32);
    u32 size_struct = rd_be32(fdt + 36);

    /* Validate every offset against totalsize before dereferencing */
    if (off_struct  > totalsize || size_struct  > totalsize - off_struct  ||
        off_strings > totalsize || size_strings > totalsize - off_strings ||
        off_rsvmap  > totalsize || (off_struct & 3)) {
        pr_warn("DTB: corrupt header (total=%u struct=%u+%u strings=%u+%u)\n",
                totalsize, off_struct, size_struct, off_strings, size_strings);
        return;
    }

    out->dtb_phys = dtb_phys;
    out->dtb_size = totalsize;

    struct fdt_ctx ctx;
    ctx.base         = fdt;
    ctx.struct_base  = fdt + off_struct;
    ctx.strings_base = (const char *)(fdt + off_strings);
    ctx.struct_size  = size_struct;
    ctx.strings_size = size_strings;
    ctx.pos          = 0;

    pr_info("DTB: FDT version %u total_size=%u\n", version, totalsize);

    /* ---- /memreserve/ block: (addr, size) pairs of u64 BE, 0/0 ends ---- */
    for (u32 o = off_rsvmap; o + 16 <= totalsize; o += 16) {
        u64 a = rd_be64(fdt + o), s = rd_be64(fdt + o + 8);
        if (!a && !s) break;
        add_region(out, a, s, MEM_TYPE_RESERVED);
    }

    /* ---- Walk FDT structure ---- */
    struct parse_node stack[MAX_DEPTH];
    int  depth = -1;

    u32  ts_prop_count = 0;
    u32  cpu_count_local = 0;

    bool gic_done = false;
    bool uart_is_pl011 = false;

    static char ts_keys[16][32];
    static char ts_vals[16][64];

    while (ctx.pos + 4 <= ctx.struct_size) {
        u32 token = rd_be32(ctx.struct_base + ctx.pos);
        ctx.pos += 4;

        switch (token) {
        case FDT_BEGIN_NODE: {
            /* NUL-terminated name, padded to 4 bytes. ALWAYS consume it,
             * even when the node is too deep to be tracked. */
            const u8 *nm = ctx.struct_base + ctx.pos;
            u32 maxlen = ctx.struct_size - ctx.pos;
            u32 len = 0;
            while (len < maxlen && nm[len]) len++;
            if (len >= maxlen) { pr_warn("DTB: unterminated node name\n"); goto done; }
            ctx.pos += ALIGN(len + 1, 4);

            depth++;
            if (depth >= MAX_DEPTH) break;           /* untracked deep node */

            struct parse_node *n = &stack[depth];
            copy_prop_str(n->name, sizeof(n->name), nm, len + 1);
            n->addr_cells = 2;                       /* spec defaults, NOT inherited */
            n->size_cells = 1;
            n->kind       = NK_NONE;
            n->reg_len = 0; n->reg_pac = n->reg_psc = 0;
            n->has_compat = n->is_gic = n->is_pl011 = n->name_gic = false;
            n->stride = 0;

            const char *nmz = n->name;
            if (strncmp(nmz, "memory@", 7) == 0 || strcmp(nmz, "memory") == 0)
                n->kind = NK_MEMORY;
            else if (strncmp(nmz, "interrupt-controller@", 21) == 0 ||
                     strncmp(nmz, "gic@", 4) == 0 || strncmp(nmz, "gic-v3@", 7) == 0 ||
                     strncmp(nmz, "intc@", 5) == 0) {
                n->kind = NK_GIC;
                n->name_gic = true;          /* only used if no compatible is given */
            }
            else if (strncmp(nmz, "gpu@", 4) == 0 || strncmp(nmz, "mali@", 5) == 0)
                n->kind = NK_GPU;
            else if (strncmp(nmz, "usb@", 4) == 0 || strncmp(nmz, "xhci@", 5) == 0)
                n->kind = NK_XHCI;
            else if (strncmp(nmz, "wifi@", 5) == 0 || strncmp(nmz, "wlan@", 5) == 0)
                n->kind = NK_WIFI;
            else if (strncmp(nmz, "touchscreen@", 12) == 0 ||
                     strncmp(nmz, "goodix@", 7) == 0 || strncmp(nmz, "elan@", 5) == 0)
                n->kind = NK_TS;
            else if (strcmp(nmz, "chosen") == 0)
                n->kind = NK_CHOSEN;
            else if (strncmp(nmz, "cpu@", 4) == 0) {
                n->kind = NK_CPU;
                cpu_count_local++;
            }
            else if (strcmp(nmz, "reserved-memory") == 0)
                n->kind = NK_RESV_PARENT;
            else if (depth >= 1 && stack[depth - 1].kind == NK_RESV_PARENT)
                n->kind = NK_RESV_CHILD;
            else if (strncmp(nmz, "serial@", 7) == 0 || strncmp(nmz, "uart@", 5) == 0 ||
                     strncmp(nmz, "pl011@", 6) == 0) {
                n->kind = NK_UART;
            }
            else if (strcmp(nmz, "psci") == 0)
                n->kind = NK_PSCI;
            break;
        }

        case FDT_END_NODE:
            if (depth >= 0 && depth < MAX_DEPTH) {
                struct parse_node *n = &stack[depth];
                u32 entry = (n->reg_pac + n->reg_psc) * 4;

                /* GICv3: reg = [GICD base,size  GICR base,size ...] */
                bool gic = n->is_gic || (n->name_gic && !n->has_compat);
                if (gic && !gic_done && n->reg_pac && entry &&
                    n->reg_len >= 2 * entry) {
                    out->gic_dist_base     = rd_cells(n->reg, n->reg_pac);
                    out->gic_redist_base   = rd_cells(n->reg + entry, n->reg_pac);
                    out->gic_redist_stride = n->stride;
                    gic_done = true;
                    pr_info("DTB: GIC dist=0x%llx redist=0x%llx\n",
                            out->gic_dist_base, out->gic_redist_base);
                }

                /* UART: a PL011 beats anything else (the driver only speaks PL011) */
                if ((n->is_pl011 || n->kind == NK_UART) && n->reg_pac &&
                    n->reg_len >= n->reg_pac * 4) {
                    if (!out->uart_base || (n->is_pl011 && !uart_is_pl011)) {
                        out->uart_base = rd_cells(n->reg, n->reg_pac);
                        uart_is_pl011  = n->is_pl011;
                        pr_info("DTB: UART @ 0x%llx%s\n", out->uart_base,
                                n->is_pl011 ? " (pl011)" : "");
                    }
                }
            }
            if (depth < 0) { pr_warn("DTB: unbalanced END_NODE\n"); goto done; }
            depth--;
            break;

        case FDT_PROP: {
            if (ctx.pos + 8 > ctx.struct_size) { pr_warn("DTB: truncated property\n"); goto done; }
            u32 prop_len = rd_be32(ctx.struct_base + ctx.pos);
            u32 name_off = rd_be32(ctx.struct_base + ctx.pos + 4);
            ctx.pos += 8;

            u32 aligned_len = ALIGN(prop_len, 4);
            if (prop_len > ctx.struct_size || aligned_len > ctx.struct_size - ctx.pos ||
                name_off >= ctx.strings_size) {
                pr_warn("DTB: property out of bounds (len=%u name_off=%u)\n", prop_len, name_off);
                goto done;
            }
            const u8   *pd = ctx.struct_base + ctx.pos;
            const char *pn = ctx.strings_base + name_off;
            ctx.pos += aligned_len;

            /* the property name must be NUL-terminated inside the strings
             * block, otherwise strcmp() would run off the end of the blob */
            {
                u32 k = name_off;
                while (k < ctx.strings_size && ctx.strings_base[k]) k++;
                if (k >= ctx.strings_size) {
                    pr_warn("DTB: unterminated property name, skipped\n");
                    break;
                }
            }

            if (depth < 0 || depth >= MAX_DEPTH) break;   /* untracked */

            struct parse_node *n = &stack[depth];

            /* cells that apply to THIS node's reg = parent's children cells */
            u32 pac = (depth > 0) ? stack[depth - 1].addr_cells : 2;
            u32 psc = (depth > 0) ? stack[depth - 1].size_cells : 1;

            /* ---- #address-cells / #size-cells: affect this node's children ---- */
            if (strcmp(pn, "#address-cells") == 0 && prop_len == 4)
                n->addr_cells = rd_be32(pd);
            else if (strcmp(pn, "#size-cells") == 0 && prop_len == 4)
                n->size_cells = rd_be32(pd);

            /* ---- Root level properties ---- */
            if (depth == 0 && strcmp(pn, "model") == 0 && prop_len > 0)
                copy_prop_str(out->model, sizeof(out->model), pd, prop_len);

            bool is_reg = (strcmp(pn, "reg") == 0);

            /* kind-independent capture (evaluated at END_NODE) */
            if (is_reg && prop_len) {
                n->reg_len = prop_len < sizeof(n->reg) ? prop_len : (u32)sizeof(n->reg);
                memcpy(n->reg, pd, n->reg_len);
                n->reg_pac = pac;
                n->reg_psc = psc;
            } else if (strcmp(pn, "compatible") == 0) {
                n->has_compat = true;
                if (compat_eq(pd, prop_len, "arm,gic-v3") || compat_eq(pd, prop_len, "arm,gic-v4"))
                    n->is_gic = true;
                if (compat_has(pd, prop_len, "arm,pl011"))
                    n->is_pl011 = true;
            } else if (strcmp(pn, "redistributor-stride") == 0 && prop_len >= 4) {
                n->stride = (prop_len >= 8) ? (u32)rd_be64(pd) : rd_be32(pd);
            }

            u32 entry_bytes = (pac + psc) * 4;
            if (pac > 4 || psc > 4) entry_bytes = 0;   /* nonsensical */

            switch (n->kind) {
            case NK_MEMORY:
                if (is_reg && entry_bytes) {
                    u32 cnt = prop_len / entry_bytes;
                    for (u32 e = 0; e < cnt; e++) {
                        const u8 *p = pd + e * entry_bytes;
                        u64 base = rd_cells(p, pac);
                        u64 size = rd_cells(p + pac * 4, psc);
                        if (add_region(out, base, size, MEM_TYPE_RAM)) {
                            out->total_ram += size;
                            pr_info("DTB: memory 0x%012llx size=%llu MiB\n",
                                    base, size / (1024 * 1024));
                        }
                    }
                }
                break;

            case NK_RESV_CHILD:
                if (is_reg && entry_bytes) {
                    u32 cnt = prop_len / entry_bytes;
                    for (u32 e = 0; e < cnt; e++) {
                        const u8 *p = pd + e * entry_bytes;
                        add_region(out, rd_cells(p, pac),
                                   rd_cells(p + pac * 4, psc), MEM_TYPE_RESERVED);
                    }
                }
                break;

            case NK_CHOSEN:
                if (strcmp(pn, "stdout-path") == 0 && prop_len > 0) {
                    char sp[64];
                    copy_prop_str(sp, sizeof(sp), pd, prop_len);
                    pr_info("DTB: stdout-path = %s\n", sp);
                }
                break;

            case NK_PSCI:
                if (strcmp(pn, "method") == 0 && prop_len >= 3) {
                    out->psci_hvc = (strncmp((const char *)pd, "hvc", 3) == 0);
                    pr_info("DTB: PSCI conduit = %s\n", out->psci_hvc ? "hvc" : "smc");
                }
                break;

            case NK_GPU:
                if (is_reg && prop_len >= (pac + psc) * 4) {
                    out->gpu_mmio      = rd_cells(pd, pac);
                    out->gpu_mmio_size = (size_t)rd_cells(pd + pac * 4, psc);
                } else if (strcmp(pn, "interrupts") == 0 && prop_len >= 4)
                    out->gpu_irq = fdt_intid(pd, prop_len);
                else if (strcmp(pn, "compatible") == 0 && prop_len > 0)
                    copy_prop_str(out->gpu_compatible, sizeof(out->gpu_compatible), pd, prop_len);
                break;

            case NK_XHCI:
                if (is_reg && prop_len >= (pac + psc) * 4) {
                    out->xhci_mmio      = rd_cells(pd, pac);
                    out->xhci_mmio_size = (size_t)rd_cells(pd + pac * 4, psc);
                } else if (strcmp(pn, "interrupts") == 0 && prop_len >= 4)
                    out->xhci_irq = fdt_intid(pd, prop_len);
                break;

            case NK_WIFI:
                if (is_reg && prop_len >= pac * 4 && pac)
                    out->wifi_mmio = rd_cells(pd, pac);
                else if (strcmp(pn, "interrupts") == 0 && prop_len >= 4)
                    out->wifi_irq = fdt_intid(pd, prop_len);
                else if (strcmp(pn, "local-mac-address") == 0 && prop_len == 6)
                    memcpy(out->wifi_mac, pd, 6);
                else if (strcmp(pn, "compatible") == 0 && prop_len > 0)
                    copy_prop_str(out->wifi_compatible, sizeof(out->wifi_compatible), pd, prop_len);
                break;

            case NK_TS:
                /* ts_dtb_props[] holds 2 pointers per property plus a NULL
                 * sentinel in a 32-slot array => at most 15 properties
                 * (the old limit of 30 wrote up to index 60). */
                if (ts_prop_count < 15) {
                    u32 i = ts_prop_count;
                    copy_prop_str(ts_keys[i], sizeof(ts_keys[i]), (const u8 *)pn, 31);
                    ts_vals[i][0] = '\0';

                    /* The touchscreen driver parses TEXT: turn numeric cells
                     * into text. */
                    if (strcmp(pn, "reg") == 0 && prop_len >= 4)
                        put_hex(ts_vals[i], sizeof(ts_vals[i]), rd_be32(pd));
                    else if (strcmp(pn, "interrupts") == 0 && prop_len >= 4)
                        put_dec(ts_vals[i], sizeof(ts_vals[i]), fdt_intid(pd, prop_len));
                    else if (prop_len == 4 && (pd[0] == 0 || pd[0] < 0x20 || pd[0] > 0x7e))
                        put_dec(ts_vals[i], sizeof(ts_vals[i]), rd_be32(pd));
                    else if (prop_len > 0)
                        copy_prop_str(ts_vals[i], sizeof(ts_vals[i]), pd, prop_len);

                    out->ts_dtb_props[i * 2    ] = ts_keys[i];
                    out->ts_dtb_props[i * 2 + 1] = ts_vals[i];
                    ts_prop_count++;
                    out->ts_dtb_props[ts_prop_count * 2] = NULL; /* sentinel */
                }
                break;

            default:
                break;
            }
            break;
        }

        case FDT_NOP:
            break;

        case FDT_END:
            goto done;

        default:
            pr_warn("DTB: unknown token 0x%08x at offset %u\n", token, ctx.pos);
            goto done;
        }
    }

done:
    if (cpu_count_local > 0)
        pr_info("DTB: found %u CPU nodes\n", cpu_count_local);

    pr_info("DTB: parsed: UART=0x%llx GIC-dist=0x%llx GPU=0x%llx USB=0x%llx\n",
            out->uart_base, out->gic_dist_base, out->gpu_mmio, out->xhci_mmio);
    pr_info("DTB: total RAM = %llu MiB across %u regions\n",
            out->total_ram / (1024 * 1024), out->nr_regions);
}
