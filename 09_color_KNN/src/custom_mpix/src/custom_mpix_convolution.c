/* Коды ошибок (-EINVAL и т.д.). */
#include <stdlib.h>
#include <errno.h>
/* sqrt(). */
#include <math.h>
/* CLAMP() и другие системные макросы. */
#include <zephyr/sys/util.h>
/* Регистрация модуля логирования. */
#include <zephyr/logging/log.h>

#include "custom_mpix_convolution.h"

LOG_MODULE_REGISTER(custom_mpix_convolution, LOG_LEVEL_DBG);

/* ------------------------------------------------- Готовые ядра ------------------------------------------------- */

const double custom_mpix_kernel_identity_3x3[3][3] = {
	{0, 0, 0},
	{0, 1, 0},
	{0, 0, 0}
};

const double custom_mpix_kernel_blur_3x3[3][3] = {
	{0.0625, 0.125, 0.0625},
	{0.125,  0.25,  0.125},
	{0.0625, 0.125, 0.0625}
};

const double custom_mpix_kernel_sharpen_3x3[3][3] = {
	{0, -1,  0},
	{-1, 5, -1},
	{0, -1,  0}
};

const double custom_mpix_kernel_dilation_3x3[3][3] = {
	{0, 1, 0},
	{1, 1, 1},
	{0, 1, 0}
};

const double custom_mpix_kernel_vertical_borders_3x3[3][3] = {
	{0, 0,  0},
	{1, -2, 1},
	{0, 0,  0}
};

const double custom_mpix_kernel_horizontal_borders_3x3[3][3] = {
	{0,  1, 0},
	{0, -2, 0},
	{0,  1, 0}
};

const double custom_mpix_kernel_laplacian_borders_3x3[3][3] = {
	{0,  1, 0},
	{1, -4, 1},
	{0,  1, 0}
};

const double custom_mpix_kernel_roberts_gx_2x2[2][2] = {
	{1,  0},
	{0, -1}
};

const double custom_mpix_kernel_roberts_gy_2x2[2][2] = {
	{0,  1},
	{-1, 0}
};

const double custom_mpix_kernel_sobel_gx_3x3[3][3] = {
	{1, 0, -1},
	{2, 0, -2},
	{1, 0, -1}
};

const double custom_mpix_kernel_sobel_gy_3x3[3][3] = {
	{1,  2,  1},
	{0,  0,  0},
	{-1, -2, -1}
};

const double custom_mpix_kernel_prewitt_gx_3x3[3][3] = {
	{1, 0, -1},
	{1, 0, -1},
	{1, 0, -1}
};

const double custom_mpix_kernel_prewitt_gy_3x3[3][3] = {
	{1,  1,  1},
	{0,  0,  0},
	{-1, -1, -1}
};

/* ------------------------------------------------ Служебные функции --------------------------------------------- */

/* Яркость пикселя (x, y) RGB24-кадра по формуле оригинала:
 * яркость = 0.299*R + 0.587*G + 0.114*B.
 * Вызывающая сторона обязана гарантировать 0 <= x < width и 0 <= y < height —
 * функция не делает bounds-check ради скорости (вызывается в самом горячем цикле). */
static inline double custom_mpix_pixel_luma(const uint8_t *buf_rgb24, int width, int x, int y)
{
	size_t idx = ((size_t)y * (size_t)width + (size_t)x) * 3;

	double r = (double)buf_rgb24[idx + CUSTOM_MPIX_RGB24_R_OFFSET];
	double g = (double)buf_rgb24[idx + CUSTOM_MPIX_RGB24_G_OFFSET];
	double b = (double)buf_rgb24[idx + CUSTOM_MPIX_RGB24_B_OFFSET];

	return 0.299 * r + 0.587 * g + 0.114 * b;
}

/* Общая проверка аргументов для всех трёх публичных функций свёртки. */
static int custom_mpix_convolve_check_args(const uint8_t *buf_rgb24_in, uint8_t *buf_grey_out,
					    int width, int height)
{
	if (!buf_rgb24_in || !buf_grey_out) {
		LOG_ERR("convolve: NULL buffer");
		return -EINVAL;
	}
	/* Нужен минимум один "внутренний" пиксель, т.е. рамка в 1px с каждой стороны
	 * плюс хотя бы одна строка/столбец для самой свёртки. */
	if (width < 3 || height < 3) {
		LOG_ERR("convolve: frame too small (%dx%d), need at least 3x3", width, height);
		return -EINVAL;
	}
	return 0;
}

