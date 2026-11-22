/* Коды ошибок (-EINVAL и т.д.). */
#include <errno.h>
/* Регистрация модуля логирования. */
#include <zephyr/logging/log.h>

#include "custom_mpix_combine.h"

LOG_MODULE_REGISTER(custom_mpix_combine, LOG_LEVEL_DBG);

int custom_mpix_combine_insert_rgb24(uint8_t *buf_base_rgb24, int base_width, int base_height,
				      const uint8_t *buf_logo_rgb24, int logo_width, int logo_height,
				      int x, int y,
				      const struct custom_mpix_combine_config *config)
{
	static const struct custom_mpix_combine_config no_transparency = {.use_chroma_key = false};

	if (!buf_base_rgb24 || !buf_logo_rgb24) {
		LOG_ERR("combine_insert: NULL argument");
		return -EINVAL;
	}
	if (base_width <= 0 || base_height <= 0 || logo_width <= 0 || logo_height <= 0) {
		LOG_ERR("combine_insert: invalid size base=%dx%d logo=%dx%d",
			base_width, base_height, logo_width, logo_height);
		return -EINVAL;
	}
	if (x < 0 || y < 0 || x + logo_width > base_width || y + logo_height > base_height) {
		LOG_ERR("combine_insert: logo %dx%d at (%d,%d) doesn't fit in base %dx%d",
			logo_width, logo_height, x, y, base_width, base_height);
		return -EINVAL;
	}

	if (!config) {
		config = &no_transparency;
	}

	for (int ly = 0; ly < logo_height; ly++) {
		size_t base_row_idx = (size_t)(y + ly) * (size_t)base_width * 3;
		size_t logo_row_idx = (size_t)ly * (size_t)logo_width * 3;

		for (int lx = 0; lx < logo_width; lx++) {
			size_t logo_idx = logo_row_idx + (size_t)lx * 3;
			uint8_t r = buf_logo_rgb24[logo_idx + CUSTOM_MPIX_RGB24_R_OFFSET];
			uint8_t g = buf_logo_rgb24[logo_idx + CUSTOM_MPIX_RGB24_G_OFFSET];
			uint8_t b = buf_logo_rgb24[logo_idx + CUSTOM_MPIX_RGB24_B_OFFSET];

			if (config->use_chroma_key &&
			    r >= config->chroma_key_r &&
			    g >= config->chroma_key_g &&
			    b >= config->chroma_key_b) {
				/* Пиксель "прозрачный" - оставляем базовый кадр как есть. */
				continue;
			}

			size_t base_idx = base_row_idx + (size_t)(x + lx) * 3;

			buf_base_rgb24[base_idx + CUSTOM_MPIX_RGB24_R_OFFSET] = r;
			buf_base_rgb24[base_idx + CUSTOM_MPIX_RGB24_G_OFFSET] = g;
			buf_base_rgb24[base_idx + CUSTOM_MPIX_RGB24_B_OFFSET] = b;
		}
	}

	return 0;
}
