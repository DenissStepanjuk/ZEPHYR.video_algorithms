/* Коды ошибок (-EINVAL и т.д.). */
#include <errno.h>
/* sqrtf() — используется ТОЛЬКО в custom_mpix_color_knn_init(). */
#include <math.h>
/* Регистрация модуля логирования. */
#include <zephyr/logging/log.h>

#include "custom_mpix_color_KNN.h"

LOG_MODULE_REGISTER(custom_mpix_color_KNN, LOG_LEVEL_DBG);

/* Датасет: 10 цветов x 10 образцов в СЫРЫХ RGB 0..255 (порядок R, G, B).
 * ВНИМАНИЕ: значения ниже — ЗАГЛУШКИ (прежние эталоны + небольшой разброс).
 * Замените их образцами, снятыми с камеры; порядок цветов менять можно, количество — нет
 * (CUSTOM_MPIX_COLOR_COUNT x CUSTOM_MPIX_COLOR_SAMPLES_PER_COLOR). */
const struct custom_mpix_named_color custom_mpix_default_color_palette[CUSTOM_MPIX_COLOR_COUNT] = {
	{
		.name = "WHITE",
		.samples = {
			{115, 190, 60},
			{150, 210, 90},
			{90, 30, 180},
			{30, 33, 200},
			{130, 30, 200},
			{200, 80, 160},
			{190, 160, 120},
			{90, 70, 60},
			{100, 70, 50},
			{70, 50, 110}
		},
	},
	{
		.name = "BLACK",
		.samples = {
			{  75,   180,   35},
			{  30,  160,  100},
			{  90,   150,   40},
			{  100,   140,   60},
			{  50,   190,   30},
			{  150,   50,   70},
			{  80,  170,   40},
			{  150,   100,  70},
			{  70,   190,   30},
			{  60,  200,   30}
		},
	},
	{
		.name = "RED",
		.samples = {
			{190,  90,  155},
			{120,  160,  200},
			{100,  130,  180},
			{130,  54,  60},
			{60,  130,  70},
			{200,  70,  160},
			{50,  115,  120},
			{180,  65,  140},
			{150,  54,  100},
			{140,  45,  100}
		},
	},
	{
		.name = "ORANGE",
		.samples = {
			{210, 75,   80},
			{190, 75,  50},
			{170, 70,   50},
			{90, 150,   90},
			{170, 160, 170},
			{120, 145, 135},
			{200, 190, 180},
			{160, 160, 170},
			{180, 150, 180},
			{190, 155, 180}
		},
	},
	{
		.name = "YELLOW",
		.samples = {
			{50, 75,  160},
			{45, 75,  160},
			{50, 70,  143},
			{170, 120,  120},
			{160, 115,  120},
			{115, 100,  100},
			{130, 150,  200},
			{60, 180,  70},
			{165, 230,  170},
			{65, 180,  70}
		},
	},
	{
		.name = "GREEN",
		.samples = {
			{ 200, 40,  80},
			{ 180, 40,  178},
			{ 115, 169, 150},
			{ 105, 158, 140},
			{ 70, 140,  130},
			{ 140, 140,  90},
			{ 50, 115,  61},
			{ 40, 152,  70},
			{ 45, 145,  70},
			{ 60, 100,  130}
		},
	},
	{
		.name = "BLUE",
		.samples = {
			{ 55, 150, 190},
			{ 140, 125, 90},
			{ 60, 149, 180},
			{ 50, 160, 120},
			{ 70, 160, 80},
			{ 60, 160, 180},
			{ 50, 150, 190},
			{ 115, 138, 130},
			{ 70, 170, 170},
			{ 170, 136, 110}
		},
	},
	{
		.name = "PURPLE",
		.samples = {
			{90,  45, 70},
			{160,  45, 133},
			{110,  50, 100},
			{100,  60, 100},
			{70,  39, 130},
			{90,  50, 140},
			{70,  130, 200},
			{90,  160, 52},
			{80,  50, 60},
			{100,  60, 60}
		},
	},
	{
		.name = "BROWN",
		.samples = {
			{50,  130,  180},
			{145,  100, 135},
			{170,  100,  115},
			{160,  180,  80},
			{175,  200,  70},
			{130,  180,  55},
			{170,  77,  120},
			{190,  70,  125},
			{150,  180,  70},
			{180,  190,  68}
		},
	},
	{
		.name = "GRAY",
		.samples = {
			{145, 90, 65},
			{160, 200, 60},
			{90, 180, 130},
			{90, 45, 170},
			{120, 190, 100},
			{160, 154, 190},
			{150, 90, 61},
			{130, 35, 200},
			{120, 200, 60},
			{80, 200, 90}
		},
	},
};