/* Заполнить рамку в 1px по периметру кадра яркостью исходного пикселя (без свёртки). */
static void custom_mpix_convolve_fill_border(const uint8_t *buf_rgb24_in, uint8_t *buf_grey_out,
					      int width, int height)
{
	/* Верхняя и нижняя строки. */
	for (int x = 0; x < width; x++) {
		buf_grey_out[x] =
			(uint8_t)custom_mpix_pixel_luma(buf_rgb24_in, width, x, 0);
		buf_grey_out[(height - 1) * width + x] =
			(uint8_t)custom_mpix_pixel_luma(buf_rgb24_in, width, x, height - 1);
	}
	/* Левый и правый столбцы (без повторной записи уже заполненных углов). */
	for (int y = 1; y < height - 1; y++) {
		buf_grey_out[y * width] =
			(uint8_t)custom_mpix_pixel_luma(buf_rgb24_in, width, 0, y);
		buf_grey_out[y * width + (width - 1)] =
			(uint8_t)custom_mpix_pixel_luma(buf_rgb24_in, width, width - 1, y);
	}
}

/* ------------------------------------------------- Публичные функции -------------------------------------------- */

int custom_mpix_convolve_3x3(const uint8_t *buf_rgb24_in, uint8_t *buf_grey_out,
			      int width, int height, const double kernel[3][3])
{
	int ret = custom_mpix_convolve_check_args(buf_rgb24_in, buf_grey_out, width, height);

	if (ret) {
		return ret;
	}

	custom_mpix_convolve_fill_border(buf_rgb24_in, buf_grey_out, width, height);

	/* Проходим по всем "внутренним" пикселям кадра (рамка в 1px уже заполнена выше). */
	for (int y = 1; y < height - 1; y++) {
		for (int x = 1; x < width - 1; x++) {
			double acc = 0.0;

			/* Окно 3x3 вокруг текущего пикселя, ky/kx - индексы строки/столбца ядра. */
			for (int ky = 0; ky < 3; ky++) {
				for (int kx = 0; kx < 3; kx++) {
					acc += kernel[ky][kx] *
					       custom_mpix_pixel_luma(buf_rgb24_in, width,
								       x + kx - 1, y + ky - 1);
				}
			}

			/* Ограничиваем результат диапазоном байта (в отличие от оригинала,
			 * где прямой каст double -> byte мог давать переполнение). */
			buf_grey_out[y * width + x] = (uint8_t)CLAMP(acc, 0, 255);
		}
	}

	return 0;
}

int custom_mpix_convolve_two_2x2(const uint8_t *buf_rgb24_in, uint8_t *buf_grey_out,
				  int width, int height,
				  const double kernel_horizontal[2][2],
				  const double kernel_vertical[2][2])
{
	int ret = custom_mpix_convolve_check_args(buf_rgb24_in, buf_grey_out, width, height);

	if (ret) {
		return ret;
	}

	custom_mpix_convolve_fill_border(buf_rgb24_in, buf_grey_out, width, height);

	for (int y = 1; y < height - 1; y++) {
		for (int x = 1; x < width - 1; x++) {
			/* Окно 2x2, "прижатое" к верхнему левому углу текущего пикселя:
			 * (x-1,y-1) (x,y-1)
			 * (x-1,y)   (x,y)
			 * - те же смещения, что и в оригинальном applyTwo2DKernel_2x2(). */
			double p00 = custom_mpix_pixel_luma(buf_rgb24_in, width, x - 1, y - 1);
			double p01 = custom_mpix_pixel_luma(buf_rgb24_in, width, x,     y - 1);
			double p10 = custom_mpix_pixel_luma(buf_rgb24_in, width, x - 1, y);
			double p11 = custom_mpix_pixel_luma(buf_rgb24_in, width, x,     y);

			double gh = kernel_horizontal[0][0] * p00 + kernel_horizontal[0][1] * p01 +
				    kernel_horizontal[1][0] * p10 + kernel_horizontal[1][1] * p11;

			double gv = kernel_vertical[0][0] * p00 + kernel_vertical[0][1] * p01 +
				    kernel_vertical[1][0] * p10 + kernel_vertical[1][1] * p11;

			double magnitude = sqrt(gh * gh + gv * gv);

			buf_grey_out[y * width + x] = (uint8_t)CLAMP(magnitude, 0, 255);
		}
	}

	return 0;
}

