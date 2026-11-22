/*
 * custom_mpix_line_border_detect.c
 *
 * См. подробное описание в custom_mpix_line_border_detect.h.
 */

#include <stdbool.h>

#include "custom_mpix_line_border_detect.h"

/*
 * Весовые коэффициенты перевода RGB -> градации серого (формула ITU-R
 * BT.601, та же, что использовалась в исходном коде:
 *   gray = 0.114*B + 0.587*G + 0.299*R
 * ), но в целочисленной арифметике с фиксированной точкой Q8 (сумма
 * коэффициентов = 256), чтобы не тянуть float/double на каждый пиксель -
 * это заметно быстрее на MCU без аппаратного FPU (часть STM32) и не
 * медленнее на ESP32-S3.
 *
 *   77/256  ≈ 0.3008  (≈ 0.299 - R)
 *   150/256 ≈ 0.5859  (≈ 0.587 - G)
 *   29/256  ≈ 0.1133  (≈ 0.114 - B)
 */
#define LUMA_R 77
#define LUMA_G 150
#define LUMA_B 29

/*
 * Статические буферы под скан-линию. Размещены статически (.bss), а НЕ
 * как локальные массивы внутри функций, т.к. поток обработки кадра
 * (processing_thread_start) работает с очень небольшим стеком (см.
 * PROCESSING_THREAD_STACK_SIZE в main.c). Кроме того, статический буфер
 * исключает необходимость ps_malloc()/free() на каждый вызов, в отличие
 * от исходной Arduino-реализации (jpeg2gray() конвертировала весь кадр
 * в PSRAM).
 *
 * line_smoothed - скан-линия ПОСЛЕ усреднения по соседним строкам/
 *      столбцам (первый проход шумоподавления), но ДО подавления
 *      единичных выбросов.
 * line_clean    - та же скан-линия ПОСЛЕ подавления единичных выбросов
 *      (второй проход). Именно в ней ищутся границы линии.
 *
 * ВНИМАНИЕ: буферы общие для обеих функций (vline/hline) и не
 * потокобезопасны - предполагается последовательный вызов из одного
 * потока обработки кадра.
 */
static uint8_t line_smoothed[CUSTOM_MPIX_LBD_MAX_LINE_LEN];
static uint8_t line_clean[CUSTOM_MPIX_LBD_MAX_LINE_LEN];

/* Перевести один пиксель RGB24 (px[0]=R, px[1]=G, px[2]=B) в градации
 * серого. */
static inline uint8_t rgb24_pixel_to_gray(const uint8_t *px)
{
	return (uint8_t)((LUMA_R * px[0] + LUMA_G * px[1] + LUMA_B * px[2]) >> 8);
}

static inline int iabs(int v)
{
	return (v < 0) ? -v : v;
}

/*
 * ПРОХОД 1 (для ВЕРТИКАЛЬНОЙ линии): построить усреднённую по строкам
 * скан-линию line_smoothed[0..width-1] для горизонтального скана вокруг
 * строки y.
 *
 * Вместо одной строки y берём 2*CUSTOM_MPIX_LBD_SMOOTH_RADIUS + 1
 * соседних строк (y-radius .. y+radius, с отсечкой по границам кадра) и
 * усредняем яркость каждого столбца x по всем этим строкам. Разовый шум
 * одной строки (сбойный ряд пикселей матрицы, блочность JPEG и т.п.)
 * при этом гасится усреднением, а не искажает результат, как было бы
 * при работе с единственной строкой.
 *
 * Строки RGB24 лежат в памяти непрерывно (packed, без padding), поэтому
 * чтение каждой из строк остаётся кэш-дружественным - просто несколько
 * последовательных проходов вместо одного.
 */
