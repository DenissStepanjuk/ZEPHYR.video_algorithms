/* Коды ошибок (-EINVAL и т.д.). */
#include <errno.h>
/* Пределы целочисленных типов (UINT16_MAX). */
#include <stdint.h>
/* Тип bool. */
#include <stdbool.h>
/* memset(). */
#include <string.h>
/* sqrt(), cos(), sin(), ceil(), lround(), lroundf(), fabs(). */
#include <math.h>
/* abs(). */
#include <stdlib.h>
/* CLAMP(), MIN() и другие системные макросы. */
#include <zephyr/sys/util.h>
/* Основные типы/макросы RTOS — тянет за собой Z_GENERIC_SECTION для .ext_ram.bss. */
#include <zephyr/kernel.h>
/* Регистрация модуля логирования. */
#include <zephyr/logging/log.h>

#include "custom_mpix_Hough_transform_line.h"

LOG_MODULE_REGISTER(custom_mpix_hough_line, LOG_LEVEL_DBG);

/* Смещения байт R/G/B внутри пикселя MPIX_FMT_RGB24 — согласовано с остальными custom_mpix_*
 * модулями, проверьте по факту на своём кадре, если яркость "не сходится". */
#define CUSTOM_MPIX_RGB24_R_OFFSET 0
#define CUSTOM_MPIX_RGB24_G_OFFSET 1
#define CUSTOM_MPIX_RGB24_B_OFFSET 2

/* Своя константа PI вместо M_PI: наличие M_PI в <math.h> зависит от флагов feature-test
 * макросов конкретного toolchain'а, поэтому не полагаемся на неё (самодостаточность модуля,
 * как и у остальных custom_mpix_* файлов). */
#define CUSTOM_MPIX_PI 3.14159265358979323846

/* ------------------------------------------------ Статические буферы (PSRAM) ------------------------------------------------ */
#if defined(CONFIG_ESP32_TRUE)
	/* Пространство Хафа: hough_accum[theta_idx * diag_row + (r + diag_row/2)].
	* diag_row (ширина одной "строки" по R) вычисляется на каждый вызов под конкретный ROI,
	* но не может превышать CUSTOM_MPIX_HOUGH_LINE_ACCUM_MAX_DIAGONAL — буфер рассчитан на
	* максимум по обоим измерениям сразу. */
	static uint16_t hough_line_accum[CUSTOM_MPIX_HOUGH_LINE_MAX_THETA_COUNT *
					CUSTOM_MPIX_HOUGH_LINE_ACCUM_MAX_DIAGONAL]
		__attribute__((aligned(4)))
		Z_GENERIC_SECTION(.ext_ram.bss);

	/* Кэш таблицы cos(theta)/sin(theta) (theta уже переведён в радианы, см. п.1 в шапке .h) —
	* пересчитывается только при смене theta_step_deg между вызовами (у вас шаг фиксированный,
	* значит пересчёт произойдёт один раз). */
	static float hough_line_cos_table[CUSTOM_MPIX_HOUGH_LINE_MAX_THETA_COUNT]
		Z_GENERIC_SECTION(.ext_ram.bss);
	static float hough_line_sin_table[CUSTOM_MPIX_HOUGH_LINE_MAX_THETA_COUNT]
		Z_GENERIC_SECTION(.ext_ram.bss);
	static uint32_t hough_line_theta_count;
	static int32_t hough_line_cached_theta_step_deg = -1;

	/* "Сырые" пики голосования до слияния близких кандидатов в одну линию. */
	static struct custom_mpix_line hough_line_raw_candidates[CUSTOM_MPIX_HOUGH_LINE_MAX_RAW_CANDIDATES]
		Z_GENERIC_SECTION(.ext_ram.bss);
#endif /* CONFIG_ESP32_TRUE */

