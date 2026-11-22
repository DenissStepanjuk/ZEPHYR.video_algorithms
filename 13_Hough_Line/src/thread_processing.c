#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/cache.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/video.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/video.h>

#include <mpix/image.h>
#include <mpix/low_level.h>

#include "thread_processing.h"

// Логика работы с дисплеем (app_display_frame).
#include "display.h"
// Логика WebSocket-сервера (frame_is_sended, shared_frame_buf, allowed_2_send, shared_fmt ...).
#include "server.h"

// Подключим функции для обработки изображения.
#include "custom_mpix_draw.h"
// #include "custom_mpix_font_12pt_rus.h"
#include "custom_mpix_Hough_transform_line.h"

// Регистрирует отдельный модуль логирования UI.
LOG_MODULE_REGISTER(PROCESSING, LOG_LEVEL_INF);

/* Устройство дисплея определено в main.c. */
extern const struct device *const display_dev;
// Формат кадра (разрешение, pixelformat). Заполнится при инициализации камеры.
struct video_format shared_fmt;

/* ------------------------------------------------------------------------------------------------- */
/*                                      Общие данные потока                                           */
/* ------------------------------------------------------------------------------------------------- */

/* Буферы для работы с изображением. */
#if defined(CONFIG_ESP32_TRUE)
    // Для ESP32 буфер следует разместить во внешнюю PSRAM.
    static uint8_t buf_out[SHARED_FRAME_BUF_SIZE]
        __attribute__((aligned(4)))
        Z_GENERIC_SECTION(.ext_ram.bss);

    uint8_t buf_processing[CONFIG_VIDEO_FRAME_WIDTH * CONFIG_VIDEO_FRAME_HEIGHT * 3]
        __attribute__((aligned(4)))
        Z_GENERIC_SECTION(.ext_ram.bss);

	
	static uint8_t buf_RGB[CONFIG_VIDEO_FRAME_WIDTH * CONFIG_VIDEO_FRAME_HEIGHT * 3]
		__attribute__((aligned(4)))
		Z_GENERIC_SECTION(.ext_ram.bss);

	
	/* Временный ч/б-буфер для ручной grayscale-конверсии  */
	static uint8_t buf_grey[CONFIG_VIDEO_FRAME_WIDTH * CONFIG_VIDEO_FRAME_HEIGHT]
		__attribute__((aligned(4)))
		Z_GENERIC_SECTION(.ext_ram.bss);
#endif /* CONFIG_ESP32_TRUE */
#if defined(CONFIG_STM32_TRUE)
	// Определяем регион памяти "SRAM1".
	#define __sram1 Z_GENERIC_SECTION(SRAM1)
	// Определяем регион памяти "SRAM2".
	#define __sram2 Z_GENERIC_SECTION(SRAM2)

    static uint8_t buf_out[SHARED_FRAME_BUF_SIZE] __sram1;
    
    uint8_t buf_processing[CONFIG_VIDEO_FRAME_WIDTH * CONFIG_VIDEO_FRAME_HEIGHT * 3] __sram1;

	static uint8_t buf_RGB[CONFIG_VIDEO_FRAME_WIDTH * CONFIG_VIDEO_FRAME_HEIGHT * 3] __sram2;

	static uint8_t buf_grey[CONFIG_VIDEO_FRAME_WIDTH * CONFIG_VIDEO_FRAME_HEIGHT] __sram1;
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

#define PROCESSING_THREAD_STACK_SIZE 1000
#define PROCESSING_THREAD_PRIORITY   7

K_THREAD_STACK_DEFINE(processing_thread_stack, PROCESSING_THREAD_STACK_SIZE);
static struct k_thread processing_thread_data;

/* Поток обработки кадра.
 * Алгоритм работы:
 *   1. Дождаться готового RGB24 кадра от main() (allowed_2_process).
 *   2. Нанести графику и текст на кадр (custom_mpix_draw_*).
 *   3. Конвертировать кадр обратно в нативный формат камеры (my_fmt.fourcc).
 *   4. Отдать буфер обратно main()/следующему кадру (frame_processing_is_done = 1).
 *   5. Скопировать готовый кадр на отправку по WebSocket.
 *   6. Вывести готовый кадр на дисплей (если он задан в Device Tree). */
