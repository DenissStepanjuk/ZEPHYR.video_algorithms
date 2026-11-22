/* Коды ошибок (-EINVAL и т.д.). */
#include <errno.h>
/* Пределы целочисленных типов (UINT16_MAX). */
#include <stdint.h>
/* Тип bool. */
#include <stdbool.h>
/* memset(). */
#include <string.h>
/* CLAMP(), MIN() и другие системные макросы; Z_GENERIC_SECTION (через toolchain.h). */
#include <zephyr/sys/util.h>
/* Основные типы/макросы RTOS — тянет за собой Z_GENERIC_SECTION для .ext_ram.bss. */
#include <zephyr/kernel.h>
/* Регистрация модуля логирования. */
#include <zephyr/logging/log.h>

#include "custom_mpix_Hough_transform_circle.h"

LOG_MODULE_REGISTER(custom_mpix_hough_circle, LOG_LEVEL_DBG);

/* Смещения байт R/G/B внутри пикселя MPIX_FMT_RGB24 — согласовано с custom_mpix_convolution.h,
 * проверьте по факту на своём кадре, если яркость "не сходится". */
#define CUSTOM_MPIX_RGB24_R_OFFSET 0
#define CUSTOM_MPIX_RGB24_G_OFFSET 1
#define CUSTOM_MPIX_RGB24_B_OFFSET 2

/* ------------------------------------------------ Статические буферы (PSRAM) ------------------------------------------------ */
#if defined(CONFIG_ESP32_TRUE)
	/* Пространство Хафа: по одному uint16_t-счётчику голосов на каждый пиксель ROI.
	* Индексация ЛОКАЛЬНАЯ относительно ROI: accum[(y - roi.y_min) * roi_w + (x - roi.x_min)]. */
	static uint16_t hough_accum[CUSTOM_MPIX_HOUGH_ACCUM_MAX_PIXELS]
		__attribute__((aligned(4)))
		Z_GENERIC_SECTION(.ext_ram.bss);

	/* Кэш смещений (dx, dy) контура окружности радиуса cached_radius относительно центра,
	* посчитанный целочисленным алгоритмом Брезенхэма. Пересчитывается только когда меняется
	* радиус (в вашем проекте радиус фиксированный, значит пересчёт произойдёт один раз). */
	static int16_t hough_circle_offsets[8 * CUSTOM_MPIX_HOUGH_MAX_RADIUS][2]
		Z_GENERIC_SECTION(.ext_ram.bss);
	static uint32_t hough_circle_offsets_count;
	static int32_t hough_circle_offsets_cached_radius = -1;

	/* "Сырые" пики голосования до слияния близких кандидатов в один. */
	static struct custom_mpix_circle hough_raw_candidates[CUSTOM_MPIX_HOUGH_MAX_RAW_CANDIDATES]
		Z_GENERIC_SECTION(.ext_ram.bss);
#endif /* CONFIG_ESP32_TRUE */

#if defined(CONFIG_STM32_TRUE)
	/* Пространство Хафа: по одному uint16_t-счётчику голосов на каждый пиксель ROI.
	* Индексация ЛОКАЛЬНАЯ относительно ROI: accum[(y - roi.y_min) * roi_w + (x - roi.x_min)]. */
	static uint16_t hough_accum[CUSTOM_MPIX_HOUGH_ACCUM_MAX_PIXELS];

	/* Кэш смещений (dx, dy) контура окружности радиуса cached_radius относительно центра,
	* посчитанный целочисленным алгоритмом Брезенхэма. Пересчитывается только когда меняется
	* радиус (в вашем проекте радиус фиксированный, значит пересчёт произойдёт один раз). */
	static int16_t hough_circle_offsets[8 * CUSTOM_MPIX_HOUGH_MAX_RADIUS][2];
	static uint32_t hough_circle_offsets_count;
	static int32_t hough_circle_offsets_cached_radius = -1;

	/* "Сырые" пики голосования до слияния близких кандидатов в один. */
	static struct custom_mpix_circle hough_raw_candidates[CUSTOM_MPIX_HOUGH_MAX_RAW_CANDIDATES];
#endif /* CONFIG_STM32_TRUE */
/* ------------------------------------------------ Служебные функции --------------------------------------------- */

/* Яркость пикселя (x, y) RGB24-кадра — те же веса, что и в custom_mpix_convolution.c,
 * для единообразия между модулями. Без bounds-check ради скорости: вызывающий код
 * обязан гарантировать 0 <= x < width, 0 <= y < height. */
