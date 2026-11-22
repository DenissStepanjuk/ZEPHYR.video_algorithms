/* Коды ошибок (-EINVAL и т.д.). */
#include <errno.h>
/* Тип bool. */
#include <stdbool.h>
/* Регистрация модуля логирования. */
#include <zephyr/logging/log.h>

#include "custom_mpix_HSV.h"

LOG_MODULE_REGISTER(custom_mpix_hsv, LOG_LEVEL_DBG);

void custom_mpix_rgb_to_hsv(uint8_t r, uint8_t g, uint8_t b, uint8_t *h, uint8_t *s, uint8_t *v)
{
	uint8_t max = r;
	uint8_t min = r;
	uint16_t delta;
	int32_t h60; /* Оттенок в градусах 0..360 (временно может быть отрицательным). */

	if (g > max) {
		max = g;
	}
	if (b > max) {
		max = b;
	}
	if (g < min) {
		min = g;
	}
	if (b < min) {
		min = b;
	}

	delta = (uint16_t)(max - min);

	/* Яркость (V) = максимальная компонента — как в оригинале. */
	*v = max;

	/* Насыщенность (S): если max == 0 (чистый чёрный), насыщенность не определена —
	 * считаем 0, а не делим на ноль (в оригинале эта ветка тоже была, но с побочным
	 * багом "continue до записи буфера" — см. п.2 в шапке .h). */
	*s = (max == 0) ? 0 : (uint8_t)(((uint32_t)delta * 255) / max);

	/* Оттенок (H): если delta == 0 (серый/чёрный/белый, r == g == b), оттенок не
	 * определён — берём 0 явно, тоже без пропуска записи. */
	if (delta == 0) {
		*h = 0;
		return;
	}

	if (max == r) {
		/* Сектор между жёлтым и пурпурным, -60..60 градусов. */
		h60 = (60 * (int32_t)((int32_t)g - (int32_t)b)) / (int32_t)delta;
	} else if (max == g) {
		/* Сектор между цианом и жёлтым, 60..180 градусов. */
		h60 = 120 + (60 * (int32_t)((int32_t)b - (int32_t)r)) / (int32_t)delta;
	} else {
		/* Сектор между пурпурным и цианом, 180..300 градусов. */
		h60 = 240 + (60 * (int32_t)((int32_t)r - (int32_t)g)) / (int32_t)delta;
	}

	if (h60 < 0) {
		h60 += 360;
	}

	/* Масштабируем честные градусы 0..360 в удобный для слайдеров диапазон 0..255,
	 * как и требовалось (в отличие от нестандартной шкалы 0..180, которую использует
	 * часть портов этого алгоритма). */
	*h = (uint8_t)((h60 * 255) / 360);
}

int custom_mpix_hsv_mask(uint8_t *buf_rgb24, int width, int height,
			  const struct custom_mpix_hsv_range *range)
{
	if (!buf_rgb24 || !range) {
		LOG_ERR("hsv_mask: NULL argument");
		return -EINVAL;
	}
	if (width <= 0 || height <= 0) {
		LOG_ERR("hsv_mask: invalid frame size %dx%d", width, height);
		return -EINVAL;
	}
	if (range->hue_min > range->hue_max || range->sat_min > range->sat_max ||
	    range->val_min > range->val_max) {
		LOG_ERR("hsv_mask: invalid range (min > max) hue[%u,%u] sat[%u,%u] val[%u,%u]",
			range->hue_min, range->hue_max, range->sat_min, range->sat_max,
			range->val_min, range->val_max);
		return -EINVAL;
	}

	size_t pixel_count = (size_t)width * (size_t)height;

	for (size_t i = 0; i < pixel_count; i++) {
		size_t idx = i * 3;
		uint8_t r = buf_rgb24[idx + CUSTOM_MPIX_RGB24_R_OFFSET];
		uint8_t g = buf_rgb24[idx + CUSTOM_MPIX_RGB24_G_OFFSET];
		uint8_t b = buf_rgb24[idx + CUSTOM_MPIX_RGB24_B_OFFSET];
		uint8_t h, s, v;
		bool in_range;

		custom_mpix_rgb_to_hsv(r, g, b, &h, &s, &v);

		in_range = (h >= range->hue_min && h <= range->hue_max) &&
			   (s >= range->sat_min && s <= range->sat_max) &&
			   (v >= range->val_min && v <= range->val_max);

		if (!in_range) {
			buf_rgb24[idx + CUSTOM_MPIX_RGB24_R_OFFSET] = 255;
			buf_rgb24[idx + CUSTOM_MPIX_RGB24_G_OFFSET] = 255;
			buf_rgb24[idx + CUSTOM_MPIX_RGB24_B_OFFSET] = 255;
		}
		/* Иначе — пиксель в норме, R/G/B не трогаем (уже на месте). */
	}

	return 0;
}