static void processing_thread_start(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	int ret;
	int err;

	/* Изображение libmpix для обработки кадра. */
	struct mpix_image img_process = {};

	/* Холст для рисования поверх RGB24-буфера. */
	struct custom_mpix_canvas canvas;

	/* Описание формата данных, лежащих в buf_processing —
	 * это ВСЕГДА RGB24 160x120, т.к. main() кладёт туда кадр уже
	 * сконвертированным. В отличие от my_fmt (нативный формат камеры),
	 * этот дескриптор нужен только внутри потока обработки. */
	struct mpix_format process_fmt = {
		.fourcc = MPIX_FMT_RGB24,
		.width  = CONFIG_VIDEO_FRAME_WIDTH,
		.height = CONFIG_VIDEO_FRAME_HEIGHT,
	};

	// Флаг определяющий какое изображение будет показано пользователю: цветное или чёрно-белое.
	bool rgb = true;
	// bool rgb = false;

	while (1) {
		/* Ждать пока main() полностью скопирует новый RGB24-кадр в buf_processing. */
		k_sem_take(&allowed_2_process, K_FOREVER);

		/* Инвалидируем D-Cache перед чтением буфера, т.к. main()
		 * писал в него из другого потока/ядра. */
		sys_cache_data_invd_range(buf_processing, FRAME_RGB24_SIZE);

		// Копируем оригинальный (ещё не обработанный) RGB24-кадр в buf_RGB,
        // на котором в дальнейшем будут отрисованы найденные окружности.
		if(rgb){
        	memcpy(buf_RGB, buf_processing, FRAME_RGB24_SIZE);
		}

		/* Схлопываем в яркость и сразу разворачиваем обратно в RGB24. */
		mpix_convert_rgb24_to_y8_bt709(buf_processing, buf_grey,
                                        CONFIG_VIDEO_FRAME_WIDTH * CONFIG_VIDEO_FRAME_HEIGHT);
        mpix_convert_y8_to_rgb24_bt709(buf_grey, buf_processing,
                                        CONFIG_VIDEO_FRAME_WIDTH * CONFIG_VIDEO_FRAME_HEIGHT);

		/* Оборачиваем модифицированный RGB24-буфер в mpix_image,
		 * чтобы применить ряд операций по обработке изображения. */
		mpix_image_from_buf(&img_process, buf_processing, FRAME_RGB24_SIZE, &process_fmt);

		// Подавить шум на изображении.
		err = mpix_image_denoise(&img_process, 3);
		if (err) {
			LOG_WRN("Image denoise failed: %d", err);
		}

		// Выделить грани объектов на изображении.
		err = mpix_image_edge_detect(&img_process, 3);
		if (err) {
			LOG_WRN("Edge detect failed: %d", err);
		}

		/* Применить ряд операций по обработке к изображению. */
		err = mpix_image_to_buf(&img_process, buf_processing, sizeof(buf_processing));
		mpix_image_free(&img_process);
		if (err) {
			LOG_ERR("Failed to write to buf_out: %d", err);
		}
		/* Оборачиваем обработанный RGB24-буфер обратно в mpix_image, */
		// mpix_image_from_buf(&img_process, buf_processing, FRAME_RGB24_SIZE, &process_fmt);

		if(rgb){
			// Инициализируем холст на цветном неочищенном буфере buf_RGB.
			if (custom_mpix_canvas_from_buf(&canvas, buf_RGB, CONFIG_VIDEO_FRAME_WIDTH, CONFIG_VIDEO_FRAME_HEIGHT) != 0) {
				LOG_ERR("canvas init failed");
				atomic_set(&frame_processing_is_done, 1);
				continue;
			}
		} else {
			// Инициализируем холст на чёрно-белом обработаном буфере buf_processing.
			if (custom_mpix_canvas_from_buf(&canvas, buf_processing, CONFIG_VIDEO_FRAME_WIDTH, CONFIG_VIDEO_FRAME_HEIGHT) != 0) {
				LOG_ERR("canvas init failed");
				atomic_set(&frame_processing_is_done, 1);
				continue;
			}
		}

		// Конфигурация для поиска линий.
		struct custom_mpix_hough_line_config line_cfg = {
				.roi = { .x_min = 20, .y_min = 15, .x_max = 140, .y_max = 105 },
				.edge_threshold = 20,     /* подберите под ваш edge_detect+denoise, как и с окружностями */
				.vote_threshold = 30,     /* стартовая точка — оригинал использовал 60 на другом детекторе границ */
				.theta_step_deg = 1,
				.merge_theta_deg = 10,
				.merge_r = 15,
				.max_lines = 6,           /* как в оригинале (6 отфильтрованных линий) */
			};
		// Массив для найденных линий.
		struct custom_mpix_line found_lines[6];
		// Переменная для хранения кол-ва найденых линий.
		uint32_t found_lines_count = 0;

		// Произвести поиск окружностей.
		err = custom_mpix_hough_line_detect(buf_processing, CONFIG_VIDEO_FRAME_WIDTH, CONFIG_VIDEO_FRAME_HEIGHT,
                                        	&line_cfg, found_lines, &found_lines_count);
		
		// Отрисовать найденные линии:
		if (err) {
			LOG_WRN("Hough line detect failed: %d", err);
		} else if (found_lines_count > 0) {
			struct custom_mpix_rgb24 color_yellow = { .r = 255, .g = 255, .b = 0 };

			for (uint32_t i = 0; i < found_lines_count; i++) {
				int32_t x0, y0, x1, y1;

				err = custom_mpix_hough_line_to_segment(&found_lines[i], &line_cfg.roi, &x0, &y0, &x1, &y1);
				if (err) {
					LOG_WRN("hough_line_to_segment failed for line %u: %d", i, err);
					continue;
				}

				custom_mpix_draw_line(&canvas, x0, y0, x1, y1, 2, color_yellow);

				LOG_INF("i:%u ) Found line theta:%d r:%d", i, found_lines[i].theta_deg, found_lines[i].r);
			}
		}



		/* Оборачиваем модифицированный RGB24-буфер в mpix_image,
		* чтобы сконвертировать его обратно в нативный формат камеры/дисплея. */
		if(rgb){
        	mpix_image_from_buf(&img_process, buf_RGB, FRAME_RGB24_SIZE, &process_fmt);
		} else {
			mpix_image_from_buf(&img_process, buf_processing, FRAME_RGB24_SIZE, &process_fmt);
		}
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