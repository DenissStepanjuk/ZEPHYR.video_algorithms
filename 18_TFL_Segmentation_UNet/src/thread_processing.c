#include "tflm_wrapper.h"

#include <string.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/cache.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/video.h>
#include <zephyr/logging/log.h>

#include <mpix/image.h>

#include "thread_processing.h"

// Логика работы с дисплеем (app_display_frame).
#include "display.h"
// Логика WebSocket-сервера (frame_is_sended, shared_frame_buf, allowed_2_send, shared_fmt ...).
#include "server.h"

// Подключим функции для обработки изображения.
#include "custom_mpix_draw.h"
#include "custom_mpix_font_12pt_rus.h"
#include "custom_mpix_font_freemonobold8pt_ru.h"

// Регистрирует отдельный модуль логирования.
LOG_MODULE_REGISTER(PROCESSING, LOG_LEVEL_INF);

/* Устройство дисплея определено в main.c. */
extern const struct device *const display_dev;
// Формат кадра (разрешение, pixelformat). Заполнится при инициализации камеры.
struct video_format shared_fmt;

/* Запускать сегментацию на каждом N-м кадре (1 = на каждом). Если инференс медленный, увеличьте значение:
 * между запусками на кадре рисуется последняя полученная маска. */
#define SEGMENT_EVERY_N_FRAMES 1

/* Отрисовка результата. */
#define SHOW_STATS       1     /* 1 = внизу кадра печатать "доля класса 0 / доля класса 1 % время" */

/* Прозрачность наложения маски 0..256 (256 = сплошной цвет класса). По умолчанию берётся из seg_config.h. */
#ifndef SEG_OVERLAY_ALPHA
#define SEG_OVERLAY_ALPHA 128
#endif

/* Подавление шума: если класс занял на маске 32x32 меньше MASK_MIN_CLASS_PX пикселей, он считается ложным
 * срабатыванием и на кадре не рисуется. 0 = отключено. Значение подбирайте по ложным срабатываниям на фоне
 * (ноутбук печатает «фон: ложных кадров» и число пикселей на такой кадр). */
#define MASK_MIN_CLASS_PX 0

/* ------------------------------------------------------------------------------------------------- */
/*                                      Общие данные потока                                           */
/* ------------------------------------------------------------------------------------------------- */

/* Буферы для работы с изображением. */
#if defined(CONFIG_ESP32_TRUE)
    // Для ESP32 буфер следует разместить во внешнюю PSRAM.
    static uint8_t buf_out[SHARED_FRAME_BUF_SIZE]
        __attribute__((aligned(4)))
        Z_GENERIC_SECTION(.ext_ram.bss);

    // Уменьшенное изображение для входа модели (RGB24, 32x32).
    static uint8_t buf_downsample[TFLM_INPUT_SIZE]
        __attribute__((aligned(4)))
        Z_GENERIC_SECTION(.ext_ram.bss);

    uint8_t buf_processing[CONFIG_VIDEO_FRAME_WIDTH * CONFIG_VIDEO_FRAME_HEIGHT * 3]
        __attribute__((aligned(4)))
        Z_GENERIC_SECTION(.ext_ram.bss);
#endif /* CONFIG_ESP32_TRUE */
#if defined(CONFIG_STM32_TRUE)
    static uint8_t buf_out[SHARED_FRAME_BUF_SIZE];

    static uint8_t buf_downsample[TFLM_INPUT_SIZE];

    uint8_t buf_processing[CONFIG_VIDEO_FRAME_WIDTH * CONFIG_VIDEO_FRAME_HEIGHT * 3];
#endif /* CONFIG_STM32_TRUE */

// Флаг дающий понять что процесс обработки кадра завершён.
atomic_t frame_processing_is_done = ATOMIC_INIT(1);

// Семафор ожидающий новый кадр из main.
K_SEM_DEFINE(allowed_2_process, 0, 1);

/* Структура libmpix, описывающая формат изображения. */
struct mpix_format my_fmt;

/* Последняя маска сегментации 32x32 (бит k = класс k) и число пикселей каждого класса. Статические, а не в стеке
 * потока: маска живёт между кадрами, когда сегментация запускается не на каждом кадре. */
static uint8_t seg_mask[TFLM_MASK_SIZE];
static int seg_class_px[TFLM_NUM_CLASSES];

/* ------------------------------------------------------------------------------------------------- */
/*                                   Поток обработки изображения                                      */
/* ------------------------------------------------------------------------------------------------- */