/* Границы INT8 после масштабирования. Симметричные (-127..127), -128 не используется. */
#define SCALED_MAX  127
#define SCALED_MIN  (-127)

/* Порог, ниже которого sigma считается нулевой (канал константный) — тогда sigma := 1,
 * т.е. остаётся только вычитание среднего. Заодно ограничивает mul_q16 сверху (int32). */
#define SIGMA_MIN_F 0.05f

/* Масштабирование одного значения признака в INT8 в целых числах:
 *   q = clamp( round( (x - mu) * (scale / sigma) ), -127, 127 )
 * x_q8 и mean_q8 — формат Q8, mul_q16 — формат Q16, произведение — Q24. int64_t нужен, т.к.
 * произведение может превысить 32 бита. Сдвиг вправо отрицательного значения — арифметический
 * (так ведут себя все компиляторы Zephyr: gcc/clang), что даёт округление floor(v + 0.5). */
static inline int8_t custom_mpix_color_scale_q8(int32_t x_q8, int32_t mean_q8, int32_t mul_q16)
{
	int64_t v = ((int64_t)x_q8 - (int64_t)mean_q8) * (int64_t)mul_q16;

	v = (v + ((int64_t)1 << 23)) >> 24;

	if (v > SCALED_MAX) {
		v = SCALED_MAX;
	} else if (v < SCALED_MIN) {
		v = SCALED_MIN;
	}

	return (int8_t)v;
}

/* Квадрат евклидова расстояния между двумя INT8-векторами (R, G, B) — sqrt() не нужен, т.к. для
 * поиска ближайших достаточно сравнивать квадраты. Максимум одного слагаемого 254*254=64516,
 * сумма трёх — 193548, далеко от переполнения int32_t. */
static inline int32_t custom_mpix_color_square_distance(const int8_t *a, const int8_t *b)
{
	int32_t d0 = (int32_t)a[0] - (int32_t)b[0];
	int32_t d1 = (int32_t)a[1] - (int32_t)b[1];
	int32_t d2 = (int32_t)a[2] - (int32_t)b[2];

	return d0 * d0 + d1 * d1 + d2 * d2;
}

int custom_mpix_color_knn_init(struct custom_mpix_color_knn *knn,
			       const struct custom_mpix_named_color *palette, size_t palette_count)
{
	float mean[CUSTOM_MPIX_COLOR_FEATURES];
	float sigma[CUSTOM_MPIX_COLOR_FEATURES];
	float max_abs_z = 0.0f;
	float scale;
	const uint32_t n = (uint32_t)(palette_count * CUSTOM_MPIX_COLOR_SAMPLES_PER_COLOR);

	/* --- Валидация аргументов --- */
	if (!knn || !palette) {
		LOG_ERR("knn_init: NULL argument");
		return -EINVAL;
	}
	if (palette_count == 0 || palette_count > CUSTOM_MPIX_COLOR_COUNT) {
		LOG_ERR("knn_init: invalid palette_count %u (max %u)", (unsigned int)palette_count,
			(unsigned int)CUSTOM_MPIX_COLOR_COUNT);
		return -EINVAL;
	}

	knn->ready = 0;
	knn->palette = palette;
	knn->color_count = palette_count;

	/* --- Среднее mu и стандартное отклонение sigma (делитель n, как в sklearn) --- */
	for (int f = 0; f < CUSTOM_MPIX_COLOR_FEATURES; f++) {
		uint32_t sum = 0;
		float sq_sum = 0.0f;

		for (size_t c = 0; c < palette_count; c++) {
			for (int s = 0; s < CUSTOM_MPIX_COLOR_SAMPLES_PER_COLOR; s++) {
				sum += palette[c].samples[s][f];
			}
		}
		mean[f] = (float)sum / (float)n;

		for (size_t c = 0; c < palette_count; c++) {
			for (int s = 0; s < CUSTOM_MPIX_COLOR_SAMPLES_PER_COLOR; s++) {
				float d = (float)palette[c].samples[s][f] - mean[f];

				sq_sum += d * d;
			}
		}
		sigma[f] = sqrtf(sq_sum / (float)n);

		if (sigma[f] < SIGMA_MIN_F) {
			LOG_WRN("knn_init: feature %d is constant, sigma set to 1", f);
			sigma[f] = 1.0f;
		}
	}

	/* --- Автоподбор множителя: самый дальний от mu образец должен лечь в +-127 --- */
	for (int f = 0; f < CUSTOM_MPIX_COLOR_FEATURES; f++) {
		for (size_t c = 0; c < palette_count; c++) {
			for (int s = 0; s < CUSTOM_MPIX_COLOR_SAMPLES_PER_COLOR; s++) {
				float z = ((float)palette[c].samples[s][f] - mean[f]) / sigma[f];

				if (z < 0.0f) {
					z = -z;
				}
				if (z > max_abs_z) {
					max_abs_z = z;
				}
			}
		}
	}
	scale = (max_abs_z > 0.0f) ? ((float)SCALED_MAX / max_abs_z) : 1.0f;

	/* --- Параметры для рантайма в фиксированной точке --- */
	for (int f = 0; f < CUSTOM_MPIX_COLOR_FEATURES; f++) {
		knn->mean_q8[f] = (int32_t)(mean[f] * 256.0f + 0.5f);
		knn->mul_q16[f] = (int32_t)(scale / sigma[f] * 65536.0f + 0.5f);
	}

	/* --- Масштабирование датасета в INT8 (тем же целочисленным путём, что и в рантайме) --- */
	for (size_t c = 0; c < palette_count; c++) {
		for (int s = 0; s < CUSTOM_MPIX_COLOR_SAMPLES_PER_COLOR; s++) {
			for (int f = 0; f < CUSTOM_MPIX_COLOR_FEATURES; f++) {
				knn->samples[c][s][f] = custom_mpix_color_scale_q8(
					(int32_t)palette[c].samples[s][f] << 8,
					knn->mean_q8[f], knn->mul_q16[f]);
			}
		}
	}

	knn->ready = 1;

	/* Печать в целых (x100), чтобы не требовать CONFIG_CBPRINTF_FP_SUPPORT. */
	LOG_INF("KNN mean x100  - R: %d, G: %d, B: %d", (int)(mean[0] * 100.0f),
		(int)(mean[1] * 100.0f), (int)(mean[2] * 100.0f));
	LOG_INF("KNN sigma x100 - R: %d, G: %d, B: %d", (int)(sigma[0] * 100.0f),
		(int)(sigma[1] * 100.0f), (int)(sigma[2] * 100.0f));
	LOG_INF("KNN INT8 scale x100: %d (max |z| x100: %d)", (int)(scale * 100.0f),
		(int)(max_abs_z * 100.0f));

	return 0;
}

