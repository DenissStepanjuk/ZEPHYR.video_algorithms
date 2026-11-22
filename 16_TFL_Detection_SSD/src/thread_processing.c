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

/* Запускать детектор на каждом N-м кадре (1 = на каждом). Если инференс медленный, увеличьте значение:
 * между запусками на кадре рисуются последние найденные объекты. */
#define DETECT_EVERY_N_FRAMES 1

/* Отрисовка результата. */
#define BBOX_THICKNESS   2     /* толщина рамки, px */
#define LABEL_BASELINE   15    /* отступ базовой линии шрифта 12pt от верхнего края строки, px */
#define LABEL_CHAR_W     14    /* ширина символа моноширинного шрифта, px (подстройте под ваш шрифт) */
#define SHOW_STATS       1     /* 1 = внизу кадра печатать "N obj, X ms" */

/* Цвет рамки по классу. Макрос, а не массив: тип CUSTOM_MPIX_COLOR_* определён в custom_mpix_draw.h. */
#define DET_COLOR(label) (((label) % 2) == 0 ? CUSTOM_MPIX_COLOR_YELLOW : CUSTOM_MPIX_COLOR_RED)

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

/* ------------------------------------------------------------------------------------------------- */
/*                                   Поток обработки изображения                                      */
/* ------------------------------------------------------------------------------------------------- */

/* Рисует одну детекцию: рамку (4 линии) и подпись "Класс NN%".
 * Координаты det_t нормированы на кадр 32x32, полученный из ВСЕГО кадра камеры (subsample без кропа),
 * поэтому масштабирование на CONFIG_VIDEO_FRAME_WIDTH x HEIGHT линейно, без смещений. */
static void draw_detection(struct custom_mpix_canvas *canvas, const det_t *d)
{
	const int W = CONFIG_VIDEO_FRAME_WIDTH;
	const int H = CONFIG_VIDEO_FRAME_HEIGHT;

	int x1 = (int)(d->x1 * (float)W + 0.5f);
	int y1 = (int)(d->y1 * (float)H + 0.5f);
	int x2 = (int)(d->x2 * (float)W + 0.5f);
	int y2 = (int)(d->y2 * (float)H + 0.5f);

	/* Жёсткий клип: функции рисования могут не проверять границы буфера. */
	x1 = CLAMP(x1, 0, W - 2);
	y1 = CLAMP(y1, 0, H - 2);
	x2 = CLAMP(x2, x1 + 1, W);
	y2 = CLAMP(y2, y1 + 1, H);

	const int w = x2 - x1;
	const int h = y2 - y1;
	const int t = (w >= 2 * BBOX_THICKNESS && h >= 2 * BBOX_THICKNESS) ? BBOX_THICKNESS : 1;

	custom_mpix_draw_hline(canvas, x1, y1, w, t, DET_COLOR(d->label));
	custom_mpix_draw_hline(canvas, x1, y2 - t, w, t, DET_COLOR(d->label));
	custom_mpix_draw_vline(canvas, x1, y1, h, t, DET_COLOR(d->label));
	custom_mpix_draw_vline(canvas, x2 - t, y1, h, t, DET_COLOR(d->label));

	char txt[24];
	snprintf(txt, sizeof(txt), "%s %u%%", tflm_wrapper_label(d->label),
		 (unsigned int)(d->score * 100.0f + 0.5f));

	/* Подпись над рамкой; если места сверху нет - внутри рамки у верхнего края. */
	int tx = x1 + 1;
	int ty = (y1 >= LABEL_BASELINE + 2) ? (y1 - 3) : (y1 + LABEL_BASELINE);
	const int max_tx = W - (int)strlen(txt) * LABEL_CHAR_W;

	tx = CLAMP(tx, 0, MAX(max_tx, 0));
	ty = CLAMP(ty, LABEL_BASELINE, H - 1);

	custom_mpix_draw_text(canvas, tx, ty, txt, &custom_mpix_font_freemonobold8pt_ru, DET_COLOR(d->label));
}

/* Стек: libmpix + TFLM Invoke() + логирование. 1000 байт было слишком мало.
 * Стартуем с 8 КБ, затем уточняем через thread analyzer. */
#define PROCESSING_THREAD_STACK_SIZE 8192
#define PROCESSING_THREAD_PRIORITY   7

K_THREAD_STACK_DEFINE(processing_thread_stack, PROCESSING_THREAD_STACK_SIZE);
static struct k_thread processing_thread_data;

/* Поток обработки кадра.
 * Алгоритм работы:
 *   1. Дождаться готового RGB24 кадра от main() (allowed_2_process).
 *   2. Уменьшить кадр до 32x32x3 и найти на нём объекты нейросетью-детектором (SSD, TFLM int8).
 *   3. Нарисовать рамки и подписи найденных объектов на кадре (custom_mpix_draw_*).
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

	/* Инициализация TFLM. При ошибке кадры продолжают обрабатываться без детекции. */
	bool tflm_ok = false;
	ret = tflm_wrapper_init();
	if (ret != 0) {
		LOG_ERR("tflm_wrapper_init failed: %d", ret);
	} else {
		tflm_ok = true;
	}

	/* Последние найденные объекты и счётчик кадров. */
	det_t dets[TFLM_MAX_DETECTIONS];
	int n_det = 0;
	bool have_result = false;
	uint32_t frame_counter = 0;
	uint32_t infer_counter = 0;

	/* Изображение libmpix для обработки кадра. */
	struct mpix_image img_process = {};

	/* Холст для рисования поверх RGB24-буфера. */
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

		/* Рисуем поверх реального кадра. */
		if (custom_mpix_canvas_from_buf(&canvas, buf_processing,
						CONFIG_VIDEO_FRAME_WIDTH,
						CONFIG_VIDEO_FRAME_HEIGHT) != 0) {
			LOG_ERR("canvas init failed");
			atomic_set(&frame_processing_is_done, 1);
			continue;
		}

		/* ---------------------------- Детекция объектов ---------------------------- */
		if (tflm_ok && (frame_counter++ % DETECT_EVERY_N_FRAMES) == 0) {

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
					/* Invoke() + декодирование + NMS. n >= 0 - число объектов, n < 0 - ошибка. */
					int n = tflm_wrapper_detect(buf_downsample, dets, TFLM_MAX_DETECTIONS);
					if (n < 0) {
						LOG_ERR("Detect failed: %d", n);
					} else {
						n_det = n;
						have_result = true;

						if ((infer_counter++ % 30) == 0) {
							uint32_t inv_us, post_us;
							tflm_wrapper_get_timing(&inv_us, &post_us);
							LOG_INF("det: n=%d invoke=%u us post=%u us",
								n_det, inv_us, post_us);
						}
					}
				}
			}
		}

		/* Вывод результата на кадр: рамки и подписи всех найденных объектов. */
		if (have_result) {
			for (int i = 0; i < n_det; i++) {
				draw_detection(&canvas, &dets[i]);
			}
#if SHOW_STATS
			{
				uint32_t inv_us, post_us;
				tflm_wrapper_get_timing(&inv_us, &post_us);
				snprintf(str, sizeof(str), "%d obj %u ms", n_det,
					 (unsigned int)((inv_us + post_us + 500U) / 1000U));
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