#if defined(CONFIG_STM32_TRUE)
	/* Пространство Хафа: hough_accum[theta_idx * diag_row + (r + diag_row/2)].
	* diag_row (ширина одной "строки" по R) вычисляется на каждый вызов под конкретный ROI,
	* но не может превышать CUSTOM_MPIX_HOUGH_LINE_ACCUM_MAX_DIAGONAL — буфер рассчитан на
	* максимум по обоим измерениям сразу. */
	static uint16_t hough_line_accum[CUSTOM_MPIX_HOUGH_LINE_MAX_THETA_COUNT *
					CUSTOM_MPIX_HOUGH_LINE_ACCUM_MAX_DIAGONAL];

	/* Кэш таблицы cos(theta)/sin(theta) (theta уже переведён в радианы, см. п.1 в шапке .h) —
	* пересчитывается только при смене theta_step_deg между вызовами (у вас шаг фиксированный,
	* значит пересчёт произойдёт один раз). */
	static float hough_line_cos_table[CUSTOM_MPIX_HOUGH_LINE_MAX_THETA_COUNT];
	static float hough_line_sin_table[CUSTOM_MPIX_HOUGH_LINE_MAX_THETA_COUNT];
	static uint32_t hough_line_theta_count;
	static int32_t hough_line_cached_theta_step_deg = -1;

	/* "Сырые" пики голосования до слияния близких кандидатов в одну линию. */
	static struct custom_mpix_line hough_line_raw_candidates[CUSTOM_MPIX_HOUGH_LINE_MAX_RAW_CANDIDATES];
#endif /* CONFIG_STM32_TRUE */
/* ------------------------------------------------ Служебные функции --------------------------------------------- */

/* Яркость пикселя (x, y) RGB24-кадра — те же веса, что и в остальных custom_mpix_* модулях.
 * Без bounds-check ради скорости: вызывающий код обязан гарантировать 0<=x<width, 0<=y<height. */
static inline uint8_t custom_mpix_hough_line_pixel_luma(const uint8_t *buf_rgb24, int width, int x, int y)
{
	size_t idx = ((size_t)y * (size_t)width + (size_t)x) * 3;

	uint32_t r = buf_rgb24[idx + CUSTOM_MPIX_RGB24_R_OFFSET];
	uint32_t g = buf_rgb24[idx + CUSTOM_MPIX_RGB24_G_OFFSET];
	uint32_t b = buf_rgb24[idx + CUSTOM_MPIX_RGB24_B_OFFSET];

	return (uint8_t)((299 * r + 587 * g + 114 * b) / 1000);
}

/* Строит таблицу cos(theta)/sin(theta) для theta = -90..90 с шагом theta_step_deg,
 * ПРАВИЛЬНО переводя градусы в радианы (в отличие от оригинала — см. п.1 в шапке .h).
 * Кэширует результат — повторный вызов с тем же шагом ничего не пересчитывает. */
static int custom_mpix_hough_line_build_theta_table(int32_t theta_step_deg)
{
	uint32_t count = 0;

	if (theta_step_deg == hough_line_cached_theta_step_deg) {
		/* Уже посчитано для этого шага на предыдущем вызове. */
		return 0;
	}

	for (int32_t theta_deg = -90; theta_deg <= 90; theta_deg += theta_step_deg) {
		if (count >= CUSTOM_MPIX_HOUGH_LINE_MAX_THETA_COUNT) {
			LOG_ERR("hough_line: theta_step_deg=%d needs more than %d entries",
				theta_step_deg, CUSTOM_MPIX_HOUGH_LINE_MAX_THETA_COUNT);
			return -EINVAL;
		}

		double theta_rad = (double)theta_deg * (CUSTOM_MPIX_PI / 180.0);

		hough_line_cos_table[count] = (float)cos(theta_rad);
		hough_line_sin_table[count] = (float)sin(theta_rad);
		count++;
	}

	hough_line_theta_count = count;
	hough_line_cached_theta_step_deg = theta_step_deg;

	return 0;
}

/* ------------------------------------------------- Публичные функции -------------------------------------------- */

int custom_mpix_hough_line_detect(const uint8_t *buf_rgb24, int width, int height,
				   const struct custom_mpix_hough_line_config *cfg,
				   struct custom_mpix_line *out_lines, uint32_t *out_count)
{
	int32_t roi_w, roi_h;
	int32_t diag_row;   /* Ширина одной "строки" пространства Хафа (по оси R). */
	int32_t diag_half;  /* Смещение, центрирующее R=0 в середине строки. */
	uint32_t raw_count = 0;
	uint32_t out_n = 0;
	int ret;

	/* --- Валидация аргументов --- */
	if (!buf_rgb24 || !cfg || !out_lines || !out_count) {
		LOG_ERR("hough_line_detect: NULL argument");
		return -EINVAL;
	}
	if (width <= 0 || height <= 0) {
		LOG_ERR("hough_line_detect: invalid frame size %dx%d", width, height);
		return -EINVAL;
	}
	if (cfg->roi.x_min < 0 || cfg->roi.y_min < 0 ||
	    cfg->roi.x_max > width || cfg->roi.y_max > height ||
	    cfg->roi.x_min >= cfg->roi.x_max || cfg->roi.y_min >= cfg->roi.y_max) {
		LOG_ERR("hough_line_detect: ROI [%d,%d)x[%d,%d) invalid for frame %dx%d",
			cfg->roi.x_min, cfg->roi.x_max, cfg->roi.y_min, cfg->roi.y_max,
			width, height);
		return -EINVAL;
	}
	if (cfg->theta_step_deg <= 0) {
		LOG_ERR("hough_line_detect: theta_step_deg must be > 0");
		return -EINVAL;
	}
	if (cfg->max_lines == 0) {
		LOG_ERR("hough_line_detect: max_lines is 0");
		return -EINVAL;
	}