int custom_mpix_color_recognize(const uint8_t *buf_rgb24, int width, int height,
				const struct custom_mpix_roi *roi,
				const struct custom_mpix_color_knn *knn,
				struct custom_mpix_color_result *result)
{
	uint64_t r_sum = 0;
	uint64_t g_sum = 0;
	uint64_t b_sum = 0;
	uint32_t pixel_count;
	uint32_t r_avg, g_avg, b_avg;
	int32_t avg_q8[CUSTOM_MPIX_COLOR_FEATURES];
	int8_t query[CUSTOM_MPIX_COLOR_FEATURES];

	/* K ближайших образцов, отсортированных по возрастанию расстояния. */
	int32_t best_dist[CUSTOM_MPIX_COLOR_KNN_K];
	uint8_t best_cls[CUSTOM_MPIX_COLOR_KNN_K];
	size_t filled = 0;
	size_t k = CUSTOM_MPIX_COLOR_KNN_K;

	/* Голоса и суммы расстояний по классам. */
	uint8_t votes[CUSTOM_MPIX_COLOR_COUNT] = {0};
	uint32_t dist_sum[CUSTOM_MPIX_COLOR_COUNT] = {0};
	int32_t best_idx = -1;

	/* --- Валидация аргументов --- */
	if (!buf_rgb24 || !roi || !result || !knn || !knn->palette) {
		LOG_ERR("color_recognize: NULL argument");
		return -EINVAL;
	}
	if (!knn->ready) {
		LOG_ERR("color_recognize: knn is not initialized");
		return -EINVAL;
	}
	if (width <= 0 || height <= 0) {
		LOG_ERR("color_recognize: invalid frame size %dx%d", width, height);
		return -EINVAL;
	}
	if (roi->x_min < 0 || roi->y_min < 0 ||
	    roi->x_max > width || roi->y_max > height ||
	    roi->x_min >= roi->x_max || roi->y_min >= roi->y_max) {
		LOG_ERR("color_recognize: ROI [%d,%d)x[%d,%d) invalid for frame %dx%d",
			roi->x_min, roi->x_max, roi->y_min, roi->y_max, width, height);
		return -EINVAL;
	}

