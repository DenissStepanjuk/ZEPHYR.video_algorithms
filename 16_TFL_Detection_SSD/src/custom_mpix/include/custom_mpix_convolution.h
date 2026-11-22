#ifndef CUSTOM_MPIX_CONVOLUTION_H_
#define CUSTOM_MPIX_CONVOLUTION_H_

/* Стандартные типы фиксированного размера. */
#include <stdint.h>
/* size_t. */
#include <stddef.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------------------------
 *                              Порт свёртки (convolution.h) из Arduino IDE проекта
 *
 * Отличия от оригинала (обоснование см. в сопроводительном сообщении):
 *   1. Функции работают напрямую с RGB24-кадром (MPIX_FMT_RGB24), яркость пикселя
 *      считается "на лету" внутри окна свёртки, отдельный чёрно-белый буфер не нужен
 *      (в оригинале для этого была функция jpeg2gray()).
 *   2. Результат имеет ТОТ ЖЕ размер (width x height), что и входной кадр. Рамка в 1px
 *      по периметру кадра не обрабатывается ядром (там физически не хватает соседей) и
 *      заполняется яркостью исходного пикселя "как есть" (копирование, без затемнения).
 *   3. Никакого malloc/free и ps_malloc: результат пишется в буфер, который выделяет и
 *      владеет вызывающая сторона (статически, по аналогии с buf_out/buf_frame_4_processing
 *      в net_http_video_server.c). Функции только пишут по переданному указателю.
 *   4. Добавлен clamp результата свёртки в диапазон [0, 255] (в оригинале было прямое
 *      приведение double -> byte, которое для отрицательных/>255 значений давало
 *      переполнение/UB — актуально для ядер резкости, Собеля, Прюитта и т.д.).
 * --------------------------------------------------------------------------------------------- */

/* Смещения байт R/G/B внутри одного пикселя MPIX_FMT_RGB24.
 * ВАЖНО: проверьте это по факту (например тестовым кадром с чисто красным объектом) —
 * если цвета/яркость "не сходятся", поменяйте местами эти три define. */
#define CUSTOM_MPIX_RGB24_R_OFFSET 0
#define CUSTOM_MPIX_RGB24_G_OFFSET 1
#define CUSTOM_MPIX_RGB24_B_OFFSET 2

/* -------------------------------- Готовые ядра 3x3 (одно ядро) -------------------------------- */

/* Тождественное преобразование (кадр без изменений). */
extern const double custom_mpix_kernel_identity_3x3[3][3];
/* Гауссово размытие. */
extern const double custom_mpix_kernel_blur_3x3[3][3];
/* Увеличение резкости. */
extern const double custom_mpix_kernel_sharpen_3x3[3][3];
/* Дилатация (расширение светлых областей). */
extern const double custom_mpix_kernel_dilation_3x3[3][3];
/* Выделение вертикальных границ. */
extern const double custom_mpix_kernel_vertical_borders_3x3[3][3];
/* Выделение горизонтальных границ. */
extern const double custom_mpix_kernel_horizontal_borders_3x3[3][3];
/* Выделение границ (Лапласиан). */
extern const double custom_mpix_kernel_laplacian_borders_3x3[3][3];

/* -------------------------- Готовые парные ядра (горизонталь + вертикаль) --------------------- */

/* Оператор Робертса (перекрёстный), 2x2. */
extern const double custom_mpix_kernel_roberts_gx_2x2[2][2];
extern const double custom_mpix_kernel_roberts_gy_2x2[2][2];

/* Оператор Собеля, 3x3. */
extern const double custom_mpix_kernel_sobel_gx_3x3[3][3];
extern const double custom_mpix_kernel_sobel_gy_3x3[3][3];

/* Оператор Прюитта, 3x3. */
extern const double custom_mpix_kernel_prewitt_gx_3x3[3][3];
extern const double custom_mpix_kernel_prewitt_gy_3x3[3][3];

/* ------------------------------------------------------------------------------------------- */

/** Свернуть RGB24-кадр одним ядром 3x3 (аналог apply2DKernel_3x3 из оригинала).
 *
 * buf_rgb24_in  - входной кадр в формате RGB24, размер width*height*3 байт.
 * buf_grey_out  - буфер под результат, размер width*height байт (1 байт на пиксель).
 *                 Выделяется и владеется вызывающей стороной (статически).
 * width, height - размеры кадра в пикселях.
 * kernel        - ядро свёртки 3x3.
 *
 * Возвращает 0 при успехе, отрицательный errno-код при некорректных аргументах
 * (NULL-указатель, width/height < 3).
 */
int custom_mpix_convolve_3x3(const uint8_t *buf_rgb24_in, uint8_t *buf_grey_out,
			      int width, int height, const double kernel[3][3]);

/** Свернуть RGB24-кадр двумя ядрами 2x2 (аналог applyTwo2DKernel_2x2 из оригинала),
 * итоговый пиксель = sqrt(h^2 + v^2), где h/v - отклики горизонтального/вертикального ядра.
 * Параметры и правила по границе кадра - как у custom_mpix_convolve_3x3().
 */
int custom_mpix_convolve_two_2x2(const uint8_t *buf_rgb24_in, uint8_t *buf_grey_out,
				  int width, int height,
				  const double kernel_horizontal[2][2],
				  const double kernel_vertical[2][2]);

/** Свернуть RGB24-кадр двумя ядрами 3x3 (аналог applyTwo2DKernel_3x3 из оригинала),
 * итоговый пиксель = sqrt(h^2 + v^2), где h/v - отклики горизонтального/вертикального ядра.
 * Параметры и правила по границе кадра - как у custom_mpix_convolve_3x3().
 */
int custom_mpix_convolve_two_3x3(const uint8_t *buf_rgb24_in, uint8_t *buf_grey_out,
				  int width, int height,
				  const double kernel_horizontal[3][3],
				  const double kernel_vertical[3][3]);

/** Вспомогательная функция интеграции с конвейером: записывает одноканальный
 * (grayscale) результат свёртки обратно в RGB24-буфер того же размера,
 * дублируя яркость в R, G и B каналы (R=G=B=grey). Удобно, когда результат
 * свёртки нужно отправить дальше по тому же RGB24-конвейеру (WebSocket,
 * дисплей), который используется для остальных кадров в processing_thread_start().
 *
 * buf_grey_in    - одноканальный буфер, размер width*height байт.
 * buf_rgb24_out  - буфер под RGB24, размер width*height*3 байт, выделяется вызывающей
 *                  стороной. Может указывать на тот же кадр, из которого была взята
 *                  яркость (in-place), т.к. запись всегда "догоняет" чтение по индексу.
 */
int custom_mpix_convolution_grey_to_rgb24(const uint8_t *buf_grey_in, uint8_t *buf_rgb24_out,
					   int width, int height);

#endif /* CUSTOM_MPIX_CONVOLUTION_H_ */





/**
 *  Подавление точечного (импульсного) шума на 1-канальном градиентном кадре.
 * 
 *  buf                  Буфер кадра (Grayscale, 1 байт/пиксель).
 *  width                Ширина кадра.
 *  height               Высота кадра.
 *  neighbor_diff_thresh Порог схожести соседей (например, 15..20).
 * center_diff_thresh   Минимальное отклонение битого пикселя от соседей (например, 35..50).
 */
void custom_mpix_delete_noise_grey(uint8_t *buf, int width, int height,
                                   uint8_t neighbor_diff_thresh,
                                   uint8_t center_diff_thresh);