#include "seg_postprocess.h"
#include "seg_config.h"

int seg_postprocess(const int8_t *logits, uint8_t *mask, int *class_px)
{
	int total = 0;

	if (class_px) {
		for (int c = 0; c < SEG_NUM_CLASSES; c++) {
			class_px[c] = 0;
		}
	}
	for (int p = 0; p < SEG_OUT_H * SEG_OUT_W; p++) {
		uint8_t bits = 0;

		for (int c = 0; c < SEG_NUM_CLASSES; c++) {
			if ((int)logits[p * SEG_NUM_CLASSES + c] >= SEG_Q_THR[c]) {
				bits |= (uint8_t)(1u << c);
				if (class_px) {
					class_px[c]++;
				}
			}
		}
		mask[p] = bits;
		if (bits) {
			total++;
		}
	}
	return total;
}

static inline uint16_t swap16(uint16_t v)
{
	return (uint16_t)((v >> 8) | (v << 8));
}

/* Смешивание пикселя RGB565 с цветом rgb888; a = 0..256. */
static inline uint16_t blend565(uint16_t px, const uint8_t rgb[3], int a)
{
	int r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;

	r = (r << 3) | (r >> 2);
	g = (g << 2) | (g >> 4);
	b = (b << 3) | (b >> 2);
	r = (r * (256 - a) + rgb[0] * a) >> 8;
	g = (g * (256 - a) + rgb[1] * a) >> 8;
	b = (b * (256 - a) + rgb[2] * a) >> 8;
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void seg_overlay_rgb565(const uint8_t *mask, uint16_t *img565)
{
	for (int p = 0; p < SEG_OUT_H * SEG_OUT_W; p++) {
		uint8_t bits = mask[p];

		if (!bits) {
			continue;
		}
		uint16_t px = img565[p];
#if SEG_RGB565_BYTESWAP
		px = swap16(px);
#endif
		for (int c = 0; c < SEG_NUM_CLASSES; c++) {
			if (bits & (1u << c)) {
				px = blend565(px, SEG_COLORS[c], SEG_OVERLAY_ALPHA);
			}
		}
#if SEG_RGB565_BYTESWAP
		px = swap16(px);
#endif
		img565[p] = px;
	}
}
