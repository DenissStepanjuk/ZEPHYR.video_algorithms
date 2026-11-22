#ifndef CUSTOM_MPIX_COLOR_RECOGNIZE_H_
#define CUSTOM_MPIX_COLOR_RECOGNIZE_H_

#include <stdint.h>
#include <stddef.h>

/* struct custom_mpix_roi объявлена в custom_mpix_Hough_transform_circle.h — переиспользуем
 * её здесь, а не дублируем тип с тем же именем (см. аналогичный комментарий в
 * custom_mpix_Hough_transform_line.h). Лёгкий include, без .c-зависимостей. */
#include "custom_mpix_Hough_transform_circle.h"

/* ---------------------------------------------------------------------------------------------
 *       Распознавание цвета по участку кадра: стандартизация признаков (INT8) + KNN
 *
 * База — ваш detectColor() из colorDetector.h (усреднение R,G,B по участку кадра), а сама
 * классификация перенесена из проекта для датчика GY-31 TCS230 (03_KNN_RGB):
 *
 *   1. Датасет — 10 цветов x 10 образцов в СЫРЫХ RGB 0..255 (custom_mpix_default_color_palette).
 *   2. custom_mpix_color_knn_init() (один раз при старте потока) считает по датасету для каждого
 *      канала среднее mu и стандартное отклонение sigma (делитель n, как в sklearn
 *      StandardScaler), затем масштабирует датасет в INT8:
 *
 *            q = clamp( round( (x - mu) / sigma * scale ), -127, 127 )
 *
 *      scale подбирается АВТОМАТИЧЕСКИ: scale = 127 / max|z| по всему датасету, то есть самый
 *      "дальний" образец ложится ровно в +-127 и переполнения int8 нет (в ноутбуке GY-31 был
 *      фиксированный множитель 127.5, который на данных с WHITE и BLACK переполняет int8).
 *   3. custom_mpix_color_recognize() усредняет RGB по ROI, масштабирует среднее ТЕМИ ЖЕ
 *      mu/sigma/scale и ищет K ближайших образцов (квадрат евклидова расстояния, sqrt не нужен).
 *      Класс определяется голосованием; при равенстве голосов побеждает класс с меньшей
 *      суммой расстояний до его голосующих образцов.
 *
 * float используется ТОЛЬКО в custom_mpix_color_knn_init() (sqrtf для sigma, один раз).
 * В рантайме (по каждому кадру) — только целочисленная арифметика (фиксированная точка).
 * --------------------------------------------------------------------------------------------- */

/* Смещения байт R/G/B внутри пикселя MPIX_FMT_RGB24 — согласовано с остальными custom_mpix_*
 * модулями, проверьте по факту на своём кадре, если цвета "не сходятся". */
#define CUSTOM_MPIX_RGB24_R_OFFSET 0
#define CUSTOM_MPIX_RGB24_G_OFFSET 1
#define CUSTOM_MPIX_RGB24_B_OFFSET 2

/* Количество признаков (R, G, B). */
#define CUSTOM_MPIX_COLOR_FEATURES          3
/* Количество цветов (классов) в палитре. */
#define CUSTOM_MPIX_COLOR_COUNT             10
/* Количество образцов на каждый цвет. */
#define CUSTOM_MPIX_COLOR_SAMPLES_PER_COLOR 10
/* Количество ближайших соседей K. */
#define CUSTOM_MPIX_COLOR_KNN_K             3

/* Один цвет датасета: имя и его образцы (сырые RGB 0..255, порядок R, G, B). */
struct custom_mpix_named_color {
	const char *name;
	uint8_t samples[CUSTOM_MPIX_COLOR_SAMPLES_PER_COLOR][CUSTOM_MPIX_COLOR_FEATURES];
};

/* Датасет "из коробки": 10 цветов x 10 образцов. Сейчас значения — ЗАГЛУШКИ вокруг прежних
 * эталонов; замените их образцами, снятыми с вашей камеры. */
extern const struct custom_mpix_named_color custom_mpix_default_color_palette[CUSTOM_MPIX_COLOR_COUNT];

/* Подготовленный классификатор: параметры стандартизации и датасет в INT8.
 * Размещайте статически (не на стеке потока). Заполняется custom_mpix_color_knn_init(). */
struct custom_mpix_color_knn {
	/* Датасет, на основе которого построен классификатор (нужен для имён цветов). */
	const struct custom_mpix_named_color *palette;
	size_t color_count;

