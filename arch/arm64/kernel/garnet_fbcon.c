// SPDX-License-Identifier: GPL-2.0
/*
 * GARNET DEBUG: printk boot console that draws text onto the bootloader
 * framebuffer (0xb8000000, 1220x2712, a8r8g8b8). Not for upstream.
 *
 * Uses early_ioremap() one text line at a time, so it needs no vmalloc or
 * ioremap (which refuse this region). Registered as a boot console, so it
 * replays the whole printk buffer and is dropped before init memory is freed.
 */
#include <linux/console.h>
#include <linux/font.h>
#include <linux/init.h>
#include <linux/io.h>
#include <asm/early_ioremap.h>

#define FB_BASE		0xb8000000UL
#define FB_W		1220
#define FB_H		2712
#define FB_STRIDE	(FB_W * 4)
#define SCALE		2
#define CELL_W		(8 * SCALE)
#define CELL_H		(16 * SCALE)
#define COLS		(FB_W / CELL_W)
#define LINES		(FB_H / CELL_H)
#define WIN_SIZE	(CELL_H * FB_STRIDE)

static int cur_line, cur_col;
static const unsigned char *font_data;

static void __iomem * __init gfb_map(int line)
{
	return early_ioremap(FB_BASE + (unsigned long)line * WIN_SIZE, WIN_SIZE);
}

static void __init gfb_glyph(void __iomem *win, int col, unsigned char c)
{
	const unsigned char *g = font_data + c * 16;
	int r, b, sy, sx;

	for (r = 0; r < 16; r++)
		for (sy = 0; sy < SCALE; sy++) {
			void __iomem *row = win + (r * SCALE + sy) * FB_STRIDE + col * CELL_W * 4;

			for (b = 0; b < 8; b++) {
				u32 px = (g[r] & (0x80 >> b)) ? 0xffffffff : 0xff000000;

				for (sx = 0; sx < SCALE; sx++)
					writel_relaxed(px, row + (b * SCALE + sx) * 4);
			}
		}
}

static void __init gfb_write(struct console *con, const char *s, unsigned int n)
{
	void __iomem *win = gfb_map(cur_line);

	if (!win)
		return;
	while (n--) {
		unsigned char c = *s++;

		if (c == '\r')
			continue;
		if (c == '\n' || cur_col >= COLS) {
			while (cur_col < COLS)		/* blank the rest of the line */
				gfb_glyph(win, cur_col++, ' ');
			early_iounmap(win, WIN_SIZE);
			cur_line = (cur_line + 1) % LINES;
			cur_col = 0;
			win = gfb_map(cur_line);
			if (!win)
				return;
			if (c == '\n')
				continue;
		}
		if (c == '\t') {
			do
				gfb_glyph(win, cur_col++, ' ');
			while ((cur_col & 7) && cur_col < COLS);
			continue;
		}
		if (c < 32 || c > 126)
			c = '?';
		gfb_glyph(win, cur_col++, c);
	}
	if (cur_col < COLS)
		gfb_glyph(win, cur_col, 0xdb);		/* block cursor = end of log */
	early_iounmap(win, WIN_SIZE);
}

static struct console gfb_con __initdata = {
	.name	= "garnetfb",
	.write	= gfb_write,
	.flags	= CON_PRINTBUFFER | CON_BOOT | CON_ENABLED,
	.index	= -1,
};

void __init garnet_fbcon_init(void)
{
	const struct font_desc *f = find_font("VGA8x16");

	if (!f)
		return;
	font_data = f->data;
	register_console(&gfb_con);
}