/* Накладывает маску 32x32 на RGB24-кадр CONFIG_VIDEO_FRAME_WIDTH x HEIGHT.
 * Маска получена из ВСЕГО кадра камеры (subsample без кропа), поэтому масштабирование линейное, без смещений:
 * каждый пиксель кадра берёт ближайший пиксель маски (nearest, для 160x120 блок 5 x 3.75 px).
 * Цвет класса - SEG_COLORS[c], смешивание с прозрачностью SEG_OVERLAY_ALPHA; перекрывающиеся классы
 * накладываются по очереди (как в ноутбуке). Пишем прямо в RGB24-буфер (байты R,G,B), без холста libmpix. */
static void draw_mask_overlay(uint8_t *rgb, const uint8_t *mask)
{
	const int W = CONFIG_VIDEO_FRAME_WIDTH;
	const int H = CONFIG_VIDEO_FRAME_HEIGHT;
	const int a = CLAMP(SEG_OVERLAY_ALPHA, 0, 256);

	/* Номер столбца маски для каждого столбца кадра: считается один раз. */
	static uint8_t col_lut[CONFIG_VIDEO_FRAME_WIDTH];
	static bool col_lut_ready;

	if (!col_lut_ready) {
		for (int x = 0; x < W; x++) {
			col_lut[x] = (uint8_t)((x * TFLM_MASK_W) / W);
		}
		col_lut_ready = true;
	}

	for (int y = 0; y < H; y++) {
		const uint8_t *mrow = &mask[((y * TFLM_MASK_H) / H) * TFLM_MASK_W];
		uint8_t *px = &rgb[y * W * 3];

		for (int x = 0; x < W; x++, px += 3) {
			const uint8_t bits = mrow[col_lut[x]];

			if (!bits) {
				continue;
			}
			for (int c = 0; c < TFLM_NUM_CLASSES; c++) {
				if (bits & (1u << c)) {
					px[0] = (uint8_t)((px[0] * (256 - a) + SEG_COLORS[c][0] * a) >> 8);
					px[1] = (uint8_t)((px[1] * (256 - a) + SEG_COLORS[c][1] * a) >> 8);
					px[2] = (uint8_t)((px[2] * (256 - a) + SEG_COLORS[c][2] * a) >> 8);
				}
			}
		}
	}
}

#if MASK_MIN_CLASS_PX > 0
/* Убирает из маски классы, занявшие меньше MASK_MIN_CLASS_PX пикселей (мелкие ложные срабатывания). */
static void suppress_small_classes(uint8_t *mask, int *class_px)
{
	uint8_t keep = 0;

	for (int c = 0; c < TFLM_NUM_CLASSES; c++) {
		if (class_px[c] >= MASK_MIN_CLASS_PX) {
			keep |= (uint8_t)(1u << c);
		} else {
			class_px[c] = 0;
		}
	}
	for (int p = 0; p < TFLM_MASK_SIZE; p++) {
		mask[p] &= keep;
	}
}
#endif /* MASK_MIN_CLASS_PX > 0 */

/* Стек: libmpix + TFLM Invoke() + логирование. 1000 байт было слишком мало.
 * Стартуем с 8 КБ, затем уточняем через thread analyzer. */
#define PROCESSING_THREAD_STACK_SIZE 8192
#define PROCESSING_THREAD_PRIORITY   7

K_THREAD_STACK_DEFINE(processing_thread_stack, PROCESSING_THREAD_STACK_SIZE);
static struct k_thread processing_thread_data;

/* Поток обработки кадра.
 * Алгоритм работы:
 *   1. Дождаться готового RGB24 кадра от main() (allowed_2_process).
 *   2. Уменьшить кадр до 32x32x3 и сегментировать его нейросетью UNet (TFLM int8): получить маску 32x32.
 *   3. Наложить полупрозрачную маску классов на кадр и вывести статистику (custom_mpix_draw_*).
 *   4. Конвертировать кадр обратно в нативный формат камеры (my_fmt.fourcc).
 *   5. Отдать буфер обратно main()/следующему кадру (frame_processing_is_done = 1).
 *   6. Скопировать готовый кадр на отправку по WebSocket.
 *   7. Вывести готовый кадр на дисплей (если он задан в Device Tree). */