	/* Среднее mu по каждому каналу в формате Q8 (mu * 256). */
	int32_t mean_q8[CUSTOM_MPIX_COLOR_FEATURES];
	/* Множитель (scale / sigma) по каждому каналу в формате Q16. */
	int32_t mul_q16[CUSTOM_MPIX_COLOR_FEATURES];

	/* Отмасштабированный в INT8 датасет. */
	int8_t samples[CUSTOM_MPIX_COLOR_COUNT][CUSTOM_MPIX_COLOR_SAMPLES_PER_COLOR]
		      [CUSTOM_MPIX_COLOR_FEATURES];

	/* 1 после успешной инициализации. */
	uint8_t ready;
};

/* Результат распознавания цвета на одном ROI. */
struct custom_mpix_color_result {
	/* Усреднённые по ROI компоненты RGB. */
	uint8_t r_avg;
	uint8_t g_avg;
	uint8_t b_avg;
	/* Индекс найденного цвета в палитре, либо -1, если цвет не определён. */
	int32_t color_index;
	/* palette[color_index].name, либо NULL, если color_index == -1. Указатель живёт столько
	 * же, сколько сама палитра (обычно статический массив — живёт всегда). */
	const char *color_name;
};

/** Подготовить классификатор: посчитать mu и sigma по сырому датасету и отмасштабировать его
 * в INT8. Вызывается один раз при старте потока обработки (float + sqrtf используются только здесь).
 *
 * knn           - куда записать подготовленный классификатор (статическая память).
 * palette       - датасет: сырые RGB 0..255 (например, custom_mpix_default_color_palette).
 * palette_count - количество цветов (<= CUSTOM_MPIX_COLOR_COUNT).
 *
 * Пишет в лог mu, sigma (x100) и автоподобранный множитель scale (x100).
 * Возвращает 0 при успехе, -EINVAL при некорректных аргументах.
 */
int custom_mpix_color_knn_init(struct custom_mpix_color_knn *knn,
			       const struct custom_mpix_named_color *palette, size_t palette_count);

/** Усреднить RGB на участке (ROI) кадра RGB24, отмасштабировать среднее в INT8 и определить
 * цвет алгоритмом K ближайших соседей (K = CUSTOM_MPIX_COLOR_KNN_K).
 *
 * buf_rgb24     - кадр в формате RGB24, размер width*height*3 байт.
 * width, height - размеры ВСЕГО кадра (ROI внутри должен в них помещаться).
 * roi           - участок кадра, по которому считается среднее.
 * knn           - классификатор, подготовленный custom_mpix_color_knn_init().
 * result        - куда записать результат (среднее RGB + найденный цвет).
 *
 * Печатает в лог строки вида:
 *   "Current average RGB - R_aver: .., G_aver: .., B_aver: .. "
 *   "Detected color: .."
 *
 * Возвращает 0 при успехе, -EINVAL при некорректных аргументах (NULL, width/height <= 0,
 * вырожденный или выходящий за кадр ROI, не инициализированный knn).
 */
int custom_mpix_color_recognize(const uint8_t *buf_rgb24, int width, int height,
				const struct custom_mpix_roi *roi,
				const struct custom_mpix_color_knn *knn,
				struct custom_mpix_color_result *result);

#endif /* CUSTOM_MPIX_COLOR_RECOGNIZE_H_ */

















// #ifndef CUSTOM_MPIX_COLOR_RECOGNIZE_H_
// #define CUSTOM_MPIX_COLOR_RECOGNIZE_H_

// #include <stdint.h>
// #include <stddef.h>

// /* struct custom_mpix_roi объявлена в custom_mpix_Hough_transform_circle.h — переиспользуем
//  * её здесь, а не дублируем тип с тем же именем (см. аналогичный комментарий в
//  * custom_mpix_Hough_transform_line.h). Лёгкий include, без .c-зависимостей. */
// #include "custom_mpix_Hough_transform_circle.h"