	/* --- Усреднение RGB по ROI --- */
	for (int32_t y = roi->y_min; y < roi->y_max; y++) {
		size_t row_idx = (size_t)y * (size_t)width * 3;

		for (int32_t x = roi->x_min; x < roi->x_max; x++) {
			size_t idx = row_idx + (size_t)x * 3;

			r_sum += buf_rgb24[idx + CUSTOM_MPIX_RGB24_R_OFFSET];
			g_sum += buf_rgb24[idx + CUSTOM_MPIX_RGB24_G_OFFSET];
			b_sum += buf_rgb24[idx + CUSTOM_MPIX_RGB24_B_OFFSET];
		}
	}

	pixel_count = (uint32_t)(roi->x_max - roi->x_min) * (uint32_t)(roi->y_max - roi->y_min);

	r_avg = (uint32_t)(r_sum / pixel_count);
	g_avg = (uint32_t)(g_sum / pixel_count);
	b_avg = (uint32_t)(b_sum / pixel_count);

	LOG_INF("Current average RGB - R_aver: %u, G_aver: %u, B_aver: %u ", r_avg, g_avg, b_avg);

	result->r_avg = (uint8_t)r_avg;
	result->g_avg = (uint8_t)g_avg;
	result->b_avg = (uint8_t)b_avg;

	/* --- Масштабирование среднего в INT8 (вход — среднее в Q8, чтобы не терять дробную часть) --- */
	avg_q8[0] = (int32_t)((r_sum << 8) / pixel_count);
	avg_q8[1] = (int32_t)((g_sum << 8) / pixel_count);
	avg_q8[2] = (int32_t)((b_sum << 8) / pixel_count);

	for (int f = 0; f < CUSTOM_MPIX_COLOR_FEATURES; f++) {
		query[f] = custom_mpix_color_scale_q8(avg_q8[f], knn->mean_q8[f], knn->mul_q16[f]);
	}

	/* --- Поиск K ближайших образцов (вставка в отсортированный массив из K элементов) --- */
	if (k > knn->color_count * CUSTOM_MPIX_COLOR_SAMPLES_PER_COLOR) {
		k = knn->color_count * CUSTOM_MPIX_COLOR_SAMPLES_PER_COLOR;
	}

	for (size_t c = 0; c < knn->color_count; c++) {
		for (int s = 0; s < CUSTOM_MPIX_COLOR_SAMPLES_PER_COLOR; s++) {
			int32_t d = custom_mpix_color_square_distance(query, knn->samples[c][s]);
			size_t pos;

			if (filled < k) {
				pos = filled++;
			} else if (d < best_dist[k - 1]) {
				pos = k - 1;
			} else {
				continue;
			}

			while (pos > 0 && best_dist[pos - 1] > d) {
				best_dist[pos] = best_dist[pos - 1];
				best_cls[pos] = best_cls[pos - 1];
				pos--;
			}
			best_dist[pos] = d;
			best_cls[pos] = (uint8_t)c;
		}
	}

	/* --- Голосование: больше голосов; при равенстве — меньше сумма расстояний --- */
	for (size_t i = 0; i < filled; i++) {
		votes[best_cls[i]]++;
		dist_sum[best_cls[i]] += (uint32_t)best_dist[i];
	}

	for (size_t c = 0; c < knn->color_count; c++) {
		if (votes[c] == 0) {
			continue;
		}
		if (best_idx < 0 || votes[c] > votes[best_idx] ||
		    (votes[c] == votes[best_idx] && dist_sum[c] < dist_sum[best_idx])) {
			best_idx = (int32_t)c;
		}
	}

	result->color_index = best_idx;
	result->color_name = (best_idx >= 0) ? knn->palette[best_idx].name : NULL;

	LOG_INF("Detected color: %u", (unsigned int)best_idx);

	return 0;
}





// /* Коды ошибок (-EINVAL и т.д.). */
// #include <errno.h>
// /* Регистрация модуля логирования. */
// #include <zephyr/logging/log.h>

// #include "custom_mpix_color_recognize.h"

// LOG_MODULE_REGISTER(custom_mpix_color_recognize, LOG_LEVEL_DBG);

// /* Палитра "из коробки" — 16 цветов, уже использовавшихся в вашем проекте. */
// const struct custom_mpix_named_color custom_mpix_default_color_palette[10] = {
// 	{"WHITE",     255, 255, 255},
// 	{"BLACK",     0,   0,   0},
// 	{"RED",       229, 57,  53},
// 	{"ORANGE",    251, 140, 0},
// 	{"YELLOW",    253, 216, 53},
// 	{"GREEN",     67,  160, 71},
// 	{"BLUE",      30,  136, 229},
// 	{"PURPLE",    142, 36,  170},
// 	{"BROWN",     121, 85,  72},
// 	{"GRAY",      158, 158, 158},
// };