	roi_w = cfg->roi.x_max - cfg->roi.x_min;
	roi_h = cfg->roi.y_max - cfg->roi.y_min;

	/* Диагональ ROI, удвоенная — тот же приём, что и в оригинале: R естественным образом
	 * лежит в диапазоне [-ceil(sqrt(w^2+h^2)), +ceil(sqrt(w^2+h^2))], а удвоенное значение,
	 * поделённое пополам (diag_half), даёт ровно нужное центрирующее смещение. */
	diag_half = (int32_t)ceil(sqrt((double)roi_w * roi_w + (double)roi_h * roi_h));
	diag_row = 2 * diag_half;

	if (diag_row > CUSTOM_MPIX_HOUGH_LINE_ACCUM_MAX_DIAGONAL) {
		LOG_ERR("hough_line_detect: ROI diagonal %d exceeds accumulator capacity %d",
			diag_row, CUSTOM_MPIX_HOUGH_LINE_ACCUM_MAX_DIAGONAL);
		return -EINVAL;
	}

	/* --- Подготовка --- */
	ret = custom_mpix_hough_line_build_theta_table(cfg->theta_step_deg);
	if (ret) {
		return ret;
	}
	memset(hough_line_accum, 0,
	       (size_t)hough_line_theta_count * (size_t)diag_row * sizeof(hough_line_accum[0]));

	/* --- Голосование: для каждого контурного пикселя ROI и каждого theta из таблицы
	 * считаем R = x*cos(theta) + y*sin(theta) и голосуем в hough_line_accum. --- */
	for (int32_t y = cfg->roi.y_min; y < cfg->roi.y_max; y++) {
		for (int32_t x = cfg->roi.x_min; x < cfg->roi.x_max; x++) {
			uint8_t luma = custom_mpix_hough_line_pixel_luma(buf_rgb24, width, x, y);

			if (luma <= cfg->edge_threshold) {
				continue;
			}

			for (uint32_t t = 0; t < hough_line_theta_count; t++) {
				float r_f = (float)x * hough_line_cos_table[t] +
					    (float)y * hough_line_sin_table[t];
				int32_t r = (int32_t)lroundf(r_f);
				int32_t r_idx = r + diag_half;

				if (r_idx < 0 || r_idx >= diag_row) {
					/* Теоретически не должно случаться при корректном
					 * diag_half, но перестрахуемся от ошибок округления
					 * на самой границе диапазона. */
					continue;
				}

				uint32_t acc_idx = t * (uint32_t)diag_row + (uint32_t)r_idx;

				if (hough_line_accum[acc_idx] < UINT16_MAX) {
					hough_line_accum[acc_idx]++;
				}
			}
		}
	}

	/* --- Для каждого theta идём по оси R и подавляем связные "runs" точек выше порога
	 * до одного представителя — с наибольшим числом голосов внутри run (аналог того,
	 * что делает getDotsFromHoughSpace() с "отрезками" в оригинале, но с явной проверкой
	 * границ буфера — см. п.3 в шапке .h). --- */
	for (uint32_t t = 0; t < hough_line_theta_count; t++) {
		uint32_t row_base = t * (uint32_t)diag_row;
		int32_t i = 0;

		while (i < diag_row) {
			if (hough_line_accum[row_base + (uint32_t)i] <= cfg->vote_threshold) {
				i++;
				continue;
			}

			/* Нашли начало run'а — идём вперёд, пока значения выше порога,
			 * запоминая индекс с максимальным числом голосов внутри run'а. */
			int32_t run_start = i;
			int32_t best_idx = i;
			uint16_t best_votes = hough_line_accum[row_base + (uint32_t)i];

			while (i < diag_row &&
			       hough_line_accum[row_base + (uint32_t)i] > cfg->vote_threshold) {
				if (hough_line_accum[row_base + (uint32_t)i] > best_votes) {
					best_votes = hough_line_accum[row_base + (uint32_t)i];
					best_idx = i;
				}
				i++;
			}
			(void)run_start;

			if (raw_count >= CUSTOM_MPIX_HOUGH_LINE_MAX_RAW_CANDIDATES) {
				LOG_WRN("hough_line_detect: raw candidate buffer full, "
					"some peaks dropped");
				goto peaks_done;
			}

			hough_line_raw_candidates[raw_count].theta_deg =
				-90 + (int32_t)t * cfg->theta_step_deg;
			hough_line_raw_candidates[raw_count].r = best_idx - diag_half;
			hough_line_raw_candidates[raw_count].votes = best_votes;
			raw_count++;
		}
	}
peaks_done:

