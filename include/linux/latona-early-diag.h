/* SPDX-License-Identifier: GPL-2.0-only */
/* Temporary Latona diagnostic; stages 1-6 use identity maps, 7+ use registered I/O maps. */
#ifndef _LINUX_LATONA_EARLY_DIAG_H
#define _LINUX_LATONA_EARLY_DIAG_H
#include <linux/init.h>
#include <linux/types.h>
#if defined(CONFIG_ARCH_OMAP3) && !defined(CONFIG_ARM_LPAE) && !defined(CONFIG_DRM_SIMPLEDRM) && !defined(CONFIG_DRM_OMAP)
static noinline __init void latona_early_digit(unsigned int stage)
{
        volatile u32 *dispc = (volatile u32 *)(stage <= 6 ? 0x48050400UL : 0xf7250400UL);
        volatile u32 *fb = (volatile u32 *)(stage <= 6 ? 0x9fd8f000UL : 0xf708f000UL);
        static const u8 digits[] = {0, 6, 91, 79, 102, 109, 125, 7, 127, 111, 119, 124, 57, 94, 121, 113, 63, 6, 91, 79, 102, 109, 125, 7, 127, 111, 121};
        unsigned int x, y, mask, ink;
        unsigned long a;
        if (stage < 1 || stage > 26 || !(dispc[0x40/4] & 1) ||
            !(dispc[0xa0/4] & 1) || ((dispc[0xa0/4] >> 1) & 15) != 8 ||
            dispc[0x8c/4] != 0x031f01df || dispc[0x80/4] != 0x9fd80000)
                return;
        mask = digits[stage];
        for (y = 0; y < 64; y++) {
                for (x = 0; x < 480; x++) {
                        ink = ((mask & 1) && y >= 4 && y < 10 && x >= 20 && x < 44) ||
                              ((mask & 2) && x >= 44 && x < 50 && y >= 10 && y < 28) ||
                              ((mask & 4) && x >= 44 && x < 50 && y >= 32 && y < 50) ||
                              ((mask & 8) && y >= 50 && y < 56 && x >= 20 && x < 44) ||
                              ((mask & 16) && x >= 14 && x < 20 && y >= 32 && y < 50) ||
                              ((mask & 32) && x >= 14 && x < 20 && y >= 10 && y < 28) ||
                              ((mask & 64) && y >= 28 && y < 34 && x >= 20 && x < 44);
                        fb[y * 480 + x] = ink ? 0xffffffff : 0xff000000;
                }
        }
        for (a = (unsigned long)fb; a < (unsigned long)fb + 480 * 64 * 4; a += 32)
                asm volatile("mcr p15, 0, %0, c7, c10, 1" : : "r" (a) : "memory");
        asm volatile("dsb sy" : : : "memory");
}

#ifdef LATONA_DIAG_CONSOLE_IMPL
#include <linux/console.h>
#include <linux/font.h>
#include <linux/string.h>
/* 60 columns x 44 rows: y=96..799, within the mapped 2MiB region. */
static unsigned int latona_fb_col, latona_fb_row;
static bool latona_fb_capture_first, latona_fb_capture_full;
static void latona_fb_newline(void)
{
        volatile u32 *fb = (volatile u32 *)0xf70ad000UL;
        unsigned int i;
        latona_fb_col = 0;
        if (++latona_fb_row < 44)
                return;
        if (latona_fb_capture_first) {
                latona_fb_row = 43;
                latona_fb_capture_full = true;
                return;
        }
        for (i = 0; i < 480 * 688; i++)
                fb[i] = fb[i + 480 * 16];
        for (; i < 480 * 704; i++)
                fb[i] = 0xff000000;
        latona_fb_row = 43;
}
/* Keep four preceding text rows, then retain the first fault screen. */
static void latona_fb_capture_fault(void)
{
        volatile u32 *fb = (volatile u32 *)0xf70ad000UL;
        unsigned int i, from = latona_fb_row > 4 ? latona_fb_row - 4 : 0;
        for (i = 0; i < 480 * 64; i++)
                fb[i] = fb[from * 16 * 480 + i];
        for (; i < 480 * 704; i++) fb[i] = 0xff000000;
        latona_fb_row = 4;
        latona_fb_col = 0;
        latona_fb_capture_first = true;
        latona_fb_capture_full = false;
        asm volatile("dsb sy" : : : "memory");
}
static void latona_fb_write(struct console *con, const char *s, unsigned int count)
{
        volatile u32 *fb = (volatile u32 *)0xf70ad000UL;
        const unsigned char *font = font_vga_8x16.data;
        unsigned int i, x, y, ch;
        if (!latona_fb_capture_first &&
            (strnstr(s, "Unable to handle kernel", count) ||
             strnstr(s, "Unhandled fault:", count) ||
             strnstr(s, "Internal error:", count)))
                latona_fb_capture_fault();
        for (i = 0; i < count; i++) {
                if (latona_fb_capture_full) break;
                ch = (unsigned char)s[i];
                if (ch == '\n') { latona_fb_newline(); continue; }
                if (ch == '\r') { latona_fb_col = 0; continue; }
                if (ch == '\t') ch = ' ';
                if (ch < 32) continue;
                for (y = 0; y < 16; y++)
                        for (x = 0; x < 8; x++)
                                fb[(latona_fb_row * 16 + y) * 480 + latona_fb_col * 8 + x] =
                                        (font[ch * 16 + y] & (0x80 >> x)) ? 0xffffffff : 0xff000000;
                if (++latona_fb_col == 60) latona_fb_newline();
        }
        asm volatile("dsb sy" : : : "memory");
}
static void __init latona_fb_capture_init_early(void)
{
        volatile u32 *fb = (volatile u32 *)0xf70ad000UL;
        unsigned int i;
        latona_fb_col = latona_fb_row = 0;
        latona_fb_capture_full = false;
        latona_fb_capture_first = true;
        for (i = 0; i < 480 * 704; i++) fb[i] = 0xff000000;
        asm volatile("dsb sy" : : : "memory");
}
static void __init latona_fb_resume_live(void)
{
        latona_fb_capture_init_early();
        latona_fb_capture_first = false;
}
static struct console latona_fb_console = {
        .name = "latonafb", .write = latona_fb_write,
        .flags = CON_PRINTBUFFER | CON_BOOT, .index = -1,
};
static void __init latona_fb_console_init(void)
{
        volatile u32 *dispc = (volatile u32 *)0xf7250400UL;
        volatile u32 *fb = (volatile u32 *)0xf70ad000UL;
        unsigned int i;
        if (!(dispc[0x40/4] & 1) || !(dispc[0xa0/4] & 1) ||
            ((dispc[0xa0/4] >> 1) & 15) != 8 ||
            dispc[0x8c/4] != 0x031f01df || dispc[0x80/4] != 0x9fd80000)
                return;
        for (i = 0; i < 480 * 704; i++) fb[i] = 0xff000000;
        register_console(&latona_fb_console);
        pr_info("LATONA framebuffer boot console\n");
}
#endif

#else
static inline void latona_early_digit(unsigned int stage) { }
#ifdef LATONA_DIAG_CONSOLE_IMPL
static inline void latona_fb_console_init(void) { }
static inline void latona_fb_capture_init_early(void) { }
static inline void latona_fb_resume_live(void) { }
#endif
#endif
#endif