// /* ---------------------------------------------------------------------------------------------
//  *                  Порт распознавания цвета по участку кадра (Arduino IDE -> Zephyr)
//  *
//  * База — ваш detectColor()/getColorName()/colorDifference() из colorDetector.h: усреднить
//  * R,G,B на заданном участке кадра, затем найти ближайший цвет по евклидову расстоянию
//  * среди известных (палитра).
//  *
//  * Отличия от оригинала:
//  *   1. Убран hardcoded "1920" (ширина строки сенсора в байтах из другого проекта на 640px) —
//  *      ширина строки берётся из реального width, переданного в функцию.
//  *   2. sqrt() в colorDifference() не нужен: для поиска МИНИМУМА достаточно сравнивать квадраты
//  *      расстояний — как и в остальных custom_mpix_* модулях, без float в горячем цикле.
//  *   3. Палитра — параметр функции (указатель + количество), а не зашитый в .c массив: можно
//  *      передать свою, любой длины, не трогая исходники модуля. Внизу этого файла — готовая
//  *      палитра из 16 цветов "из коробки" (custom_mpix_default_color_palette), можно
//  *      использовать её как есть или как основу для правок.
//  *   4. Работает напрямую по RGB24 (как и остальные custom_mpix_* модули), ROI — структура
//  *      custom_mpix_roi, переиспользуемая из Hough-модулей.
//  *   5. Проверка границ ROI строгая ("<=" вместо "< +1" в оригинале) и обычные -EINVAL/LOG_ERR
//  *      при некорректных аргументах, как и во всех остальных модулях.
//  * --------------------------------------------------------------------------------------------- */

// /* Смещения байт R/G/B внутри пикселя MPIX_FMT_RGB24 — согласовано с остальными custom_mpix_*
//  * модулями, проверьте по факту на своём кадре, если цвета "не сходятся". */
// #define CUSTOM_MPIX_RGB24_R_OFFSET 0
// #define CUSTOM_MPIX_RGB24_G_OFFSET 1
// #define CUSTOM_MPIX_RGB24_B_OFFSET 2

// /* Один известный цвет палитры: имя (для лога/отображения) и эталонный RGB. */
// struct custom_mpix_named_color {
// 	const char *name;
// 	uint8_t r;
// 	uint8_t g;
// 	uint8_t b;
// };

// /* Готовая палитра "из коробки" — тот же список из 16 цветов, что уже использовался в вашем
//  * проекте. Можно передавать в custom_mpix_color_recognize() как есть, либо завести свою
//  * палитру (свой массив custom_mpix_named_color[], свою длину) и передавать её вместо этой —
//  * функция не привязана к конкретному массиву. */
// extern const struct custom_mpix_named_color custom_mpix_default_color_palette[10];

// /* Результат распознавания цвета на одном ROI. */
// struct custom_mpix_color_result {
// 	/* Усреднённые по ROI компоненты RGB. */
// 	uint8_t r_avg;
// 	uint8_t g_avg;
// 	uint8_t b_avg;
// 	/* Индекс ближайшего цвета в переданной палитре, либо -1, если palette_count == 0. */
// 	int32_t color_index;
// 	/* palette[color_index].name, либо NULL, если color_index == -1. Указатель живёт столько
// 	 * же, сколько сама переданная палитра (обычно статический массив — живёт всегда). */
// 	const char *color_name;
// };

// /** Усреднить RGB на участке (ROI) кадра RGB24 и определить ближайший цвет из палитры
//  * по евклидову расстоянию (без sqrt — сравниваются квадраты расстояний).
//  *
//  * buf_rgb24     - кадр в формате RGB24, размер width*height*3 байт.
//  * width, height - размеры ВСЕГО кадра (ROI внутри должен в них помещаться).
//  * roi           - участок кадра, по которому считается среднее.
//  * palette       - палитра известных цветов для сравнения (например,
//  *                  custom_mpix_default_color_palette).
//  * palette_count - количество цветов в palette (например, ARRAY_SIZE(custom_mpix_default_color_palette)).
//  * result        - куда записать результат (среднее RGB + найденный цвет).
//  *
//  * Печатает в лог строки вида:
//  *   "Current average RGB - R_aver: .., G_aver: .., B_aver: .. "
//  *   "Detected color: .."
//  * как и было явно указано в задаче — это часть контракта функции, а не отладочный вывод,
//  * который можно убрать без изменения поведения.
//  *
//  * Возвращает 0 при успехе, -EINVAL при некорректных аргументах (NULL-указатель,
//  * width/height <= 0, вырожденный или выходящий за кадр ROI).
//  */
// int custom_mpix_color_recognize(const uint8_t *buf_rgb24, int width, int height,
// 				 const struct custom_mpix_roi *roi,
// 				 const struct custom_mpix_named_color *palette, size_t palette_count,
// 				 struct custom_mpix_color_result *result);

// #endif /* CUSTOM_MPIX_COLOR_RECOGNIZE_H_ */