static inline uint8_t custom_mpix_hough_pixel_luma(const uint8_t *buf_rgb24, int width, int x, int y)
{
	size_t idx = ((size_t)y * (size_t)width + (size_t)x) * 3;

	uint32_t r = buf_rgb24[idx + CUSTOM_MPIX_RGB24_R_OFFSET];
	uint32_t g = buf_rgb24[idx + CUSTOM_MPIX_RGB24_G_OFFSET];
	uint32_t b = buf_rgb24[idx + CUSTOM_MPIX_RGB24_B_OFFSET];

	return (uint8_t)((299 * r + 587 * g + 114 * b) / 1000);
}

/* Строит смещения контура окружности радиуса radius целочисленным алгоритмом Брезенхэма
 * (midpoint circle, 8-кратная симметрия) в hough_circle_offsets[]. Кэширует результат —
 * повторный вызов с тем же радиусом ничего не пересчитывает. */
static void custom_mpix_hough_build_circle_offsets(int32_t radius)
{
	int32_t x, y, err;
	uint32_t count = 0;

	if (radius == hough_circle_offsets_cached_radius) {
		/* Уже посчитано для этого радиуса на предыдущем вызове. */
		return;
	}

	x = radius;
	y = 0;
	err = 1 - radius;

	while (x >= y) {
		int32_t pts[8][2] = {
			{ x,  y}, { y,  x}, {-y,  x}, {-x,  y},
			{-x, -y}, {-y, -x}, { y, -x}, { x, -y},
		};

		for (int i = 0; i < 8; i++) {
			if (count >= ARRAY_SIZE(hough_circle_offsets)) {
				LOG_WRN("Hough: circle offsets buffer full for radius %d", radius);
				goto done;
			}
			hough_circle_offsets[count][0] = (int16_t)pts[i][0];
			hough_circle_offsets[count][1] = (int16_t)pts[i][1];
			count++;
		}

		y++;
		if (err < 0) {
			err += 2 * y + 1;
		} else {
			x--;
			err += 2 * (y - x) + 1;
		}
	}

done:
	hough_circle_offsets_count = count;
	hough_circle_offsets_cached_radius = radius;
}

/* ------------------------------------------------- Публичная функция -------------------------------------------- */

int custom_mpix_hough_circle_detect(const uint8_t *buf_rgb24, int width, int height,
				     const struct custom_mpix_hough_circle_config *cfg,
				     struct custom_mpix_circle *out_circles, uint32_t *out_count)
{
	int32_t roi_w, roi_h;
	uint32_t roi_pixels;
	uint32_t raw_count = 0;
	uint32_t out_n = 0;

	/* --- Валидация аргументов --- */
	if (!buf_rgb24 || !cfg || !out_circles || !out_count) {
		LOG_ERR("hough_circle_detect: NULL argument");
		return -EINVAL;
	}
	if (width <= 0 || height <= 0) {
		LOG_ERR("hough_circle_detect: invalid frame size %dx%d", width, height);
		return -EINVAL;
	}
	if (cfg->roi.x_min < 0 || cfg->roi.y_min < 0 ||
	    cfg->roi.x_max > width || cfg->roi.y_max > height ||
	    cfg->roi.x_min >= cfg->roi.x_max || cfg->roi.y_min >= cfg->roi.y_max) {
		LOG_ERR("hough_circle_detect: ROI [%d,%d)x[%d,%d) invalid for frame %dx%d",
			cfg->roi.x_min, cfg->roi.x_max, cfg->roi.y_min, cfg->roi.y_max,
			width, height);
		return -EINVAL;
	}
	if (cfg->radius <= 0 || cfg->radius > CUSTOM_MPIX_HOUGH_MAX_RADIUS) {
		LOG_ERR("hough_circle_detect: radius %d out of range (1..%d)",
			cfg->radius, CUSTOM_MPIX_HOUGH_MAX_RADIUS);
		return -EINVAL;
	}
	if (cfg->max_circles == 0) {
		LOG_ERR("hough_circle_detect: max_circles is 0");
		return -EINVAL;
	}

	roi_w = cfg->roi.x_max - cfg->roi.x_min;
	roi_h = cfg->roi.y_max - cfg->roi.y_min;
	roi_pixels = (uint32_t)roi_w * (uint32_t)roi_h;

	if (roi_pixels > CUSTOM_MPIX_HOUGH_ACCUM_MAX_PIXELS) {
		LOG_ERR("hough_circle_detect: ROI area %u exceeds accumulator capacity %u",
			roi_pixels, (unsigned)CUSTOM_MPIX_HOUGH_ACCUM_MAX_PIXELS);
		return -EINVAL;
	}

	/* --- Подготовка --- */
	custom_mpix_hough_build_circle_offsets(cfg->radius);
	memset(hough_accum, 0, roi_pixels * sizeof(hough_accum[0]));

