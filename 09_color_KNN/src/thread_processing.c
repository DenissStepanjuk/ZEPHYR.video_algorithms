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

#include "thread_processing.h"

// Логика работы с дисплеем (app_display_frame).
#include "display.h"
// Логика WebSocket-сервера (frame_is_sended, shared_frame_buf, allowed_2_send, shared_fmt ...).
#include "server.h"

// Подключим функции для обработки изображения.
#include "custom_mpix_draw.h"
// #include "custom_mpix_font_12pt_rus.h"
#include "custom_mpix_font_12pt_eng.h"
#include "custom_mpix_color_KNN.h"

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
#endif /* CONFIG_ESP32_TRUE */
#if defined(CONFIG_STM32_TRUE)
    static uint8_t buf_out[SHARED_FRAME_BUF_SIZE];
    
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


	// Структура описывающая область на которой идёт распознавание цвета.
	struct custom_mpix_roi color_roi = {
        .x_min = 70, .y_min = 50, .x_max = 90, .y_max = 70, /* пример: центральная область 20x20 */
    };

	// Структура для хранения результата распознавания цвета.
    struct custom_mpix_color_result color_result = {
		.color_index = -1,
		.color_name  = "UNKNOWN",
	};

	// Цвет периметра области на которой идёт распознавание цвета.
	struct custom_mpix_rgb24 roi_color = { .r = 255, .g = 255, .b = 255 };

	// Буфер для хранения строки со значениями пикселей.
	char str[32]; 

	/* Классификатор KNN: параметры стандартизации (mu/sigma/scale) и датасет в INT8.
	 * static — чтобы не занимать стек потока (он всего 1000 байт). */
	static struct custom_mpix_color_knn color_knn;

	/* Один раз при старте потока: посчитать среднее и стандартное отклонение по сырому
	 * датасету и отмасштабировать его в INT8. */
	err = custom_mpix_color_knn_init(&color_knn, custom_mpix_default_color_palette,
					 ARRAY_SIZE(custom_mpix_default_color_palette));
	if (err) {
		LOG_ERR("KNN init failed: %d", err);
	}

	while (1) {

		/* Ждать пока main() полностью скопирует новый RGB24-кадр
		 * в buf_processing. */
		k_sem_take(&allowed_2_process, K_FOREVER);

		/* Инвалидируем D-Cache перед чтением буфера, т.к. main()
		 * писал в него из другого потока/ядра. */
		sys_cache_data_invd_range(buf_processing, FRAME_RGB24_SIZE);

		/* Чёрный фон уже не нужен — рисуем поверх реального кадра. */
		if (custom_mpix_canvas_from_buf(&canvas, buf_processing, CONFIG_VIDEO_FRAME_WIDTH, CONFIG_VIDEO_FRAME_HEIGHT) != 0) {
			LOG_ERR("canvas init failed");
			atomic_set(&frame_processing_is_done, 1);
			continue;
		}

		// Произведём захват цвета и его определение на интересующей нас области.
		err = custom_mpix_color_recognize(buf_processing, CONFIG_VIDEO_FRAME_WIDTH, CONFIG_VIDEO_FRAME_HEIGHT,
                        				&color_roi, &color_knn, &color_result);
        if (err) {
            LOG_WRN("Color recognize failed: %d", err);
        }

		// Отрисуем на изображении периметр области на которой идёт распознавание цвета.
        custom_mpix_draw_hline(&canvas, color_roi.x_min, color_roi.y_min, color_roi.x_max - color_roi.x_min, 1, roi_color);
        custom_mpix_draw_hline(&canvas, color_roi.x_min, color_roi.y_max - 1, color_roi.x_max - color_roi.x_min, 1, roi_color);
        custom_mpix_draw_vline(&canvas, color_roi.x_min, color_roi.y_min, color_roi.y_max - color_roi.y_min, 1, roi_color);
        custom_mpix_draw_vline(&canvas, color_roi.x_max - 1, color_roi.y_min, color_roi.y_max - color_roi.y_min, 1, roi_color);

		// Напечатаем на изображении наименование распознаного цвета.
		custom_mpix_draw_text(&canvas, 50, 90, color_result.color_name, &custom_mpix_font_freemonobold12pt,
				    		CUSTOM_MPIX_COLOR_BLUE);

		// Собираем строку со значениями пикселей на интересующей нас области.
		snprintf(str, sizeof(str), "R:%u\nG:%u\nB:%u", 
        							color_result.r_avg, 
        							color_result.g_avg, 
        							color_result.b_avg);

		// Напечатаем на изображении значения пикселей на интересующей нас области.
		custom_mpix_draw_text(&canvas, 1, 15, str, &custom_mpix_font_freemonobold12pt,
				    		CUSTOM_MPIX_COLOR_RED);

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















// #include <string.h>

// #include <zephyr/kernel.h>
// #include <zephyr/device.h>
// #include <zephyr/cache.h>
// #include <zephyr/sys/util.h>
// #include <zephyr/drivers/display.h>
// #include <zephyr/drivers/video.h>
// #include <zephyr/logging/log.h>
// #include <zephyr/drivers/video.h>

// #include <mpix/image.h>

// #include "thread_processing.h"

// // Логика работы с дисплеем (app_display_frame).
// #include "display.h"
// // Логика WebSocket-сервера (frame_is_sended, shared_frame_buf, allowed_2_send, shared_fmt ...).
// #include "server.h"

// // Подключим функции для обработки изображения.
// #include "custom_mpix_draw.h"
// // #include "custom_mpix_font_12pt_rus.h"
// #include "custom_mpix_font_12pt_eng.h"
// #include "custom_mpix_color_recognize.h"

// // Регистрирует отдельный модуль логирования UI.
// LOG_MODULE_REGISTER(PROCESSING, LOG_LEVEL_INF);

// /* Устройство дисплея определено в main.c. */
// extern const struct device *const display_dev;
// // Формат кадра (разрешение, pixelformat). Заполнится при инициализации камеры.
// struct video_format shared_fmt;

// /* ------------------------------------------------------------------------------------------------- */
// /*                                      Общие данные потока                                           */
// /* ------------------------------------------------------------------------------------------------- */

// /* Буферы для работы с изображением. */
// #if defined(CONFIG_ESP32_TRUE)
//     // Для ESP32 буфер следует разместить во внешнюю PSRAM.
//     static uint8_t buf_out[SHARED_FRAME_BUF_SIZE]
//         __attribute__((aligned(4)))
//         Z_GENERIC_SECTION(.ext_ram.bss);

//     uint8_t buf_processing[CONFIG_VIDEO_FRAME_WIDTH * CONFIG_VIDEO_FRAME_HEIGHT * 3]
//         __attribute__((aligned(4)))
//         Z_GENERIC_SECTION(.ext_ram.bss);
// #endif /* CONFIG_ESP32_TRUE */
// #if defined(CONFIG_STM32_TRUE)
//     static uint8_t buf_out[SHARED_FRAME_BUF_SIZE];
    
//     uint8_t buf_processing[CONFIG_VIDEO_FRAME_WIDTH * CONFIG_VIDEO_FRAME_HEIGHT * 3];
// #endif /* CONFIG_STM32_TRUE */

// // Флаг дающий понять что процесс обработки кадра завершён.
// atomic_t frame_processing_is_done = ATOMIC_INIT(1);

// // Семафор ожидающий новый кадр из main.
// K_SEM_DEFINE(allowed_2_process, 0, 1);

// /* Структура libmpix, описывающая формат изображения. */
// struct mpix_format my_fmt;

// /* ------------------------------------------------------------------------------------------------- */
// /*                                   Поток обработки изображения                                      */
// /* ------------------------------------------------------------------------------------------------- */

// #define PROCESSING_THREAD_STACK_SIZE 1000
// #define PROCESSING_THREAD_PRIORITY   7

// K_THREAD_STACK_DEFINE(processing_thread_stack, PROCESSING_THREAD_STACK_SIZE);
// static struct k_thread processing_thread_data;

// /* Поток обработки кадра.
//  * Алгоритм работы:
//  *   1. Дождаться готового RGB24 кадра от main() (allowed_2_process).
//  *   2. Нанести графику и текст на кадр (custom_mpix_draw_*).
//  *   3. Конвертировать кадр обратно в нативный формат камеры (my_fmt.fourcc).
//  *   4. Отдать буфер обратно main()/следующему кадру (frame_processing_is_done = 1).
//  *   5. Скопировать готовый кадр на отправку по WebSocket.
//  *   6. Вывести готовый кадр на дисплей (если он задан в Device Tree). */
// static void processing_thread_start(void *arg1, void *arg2, void *arg3)
// {
// 	ARG_UNUSED(arg1);
// 	ARG_UNUSED(arg2);
// 	ARG_UNUSED(arg3);

// 	int ret;
// 	int err;

// 	/* Изображение libmpix для обработки кадра. */
// 	struct mpix_image img_process = {};

// 	/* Холст для рисования поверх RGB24-буфера. */
// 	struct custom_mpix_canvas canvas;

// 	/* Описание формата данных, лежащих в buf_processing —
// 	 * это ВСЕГДА RGB24 160x120, т.к. main() кладёт туда кадр уже
// 	 * сконвертированным. В отличие от my_fmt (нативный формат камеры),
// 	 * этот дескриптор нужен только внутри потока обработки. */
// 	struct mpix_format process_fmt = {
// 		.fourcc = MPIX_FMT_RGB24,
// 		.width  = CONFIG_VIDEO_FRAME_WIDTH,
// 		.height = CONFIG_VIDEO_FRAME_HEIGHT,
// 	};


// 	// Структура описывающая область на которой идёт распознавание цвета.
// 	struct custom_mpix_roi color_roi = {
//         .x_min = 70, .y_min = 50, .x_max = 90, .y_max = 70, /* пример: центральная область 20x20 */
//     };

// 	// Структура для хранения результата распознавания цвета.
//     struct custom_mpix_color_result color_result;

// 	// Цвет периметра области на которой идёт распознавание цвета.
// 	struct custom_mpix_rgb24 roi_color = { .r = 255, .g = 255, .b = 255 };

// 	// Буфер для хранения строки со значениями пикселей.
// 	char str[32]; 

// 	while (1) {

// 		/* Ждать пока main() полностью скопирует новый RGB24-кадр
// 		 * в buf_processing. */
// 		k_sem_take(&allowed_2_process, K_FOREVER);

// 		/* Инвалидируем D-Cache перед чтением буфера, т.к. main()
// 		 * писал в него из другого потока/ядра. */
// 		sys_cache_data_invd_range(buf_processing, FRAME_RGB24_SIZE);

// 		/* Чёрный фон уже не нужен — рисуем поверх реального кадра. */
// 		if (custom_mpix_canvas_from_buf(&canvas, buf_processing, CONFIG_VIDEO_FRAME_WIDTH, CONFIG_VIDEO_FRAME_HEIGHT) != 0) {
// 			LOG_ERR("canvas init failed");
// 			atomic_set(&frame_processing_is_done, 1);
// 			continue;
// 		}

// 		// Произведём захват цвета и его определение на интересующей нас области.
// 		err = custom_mpix_color_recognize(buf_processing, CONFIG_VIDEO_FRAME_WIDTH, CONFIG_VIDEO_FRAME_HEIGHT,
//                         				&color_roi, custom_mpix_default_color_palette,
//                         				ARRAY_SIZE(custom_mpix_default_color_palette), &color_result);
//         if (err) {
//             LOG_WRN("Color recognize failed: %d", err);
//         }

// 		// Отрисуем на изображении периметр области на которой идёт распознавание цвета.
//         custom_mpix_draw_hline(&canvas, color_roi.x_min, color_roi.y_min, color_roi.x_max - color_roi.x_min, 1, roi_color);
//         custom_mpix_draw_hline(&canvas, color_roi.x_min, color_roi.y_max - 1, color_roi.x_max - color_roi.x_min, 1, roi_color);
//         custom_mpix_draw_vline(&canvas, color_roi.x_min, color_roi.y_min, color_roi.y_max - color_roi.y_min, 1, roi_color);
//         custom_mpix_draw_vline(&canvas, color_roi.x_max - 1, color_roi.y_min, color_roi.y_max - color_roi.y_min, 1, roi_color);

// 		// Напечатаем на изображении наименование распознаного цвета.
// 		custom_mpix_draw_text(&canvas, 50, 90, color_result.color_name, &custom_mpix_font_freemonobold12pt,
// 				    		CUSTOM_MPIX_COLOR_BLUE);

// 		// Собираем строку со значениями пикселей на интересующей нас области.
// 		snprintf(str, sizeof(str), "R:%u\nG:%u\nB:%u", 
//         							color_result.r_avg, 
//         							color_result.g_avg, 
//         							color_result.b_avg);

// 		// Напечатаем на изображении значения пикселей на интересующей нас области.
// 		custom_mpix_draw_text(&canvas, 1, 15, str, &custom_mpix_font_freemonobold12pt,
// 				    		CUSTOM_MPIX_COLOR_RED);

// 		/* Оборачиваем модифицированный RGB24-буфер в mpix_image,
// 		 * чтобы сконвертировать его обратно в нативный формат камеры/дисплея. */
// 		mpix_image_from_buf(&img_process, buf_processing, FRAME_RGB24_SIZE, &process_fmt);

// 		err = mpix_image_convert(&img_process, my_fmt.fourcc);
// 		if (err) {
// 			LOG_ERR("Failed to convert back to original format: %d", err);
// 			mpix_image_free(&img_process);
// 			atomic_set(&frame_processing_is_done, 1);
// 			continue;
// 		}

// 		/* Забираем итоговый кадр в buf_out. */
// 		err = mpix_image_to_buf(&img_process, buf_out, sizeof(buf_out));
// 		mpix_image_free(&img_process);
// 		if (err) {
// 			LOG_ERR("Failed to write to buf_out: %d", err);
// 			atomic_set(&frame_processing_is_done, 1);
// 			continue;
// 		}

// 		/* buf_processing нам больше не нужен — освобождаем
// 		 * его для main(), чтобы захват следующего кадра не простаивал. */
// 		atomic_set(&frame_processing_is_done, 1);

// 		/* Отправка обработанного кадра по WebSocket. */
// 		size_t copy_size = MIN(sizeof(buf_out), SHARED_FRAME_BUF_SIZE);

// 		// Код компилируется только при включённом через конфиг сервере в проект.
//         #if defined(CONFIG_SERVER_TRUE)
// 			/* Если предыдущий кадр отправлен на вебстраничку. */
// 			if (atomic_cas(&frame_is_sended, 1, 0)) {
// 				/* Копируем кадр из буфера камеры в общий буфер для сетевого потока. */
// 				memcpy(shared_frame_buf, buf_out, copy_size);
// 				// Сбрасываем кэш в RAM, чтобы сетевой стек/DMA увидел записанные нами данные.
// 				sys_cache_data_flush_range(shared_frame_buf, copy_size);
// 				// Сообщаем отправляющему потоку реальный размер кадра.
// 				shared_frame_bytesused = copy_size;
// 				//  Будим поток WebSocket: разрешаем отпраку кадра по соединению вебсокетов.
// 				k_sem_give(&allowed_2_send);
// 			}
// 		#endif /* CONFIG_SERVER_TRUE */

// 		// Код компилируется только при включённом через конфиг дисплее в проект.
//         #if defined(CONFIG_DISPLAY_TRUE)
// 			/* Проверка дисплея (если он задан в Device Tree). */
// 			if (DT_HAS_CHOSEN(zephyr_display)) {
// 				struct video_buffer display_vbuf = {
// 					.buffer    = buf_out,
// 					.bytesused = sizeof(buf_out),
// 					.size      = sizeof(buf_out),
// 					.type      = VIDEO_BUF_TYPE_OUTPUT,
// 				};
// 				ret = app_display_frame(display_dev, &display_vbuf, &shared_fmt);
// 				if (ret != 0) {
// 					LOG_WRN("Failed to display this frame");
// 				}
// 			}
// 		#endif /* CONFIG_DISPLAY_TRUE */  
// 	}
// }


// void processing_thread_init(void)
// {
// 	k_thread_create(&processing_thread_data, processing_thread_stack,
// 			K_THREAD_STACK_SIZEOF(processing_thread_stack),
// 			processing_thread_start, NULL, NULL, NULL,
// 			PROCESSING_THREAD_PRIORITY, 0, K_NO_WAIT);
// 	k_thread_name_set(&processing_thread_data, "processing_thread");
// }