static void build_smoothed_vline(const uint8_t *buf_rgb24, int width, int height, int y)
{
	int y_start = y - CUSTOM_MPIX_LBD_SMOOTH_RADIUS;
	int y_end = y + CUSTOM_MPIX_LBD_SMOOTH_RADIUS;

	if (y_start < 0) {
		y_start = 0;
	}
	if (y_end > height - 1) {
		y_end = height - 1;
	}

	int rows_count = y_end - y_start + 1;

	for (int x = 0; x < width; x++) {
		int sum = 0;

		for (int yy = y_start; yy <= y_end; yy++) {
			const uint8_t *row = buf_rgb24 + (size_t)yy * (size_t)width * 3;

			sum += rgb24_pixel_to_gray(row + (size_t)x * 3);
		}

		line_smoothed[x] = (uint8_t)(sum / rows_count);
	}
}

/*
 * ПРОХОД 1 (для ГОРИЗОНТАЛЬНОЙ линии): построить усреднённую по
 * столбцам скан-линию line_smoothed[0..height-1] для вертикального
 * скана вокруг столбца x. Полный аналог build_smoothed_vline(), но
 * усредняет соседние столбцы (x-radius .. x+radius) вместо строк.
 *
 * Доступ по столбцу в любом случае идёт с шагом stride = width*3 байт
 * (RGB24 хранится построчно), поэтому усреднение нескольких столбцов не
 * добавляет качественно нового вида доступа к памяти - только больше
 * таких же нелокальных чтений, что для кадра 160x120 всё равно
 * пренебрежимо дёшево по сравнению с полной конвертацией кадра.
 */
static void build_smoothed_hline(const uint8_t *buf_rgb24, int width, int height, int x)
{
	int x_start = x - CUSTOM_MPIX_LBD_SMOOTH_RADIUS;
	int x_end = x + CUSTOM_MPIX_LBD_SMOOTH_RADIUS;

	if (x_start < 0) {
		x_start = 0;
	}
	if (x_end > width - 1) {
		x_end = width - 1;
	}

	int cols_count = x_end - x_start + 1;
	const size_t stride = (size_t)width * 3;

	for (int y = 0; y < height; y++) {
		int sum = 0;
		const uint8_t *row = buf_rgb24 + (size_t)y * stride;

		for (int xx = x_start; xx <= x_end; xx++) {
			sum += rgb24_pixel_to_gray(row + (size_t)xx * 3);
		}

		line_smoothed[y] = (uint8_t)(sum / cols_count);
	}
}

/*
 * ПРОХОД 2 (общий для vline/hline): подавить единичные "выбитые"
 * пиксели вдоль уже усреднённой скан-линии line_smoothed[0..len-1],
 * записав результат в line_clean[0..len-1].
 *
 * Идея - адаптация deleteNoise() из другого вашего проекта под 1D
 * сигнал: если пиксель сильно отличается от соседа слева и от соседа
 * справа, а сами соседи слева и справа между собой похожи (в пределах
 * небольшого порога) - значит именно ЦЕНТРАЛЬНЫЙ пиксель аномален
 * (шумовой "укол"), а не граница линии (настоящая граница даёт разницу
 * между левым и правым соседом тоже, а не только с центром). Такой
 * пиксель заменяется средним значением своих соседей.
 *
 * Крайние пиксели (i=0 и i=len-1) не имеют пары соседей с обеих сторон
 * и копируются как есть.
 *
 * ВАЖНО: читаем всегда из line_smoothed (оригинал), а пишем в line_clean
 * (отдельный буфер) - если бы фильтрация шла "на месте" в одном
 * массиве, соседние проверки начали бы видеть уже изменённые соседние
 * значения, что даёт непредсказуемый каскадный эффект при нескольких
 * выбросах подряд. Раздельные буферы гарантируют, что каждый пиксель
 * оценивается строго по своим ОРИГИНАЛЬНЫМ соседям.
 */