	/* --- Голосование: для каждого контурного пикселя ROI "штампуем" в hough_accum
	 * окружность радиуса cfg->radius, центрированную на этом пикселе (см. п.3/4 шапки файла). --- */
	for (int32_t y = cfg->roi.y_min; y < cfg->roi.y_max; y++) {
		for (int32_t x = cfg->roi.x_min; x < cfg->roi.x_max; x++) {
			uint8_t luma = custom_mpix_hough_pixel_luma(buf_rgb24, width, x, y);

			if (luma <= cfg->edge_threshold) {
				continue;
			}

			for (uint32_t k = 0; k < hough_circle_offsets_count; k++) {
				int32_t cx = x + hough_circle_offsets[k][0];
				int32_t cy = y + hough_circle_offsets[k][1];

				/* Поточечный клиппинг: теряем только эту точку контура,
				 * а не весь голос целиком, как было в оригинале. */
				if (cx < cfg->roi.x_min || cx >= cfg->roi.x_max ||
				    cy < cfg->roi.y_min || cy >= cfg->roi.y_max) {
					continue;
				}

				uint32_t acc_idx = (uint32_t)(cy - cfg->roi.y_min) * (uint32_t)roi_w +
						    (uint32_t)(cx - cfg->roi.x_min);

				if (hough_accum[acc_idx] < UINT16_MAX) {
					hough_accum[acc_idx]++;
				}
			}
		}
	}

	/* --- Поиск пиков голосования выше порога (аналог первой половины
	 * getDotsFromHoughSpace(), но с верно восстановленными x/y из индекса). --- */
	for (uint32_t i = 0; i < roi_pixels; i++) {
		if (hough_accum[i] <= cfg->vote_threshold) {
			continue;
		}
		if (raw_count >= CUSTOM_MPIX_HOUGH_MAX_RAW_CANDIDATES) {
			LOG_WRN("hough_circle_detect: raw candidate buffer full, some peaks dropped");
			break;
		}

		/* ВАЖНО: индекс заполнялся как (y_local * roi_w + x_local), поэтому
		 * обратное преобразование — x = i % roi_w, y = i / roi_w (в оригинальном
		 * Arduino-коде тут перепутаны x и y, см. п.5 в шапке файла). */
		int32_t x_local = (int32_t)(i % (uint32_t)roi_w);
		int32_t y_local = (int32_t)(i / (uint32_t)roi_w);

		hough_raw_candidates[raw_count].x = x_local + cfg->roi.x_min;
		hough_raw_candidates[raw_count].y = y_local + cfg->roi.y_min;
		hough_raw_candidates[raw_count].radius = cfg->radius;
		hough_raw_candidates[raw_count].votes = hough_accum[i];
		raw_count++;
	}

	/* --- Слияние близких кандидатов в один (аналог второй половины
	 * getDotsFromHoughSpace()), сравнение по квадрату расстояния (без sqrt()),
	 * усреднение координат с весом по числу голосов. --- */
	int64_t merge_dist_sq = (int64_t)cfg->merge_dist * (int64_t)cfg->merge_dist;

	for (uint32_t i = 0; i < raw_count; i++) {
		bool merged = false;

		for (uint32_t j = 0; j < out_n; j++) {
			int32_t dx = hough_raw_candidates[i].x - out_circles[j].x;
			int32_t dy = hough_raw_candidates[i].y - out_circles[j].y;
			int64_t dist_sq = (int64_t)dx * dx + (int64_t)dy * dy;

			if (dist_sq < merge_dist_sq) {
				uint32_t total = (uint32_t)out_circles[j].votes +
						  (uint32_t)hough_raw_candidates[i].votes;

				out_circles[j].x = (int32_t)(((int64_t)out_circles[j].x * out_circles[j].votes +
							       (int64_t)hough_raw_candidates[i].x *
								       hough_raw_candidates[i].votes) / total);
				out_circles[j].y = (int32_t)(((int64_t)out_circles[j].y * out_circles[j].votes +
							       (int64_t)hough_raw_candidates[i].y *
								       hough_raw_candidates[i].votes) / total);
				out_circles[j].votes = (uint16_t)MIN(total, UINT16_MAX);
				merged = true;
				break;
			}
		}

		if (!merged) {
			if (out_n >= cfg->max_circles) {
				LOG_WRN("hough_circle_detect: max_circles (%u) reached, dropping rest",
					cfg->max_circles);
				break;
			}
			out_circles[out_n] = hough_raw_candidates[i];
			out_n++;
		}
	}

	*out_count = out_n;

	return 0;
}