// /* Квадрат евклидова расстояния между двумя RGB-цветами — sqrt() не нужен, т.к. для поиска
//  * минимума достаточно сравнивать квадраты (как и в custom_mpix_Hough_transform_circle.c/
//  * custom_mpix_convolution.c). int32_t с запасом: максимум одного слагаемого 255*255=65025,
//  * сумма трёх — 195075, далеко от переполнения int32_t. */
// static inline int32_t custom_mpix_color_square_distance(uint8_t r0, uint8_t g0, uint8_t b0,
// 							  uint8_t r1, uint8_t g1, uint8_t b1)
// {
// 	int32_t dr = (int32_t)r1 - (int32_t)r0;
// 	int32_t dg = (int32_t)g1 - (int32_t)g0;
// 	int32_t db = (int32_t)b1 - (int32_t)b0;

// 	return dr * dr + dg * dg + db * db;
// }

// int custom_mpix_color_recognize(const uint8_t *buf_rgb24, int width, int height,
// 				 const struct custom_mpix_roi *roi,
// 				 const struct custom_mpix_named_color *palette, size_t palette_count,
// 				 struct custom_mpix_color_result *result)
// {
// 	uint64_t r_sum = 0;
// 	uint64_t g_sum = 0;
// 	uint64_t b_sum = 0;
// 	uint32_t r_avg, g_avg, b_avg;
// 	uint32_t pixel_count;
// 	int32_t best_idx = -1;
// 	int32_t best_dist = 0;

// 	/* --- Валидация аргументов --- */
// 	if (!buf_rgb24 || !roi || !result || (palette_count > 0 && !palette)) {
// 		LOG_ERR("color_recognize: NULL argument");
// 		return -EINVAL;
// 	}
// 	if (width <= 0 || height <= 0) {
// 		LOG_ERR("color_recognize: invalid frame size %dx%d", width, height);
// 		return -EINVAL;
// 	}
// 	if (roi->x_min < 0 || roi->y_min < 0 ||
// 	    roi->x_max > width || roi->y_max > height ||
// 	    roi->x_min >= roi->x_max || roi->y_min >= roi->y_max) {
// 		LOG_ERR("color_recognize: ROI [%d,%d)x[%d,%d) invalid for frame %dx%d",
// 			roi->x_min, roi->x_max, roi->y_min, roi->y_max, width, height);
// 		return -EINVAL;
// 	}

// 	/* --- Усреднение RGB по ROI --- */
// 	for (int32_t y = roi->y_min; y < roi->y_max; y++) {
// 		size_t row_idx = (size_t)y * (size_t)width * 3;

// 		for (int32_t x = roi->x_min; x < roi->x_max; x++) {
// 			size_t idx = row_idx + (size_t)x * 3;

// 			r_sum += buf_rgb24[idx + CUSTOM_MPIX_RGB24_R_OFFSET];
// 			g_sum += buf_rgb24[idx + CUSTOM_MPIX_RGB24_G_OFFSET];
// 			b_sum += buf_rgb24[idx + CUSTOM_MPIX_RGB24_B_OFFSET];
// 		}
// 	}

// 	pixel_count = (uint32_t)(roi->x_max - roi->x_min) * (uint32_t)(roi->y_max - roi->y_min);

// 	r_avg = (uint32_t)(r_sum / pixel_count);
// 	g_avg = (uint32_t)(g_sum / pixel_count);
// 	b_avg = (uint32_t)(b_sum / pixel_count);

// 	LOG_INF("Current average RGB - R_aver: %u, G_aver: %u, B_aver: %u ", r_avg, g_avg, b_avg);

// 	result->r_avg = (uint8_t)r_avg;
// 	result->g_avg = (uint8_t)g_avg;
// 	result->b_avg = (uint8_t)b_avg;

// 	/* --- Поиск ближайшего цвета в палитре --- */
// 	for (size_t i = 0; i < palette_count; i++) {
// 		int32_t dist = custom_mpix_color_square_distance(
// 			(uint8_t)r_avg, (uint8_t)g_avg, (uint8_t)b_avg,
// 			palette[i].r, palette[i].g, palette[i].b);

// 		if (best_idx < 0 || dist < best_dist) {
// 			best_dist = dist;
// 			best_idx = (int32_t)i;
// 		}
// 	}

// 	result->color_index = best_idx;
// 	result->color_name = (best_idx >= 0) ? palette[best_idx].name : NULL;

// 	LOG_INF("Detected color: %u", (unsigned int)best_idx);

// 	return 0;
// }