static void processing_thread_start(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	int ret;
	int err;

	/* Инициализация TFLM. При ошибке кадры продолжают обрабатываться без сегментации. */
	bool tflm_ok = false;
	ret = tflm_wrapper_init();
	if (ret != 0) {
		LOG_ERR("tflm_wrapper_init failed: %d", ret);
	} else {
		tflm_ok = true;

		/* Самопроверка по контрольным кадрам ноутбука (если seg_selftest.h добавлен в проект). */
		ret = tflm_wrapper_selftest();
		if (ret >= 0) {
			LOG_INF("selftest: %d px differ from notebook (единицы - норма)", ret);
		} else if (ret != TFLM_SELFTEST_UNAVAILABLE) {
			LOG_ERR("selftest failed: %d", ret);
		}
	}

	/* Счётчики кадров и наличие готовой маски. */
	bool have_result = false;
	uint32_t frame_counter = 0;
	uint32_t infer_counter = 0;

	/* Изображение libmpix для обработки кадра. */
	struct mpix_image img_process = {};

	/* Холст для рисования текста поверх RGB24-буфера. */
	struct custom_mpix_canvas canvas;

	/* Строка для вывода результата на кадр. */
	char str[64];

	/* Описание формата данных, лежащих в buf_processing —
	 * это ВСЕГДА RGB24 (CONFIG_VIDEO_FRAME_WIDTH x CONFIG_VIDEO_FRAME_HEIGHT),
	 * т.к. main() кладёт туда кадр уже сконвертированным. */
	struct mpix_format process_fmt = {
		.fourcc = MPIX_FMT_RGB24,
		.width  = CONFIG_VIDEO_FRAME_WIDTH,
		.height = CONFIG_VIDEO_FRAME_HEIGHT,
	};

	while (1) {

		/* Ждать пока main() полностью скопирует новый RGB24-кадр
		 * в buf_processing. */
		k_sem_take(&allowed_2_process, K_FOREVER);

		/* Инвалидируем D-Cache перед чтением буфера, т.к. main()
		 * писал в него из другого потока/ядра. */
		sys_cache_data_invd_range(buf_processing, FRAME_RGB24_SIZE);

		/* Холст нужен для текста поверх реального кадра. */
		if (custom_mpix_canvas_from_buf(&canvas, buf_processing,
						CONFIG_VIDEO_FRAME_WIDTH,
						CONFIG_VIDEO_FRAME_HEIGHT) != 0) {
			LOG_ERR("canvas init failed");
			atomic_set(&frame_processing_is_done, 1);
			continue;
		}

		/* ---------------------------- Сегментация ---------------------------- */
		if (tflm_ok && (frame_counter++ % SEGMENT_EVERY_N_FRAMES) == 0) {

			/* Оборачиваем RGB24-буфер в mpix_image и уменьшаем до 32x32
			 * (тем же алгоритмом, которым получен датасет). */
			mpix_image_from_buf(&img_process, buf_processing, FRAME_RGB24_SIZE, &process_fmt);

			err = mpix_image_resize_subsample(&img_process, TFLM_INPUT_W, TFLM_INPUT_H);
			if (err) {
				LOG_ERR("Failed to resize: %d", err);
				mpix_image_free(&img_process);
			} else {
				/* Записать уменьшенное изображение в buf_downsample (R,G,B). */
				err = mpix_image_to_buf(&img_process, buf_downsample,
							sizeof(buf_downsample));
				mpix_image_free(&img_process);
				if (err) {
					LOG_ERR("Failed to write buf_downsample: %d", err);
				} else {
					/* Invoke() + пороги -> маска. n >= 0 - число пикселей переднего плана, n < 0 - ошибка. */
					int n = tflm_wrapper_segment(buf_downsample, seg_mask, seg_class_px);
					if (n < 0) {
						LOG_ERR("Segment failed: %d", n);
					} else {
#if MASK_MIN_CLASS_PX > 0
						suppress_small_classes(seg_mask, seg_class_px);
#endif
						have_result = true;

						if ((infer_counter++ % 30) == 0) {
							uint32_t inv_us, post_us;
							tflm_wrapper_get_timing(&inv_us, &post_us);
							LOG_INF("seg: fg=%d px (%s=%d, %s=%d) invoke=%u us post=%u us",
								n, SEG_LABELS[0], seg_class_px[0],
								SEG_LABELS[TFLM_NUM_CLASSES - 1],
								seg_class_px[TFLM_NUM_CLASSES - 1], inv_us, post_us);
						}
					}
				}
			}
		}

		/* Вывод результата на кадр: полупрозрачная маска классов и статистика. */
		if (have_result) {
			draw_mask_overlay(buf_processing, seg_mask);
#if SHOW_STATS
			{
				/* Доля кадра (в процентах), занятая каждым классом: "12/5% 34ms". */
				uint32_t inv_us, post_us;
				int len = 0;

				tflm_wrapper_get_timing(&inv_us, &post_us);
				for (int c = 0; c < TFLM_NUM_CLASSES && len < (int)sizeof(str); c++) {
					len += snprintf(str + len, sizeof(str) - len, c ? "/%u" : "%u",
							(unsigned int)(seg_class_px[c] * 100 / TFLM_MASK_SIZE));
				}
				if (len < (int)sizeof(str)) {
					snprintf(str + len, sizeof(str) - len, "%% %ums",
						 (unsigned int)((inv_us + post_us + 500U) / 1000U));
				}
				custom_mpix_draw_text(&canvas, 1, CONFIG_VIDEO_FRAME_HEIGHT - 3, str,
						      &custom_mpix_font_freemonobold12pt_ru,
						      CUSTOM_MPIX_COLOR_WHITE);
			}
#endif
		}

		/* Оборачиваем модифицированный RGB24-буфер в mpix_image,
		 * чтобы сконвертировать его обратно в нативный формат камеры/дисплея. */
		mpix_image_from_buf(&img_process, buf_processing, FRAME_RGB24_SIZE, &process_fmt);

		err = mpix_image_convert(&img_process, my_fmt.fourcc);
		if (err) {
			LOG_ERR("Failed to convert back to original format: %d", err);
			mpix_image_free(&img_process);
			atomic_set(&frame_processing_is_done, 1);
			continue;
		}

		/* Забираем итоговый кадр в buf_out. */
		err = mpix_image_to_buf(&img_process, buf_out, sizeof(buf_out));
		mpix_image_free(&img_process);
		if (err) {
			LOG_ERR("Failed to write to buf_out: %d", err);
			atomic_set(&frame_processing_is_done, 1);
			continue;
		}

		/* buf_processing нам больше не нужен — освобождаем
		 * его для main(), чтобы захват следующего кадра не простаивал. */
		atomic_set(&frame_processing_is_done, 1);

		/* Отправка обработанного кадра по WebSocket. */
		size_t copy_size = MIN(sizeof(buf_out), SHARED_FRAME_BUF_SIZE);

		// Код компилируется только при включённом через конфиг сервере в проект.
        #if defined(CONFIG_SERVER_TRUE)
			/* Если предыдущий кадр отправлен на вебстраничку. */
			if (atomic_cas(&frame_is_sended, 1, 0)) {
				/* Копируем кадр из буфера камеры в общий буфер для сетевого потока. */
				memcpy(shared_frame_buf, buf_out, copy_size);
				// Сбрасываем кэш в RAM, чтобы сетевой стек/DMA увидел записанные нами данные.
				sys_cache_data_flush_range(shared_frame_buf, copy_size);
				// Сообщаем отправляющему потоку реальный размер кадра.
				shared_frame_bytesused = copy_size;
				//  Будим поток WebSocket: разрешаем отпраку кадра по соединению вебсокетов.
				k_sem_give(&allowed_2_send);
			}
		#endif /* CONFIG_SERVER_TRUE */

		// Код компилируется только при включённом через конфиг дисплее в проект.
        #if defined(CONFIG_DISPLAY_TRUE)
			/* Проверка дисплея (если он задан в Device Tree). */
			if (DT_HAS_CHOSEN(zephyr_display)) {
				struct video_buffer display_vbuf = {
					.buffer    = buf_out,
					.bytesused = sizeof(buf_out),
					.size      = sizeof(buf_out),
					.type      = VIDEO_BUF_TYPE_OUTPUT,
				};
				ret = app_display_frame(display_dev, &display_vbuf, &shared_fmt);
				if (ret != 0) {
					LOG_WRN("Failed to display this frame");
				}
			}
		#endif /* CONFIG_DISPLAY_TRUE */
	}
}


void processing_thread_init(void)
{
	k_thread_create(&processing_thread_data, processing_thread_stack,
			K_THREAD_STACK_SIZEOF(processing_thread_stack),
			processing_thread_start, NULL, NULL, NULL,
			PROCESSING_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&processing_thread_data, "processing_thread");
}