int custom_mpix_convolve_two_3x3(const uint8_t *buf_rgb24_in, uint8_t *buf_grey_out,
				  int width, int height,
				  const double kernel_horizontal[3][3],
				  const double kernel_vertical[3][3])
{
	int ret = custom_mpix_convolve_check_args(buf_rgb24_in, buf_grey_out, width, height);

	if (ret) {
		return ret;
	}

	custom_mpix_convolve_fill_border(buf_rgb24_in, buf_grey_out, width, height);

	for (int y = 1; y < height - 1; y++) {
		for (int x = 1; x < width - 1; x++) {
			double gh = 0.0;
			double gv = 0.0;

			for (int ky = 0; ky < 3; ky++) {
				for (int kx = 0; kx < 3; kx++) {
					double p = custom_mpix_pixel_luma(buf_rgb24_in, width,
									   x + kx - 1, y + ky - 1);

					gh += kernel_horizontal[ky][kx] * p;
					gv += kernel_vertical[ky][kx] * p;
				}
			}

			double magnitude = sqrt(gh * gh + gv * gv);

			buf_grey_out[y * width + x] = (uint8_t)CLAMP(magnitude, 0, 255);
		}
	}

	return 0;
}

int custom_mpix_convolution_grey_to_rgb24(const uint8_t *buf_grey_in, uint8_t *buf_rgb24_out,
					   int width, int height)
{
	if (!buf_grey_in || !buf_rgb24_out) {
		LOG_ERR("grey_to_rgb24: NULL buffer");
		return -EINVAL;
	}
	if (width <= 0 || height <= 0) {
		LOG_ERR("grey_to_rgb24: invalid size (%dx%d)", width, height);
		return -EINVAL;
	}

	/* Идём с конца буфера к началу: при in-place использовании (buf_rgb24_out указывает
	 * на тот же кадр, из которого исходно бралась яркость) это гарантирует, что мы не
	 * затрём ещё не прочитанные grey-пиксели, т.к. RGB24-запись на пиксель "шире"
	 * (3 байта), чем grey-чтение (1 байт), и индекс записи всегда >= индекса чтения. */
	for (size_t i = (size_t)width * (size_t)height; i-- > 0;) {
		uint8_t grey = buf_grey_in[i];

		buf_rgb24_out[i * 3 + CUSTOM_MPIX_RGB24_R_OFFSET] = grey;
		buf_rgb24_out[i * 3 + CUSTOM_MPIX_RGB24_G_OFFSET] = grey;
		buf_rgb24_out[i * 3 + CUSTOM_MPIX_RGB24_B_OFFSET] = grey;
	}

	return 0;
}











/**
 * @brief Подавление точечного (импульсного) шума на 1-канальном градиентном кадре.
 * 
 * @param buf                  Буфер кадра (Grayscale, 1 байт/пиксель).
 * @param width                Ширина кадра.
 * @param height               Высота кадра.
 * @param neighbor_diff_thresh Порог схожести соседей (например, 15..20).
 * @param center_diff_thresh   Минимальное отклонение битого пикселя от соседей (например, 35..50).
 */
void custom_mpix_delete_noise_grey(uint8_t *buf, int width, int height,
                                   uint8_t neighbor_diff_thresh,
                                   uint8_t center_diff_thresh)
{
    if (!buf || width < 3 || height < 3) {
        return;
    }

    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            int idx = y * width + x;

            int center = buf[idx];
            int left   = buf[idx - 1];
            int right  = buf[idx + 1];
            int top    = buf[idx - width];
            int bottom = buf[idx + width];

            // 1. Проверка по горизонтали: соседи слева/справа похожи, а центр сильно выбивается
            bool is_h_noise = (abs(left - right) <= neighbor_diff_thresh) &&
                              (abs(center - (left + right) / 2) > center_diff_thresh);

            // 2. Проверка по вертикали: соседи сверху/снизу похожи, а центр сильно выбивается
            bool is_v_noise = (abs(top - bottom) <= neighbor_diff_thresh) &&
                              (abs(center - (top + bottom) / 2) > center_diff_thresh);

            if (is_h_noise || is_v_noise) {
                // Заменяем битый пиксель средним значением его нормальных соседей
                buf[idx] = (uint8_t)((left + right + top + bottom) / 4);
            }
        }
    }
}