	/* --- Слияние похожих линий: сравниваем параметры (theta, R) напрямую (O(1) на пару),
	 * а не полный профиль y(x) как в оригинале (см. п.5 в шапке .h). --- */
	for (uint32_t i = 0; i < raw_count; i++) {
		bool merged = false;

		for (uint32_t j = 0; j < out_n; j++) {
			int32_t dtheta = hough_line_raw_candidates[i].theta_deg - out_lines[j].theta_deg;
			int32_t dr = hough_line_raw_candidates[i].r - out_lines[j].r;

			if (abs(dtheta) <= cfg->merge_theta_deg && abs(dr) <= cfg->merge_r) {
				uint32_t total = (uint32_t)out_lines[j].votes +
						  (uint32_t)hough_line_raw_candidates[i].votes;

				out_lines[j].theta_deg = (int32_t)
					(((int64_t)out_lines[j].theta_deg * out_lines[j].votes +
					  (int64_t)hough_line_raw_candidates[i].theta_deg *
						  hough_line_raw_candidates[i].votes) / total);
				out_lines[j].r = (int32_t)
					(((int64_t)out_lines[j].r * out_lines[j].votes +
					  (int64_t)hough_line_raw_candidates[i].r *
						  hough_line_raw_candidates[i].votes) / total);
				out_lines[j].votes = (uint16_t)MIN(total, UINT16_MAX);
				merged = true;
				break;
			}
		}

		if (!merged) {
			if (out_n >= cfg->max_lines) {
				LOG_WRN("hough_line_detect: max_lines (%u) reached, dropping rest",
					cfg->max_lines);
				break;
			}
			out_lines[out_n] = hough_line_raw_candidates[i];
			out_n++;
		}
	}

	*out_count = out_n;

	return 0;
}

int custom_mpix_hough_line_to_segment(const struct custom_mpix_line *line,
				       const struct custom_mpix_roi *roi,
				       int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1)
{
	double theta_rad;
	double c, s;

	if (!line || !roi || !x0 || !y0 || !x1 || !y1) {
		LOG_ERR("hough_line_to_segment: NULL argument");
		return -EINVAL;
	}
	if (line->theta_deg < -90 || line->theta_deg > 90) {
		LOG_ERR("hough_line_to_segment: theta_deg=%d out of range", line->theta_deg);
		return -EINVAL;
	}
	if (roi->x_min >= roi->x_max || roi->y_min >= roi->y_max) {
		LOG_ERR("hough_line_to_segment: degenerate ROI");
		return -EINVAL;
	}

	theta_rad = (double)line->theta_deg * (CUSTOM_MPIX_PI / 180.0);
	c = cos(theta_rad);
	s = sin(theta_rad);

	/* x*cos(theta) + y*sin(theta) = r. Выбираем "безопасную" переменную для итерации —
	 * ту, чей коэффициент по модулю больше — чтобы не делить на близкое к нулю значение
	 * (при theta около 0 линия почти вертикальна: sin(theta)~0, решаем относительно x;
	 * при theta около +-90 линия почти горизонтальна: cos(theta)~0, решаем относительно y). */
	if (fabs(s) >= fabs(c)) {
		/* Линия скорее горизонтальная — идём по x, находим y. */
		int32_t xa = roi->x_min;
		int32_t xb = roi->x_max - 1;

		*x0 = xa;
		*y0 = (int32_t)lround(((double)line->r - (double)xa * c) / s);
		*x1 = xb;
		*y1 = (int32_t)lround(((double)line->r - (double)xb * c) / s);
	} else {
		/* Линия скорее вертикальная — идём по y, находим x. */
		int32_t ya = roi->y_min;
		int32_t yb = roi->y_max - 1;

		*y0 = ya;
		*x0 = (int32_t)lround(((double)line->r - (double)ya * s) / c);
		*y1 = yb;
		*x1 = (int32_t)lround(((double)line->r - (double)yb * s) / c);
	}

	return 0;
}