static void despike_line(int len)
{
	if (len <= 0) {
		return;
	}

	line_clean[0] = line_smoothed[0];

	for (int i = 1; i < len - 1; i++) {
		int center = line_smoothed[i];
		int left = line_smoothed[i - 1];
		int right = line_smoothed[i + 1];

		bool is_spike = (iabs(center - left) >= CUSTOM_MPIX_LBD_SPIKE_DIFF_THRESHOLD) &&
				(iabs(center - right) >= CUSTOM_MPIX_LBD_SPIKE_DIFF_THRESHOLD) &&
				(iabs(left - right) <= CUSTOM_MPIX_LBD_SPIKE_NEIGHBOR_SIMILARITY);

		line_clean[i] = is_spike ? (uint8_t)((left + right) / 2) : (uint8_t)center;
	}

	if (len > 1) {
		line_clean[len - 1] = line_smoothed[len - 1];
	}
}

/*
 * Поиск границ по уже очищенной скан-линии line_clean[0 .. len-1].
 * Логика 1:1 повторяет findBorders() из Arduino-проекта автора (см.
 * lineBordersDetection.h): пробегаем по всем соседним парам пикселей
 * скан-линии, считаем diff = line_clean[i] - line_clean[i+1], запоминаем
 * координату наибольшего положительного перепада (leftBorder) и
 * наибольшего отрицательного перепада (rightBorder). Никакой встроенной
 * фильтрации/порогов на сам перепад - как и в оригинале; вся борьба с
 * шумом сделана раньше, на уровне сигнала (build_smoothed_* +
 * despike_line), а не здесь.
 *
 * ЕДИНСТВЕННОЕ отличие от оригинального findBorders() - способ адресации
 * буфера. В оригинале использовалось
 *   buf[verticalCoordinate * height + i]
 * что при width != height (в этом проекте 160 != 120) адресует не ту
 * строку/столбец (баг). Здесь этот класс ошибок исключён в принципе:
 * нужная строка/столбец заранее выгружена в компактный локальный буфер,
 * и обход идёт по локальным индексам 0..len-1, без домножения на
 * width/height.
 */
static void find_borders_in_clean_line(int len, int *leftBorder, int *center, int *rightBorder,
					int *diffValuesArray)
{
	int diffValue = 0;
	int minValue = 0;
	int maxValue = 0;

	*leftBorder = 0;
	*rightBorder = 0;

	for (int i = 0; i < len - 1; i++) {
		diffValue = (int)line_clean[i] - (int)line_clean[i + 1];

		if (diffValuesArray != NULL) {
			diffValuesArray[i] = diffValue;
		}

		if (minValue > diffValue) {
			minValue = diffValue;
			*rightBorder = i;
		}

		if (maxValue < diffValue) {
			maxValue = diffValue;
			*leftBorder = i;
		}
	}

	*center = (*leftBorder + *rightBorder) / 2;
}

void custom_mpix_find_vline_borders(const uint8_t *buf_rgb24, int width, int height,
				     int verticalCoordinate, int *leftBorder, int *center,
				     int *rightBorder, int *diffValuesArray)
{
	if (buf_rgb24 == NULL || leftBorder == NULL || center == NULL || rightBorder == NULL) {
		return;
	}
	if (width <= 1 || width > CUSTOM_MPIX_LBD_MAX_LINE_LEN) {
		return;
	}
	if (verticalCoordinate < 0 || verticalCoordinate >= height) {
		return;
	}

	build_smoothed_vline(buf_rgb24, width, height, verticalCoordinate);
	despike_line(width);
	find_borders_in_clean_line(width, leftBorder, center, rightBorder, diffValuesArray);
}

void custom_mpix_find_hline_borders(const uint8_t *buf_rgb24, int width, int height,
				     int horizontalCoordinate, int *topBorder, int *center,
				     int *bottomBorder, int *diffValuesArray)
{
	if (buf_rgb24 == NULL || topBorder == NULL || center == NULL || bottomBorder == NULL) {
		return;
	}
	if (height <= 1 || height > CUSTOM_MPIX_LBD_MAX_LINE_LEN) {
		return;
	}
	if (horizontalCoordinate < 0 || horizontalCoordinate >= width) {
		return;
	}

	build_smoothed_hline(buf_rgb24, width, height, horizontalCoordinate);
	despike_line(height);
	find_borders_in_clean_line(height, topBorder, center, bottomBorder, diffValuesArray);
}