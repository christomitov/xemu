/*
 * Wasm framebuffer bridge.
 *
 * The browser build has no GL and no console listener, so expose the current
 * scanout to the host page through MEMFS: on each vblank update, write the
 * framebuffer parameters (width, pitch, height, bpp) and a copy of guest
 * VRAM starting at the scanout base. The page polls these files and blits
 * the pixels to a canvas. Called from the BQL-held gfx update path; kept
 * cheap: one memcpy of at most ~1.5MB per frame is acceptable for MEMFS.
 */
#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "hw/xbox/nv2a/nv2a_int.h"

void xemu_wasm_fb_update(NV2AState *d)
{
    static bool dir_ready;
    static char last_params[128];
    static bool wrote_once;

    if (!dir_ready) {
        qemu_mkdir("/xemu/fb");
        dir_ready = true;
    }

    VGACommonState *v = &d->vga;

    {
        static bool fb_first;
        if (!fb_first) { fb_first = true; xemu_wasm_milestone("fb-gfx-update"); }
    }

    /* Derive bpp the same way as nv2a_get_bpp(): depth = cr[0x28]&3, with
     * ALT_MODE_SEL switching 15/16bpp; 32bpp for depth 3. */
    int depth = v->cr[0x28] & 3;
    int bpp;
    switch (depth) {
    case 2:
        bpp = d->pramdac.general_control &
              NV_PRAMDAC_GENERAL_CONTROL_ALT_MODE_SEL ? 16 : 15;
        break;
    case 3:
        bpp = 32;
        break;
    default:
        return; /* 0/1: early-boot VGA text-ish modes, skip */
    }
    uint32_t bytes_pp = (bpp + 7) / 8;

    uint32_t line_offset = (v->cr[0x13] | ((v->cr[0x19] & 0xe0) << 3) |
                            ((v->cr[0x25] & 0x20) << 6)) << 3;
    if (line_offset == 0 || line_offset > 1280 * 4) {
        return;
    }

    uint32_t width = line_offset / bytes_pp;
    uint32_t height = 480;
    uint32_t base = d->pcrtc.start;
    uint32_t copy_size = line_offset * height;

    if (base + copy_size > memory_region_size(d->vram)) {
        return;
    }

    char hdr[128];
    snprintf(hdr, sizeof(hdr), "%u %u %u %u\n", width, height, line_offset, bpp);

    /* Params only written on change; pixels every frame. */
    if (!wrote_once || strcmp(last_params, hdr) != 0) {
        FILE *f = qemu_fopen("/xemu/fb/params", "w");
        if (f) {
            fputs(hdr, f);
            fclose(f);
        }
        strncpy(last_params, hdr, sizeof(last_params) - 1);
        wrote_once = true;
    }

    {
        FILE *f = qemu_fopen("/xemu/fb/pixels", "wb");
        if (f) {
            fwrite(d->vram_ptr + base, 1, copy_size, f);
            fclose(f);
        }
    }
}
