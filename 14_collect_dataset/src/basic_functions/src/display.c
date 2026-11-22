// API работы с устройствами: struct device, DEVICE_DT_GET.
#include <zephyr/device.h>
// API дисплейного драйвера: display_write(), display_get_capabilities().
#include <zephyr/drivers/display.h>
// API видеодрайвера: захват кадров, форматы, буферы, управление камерой
#include <zephyr/drivers/video.h>
// Константы управляющих параметров камеры.
// #include <zephyr/drivers/video-controls.h>
// подсистема логирования Zephyr.
#include <zephyr/logging/log.h>
// Проверка согласованности сигнатур.
#include "display.h"
// Подключаем глобальные переменные.
#include "server.h"

// Регистрирует модуль логирования с именем "func_4_display".
LOG_MODULE_REGISTER(DISPLAY_LOG, CONFIG_LOG_DEFAULT_LEVEL);

/* ============================================================================
 * ФУНКЦИЯ: app_clear_display
 * Назначение: залить весь экран цветом чтобы убрать шумы.
 * Вызывается один раз при инициализации. Фон не перерисовывается
 * при каждом кадре — только область изображения обновляется в
 * app_display_frame(), что значительно снижает нагрузку на SPI.
 * Параметры:
 *   display_dev — указатель на структуру устройства дисплея (const, не изменяем)
 * Возвращает: 0 при успехе, отрицательный код ошибки при сбое.
 * ============================================================================ */
int app_clear_display(const struct device *const display_dev)
{
	// Ширина дисплея в байтах.
 	int DISPLAY_W = 320;
	// Высота дисплея (кол-во строк).
 	int DISPLAY_H = 480;

    /* Статический буфер одной строки дисплея: 480 пикселей × 2 байта = 640 байт.
     * static — размещается в .bss/.data, не на стеке (стек ARM обычно 4–8 КБ). */
    uint16_t line_with_color[DISPLAY_W];

    /* memset заполняет байты значением 0xFF.
     * Так как RGB565 белый = 0xFFFF, оба байта пикселя = 0xFF → всё корректно.*/
    memset(line_with_color, 0x5020, sizeof(line_with_color));

    /* Дескриптор строки: одна горизонтальная полоса на всю ширину дисплея.
     *   .width    = ширина в пикселях
     *   .pitch    = пикселей от начала одной строки до следующей (без padding = width)
     *   .height   = 1 строка за один вызов display_write
     *   .buf_size = байт в буфере */
    struct display_buffer_descriptor desc = {
        .width    = DISPLAY_W,
        .pitch    = DISPLAY_W * 2,
        .height   = 1,
        .buf_size = DISPLAY_W * sizeof(uint16_t),
    };

    /* Рисуем 480 белых строк, начиная с (0, 0) */
    for (uint16_t row = 0; row < DISPLAY_H; row++) {
        int ret = display_write(display_dev, 0, row, &desc, line_with_color);
        if (ret != 0) {
            LOG_ERR("Failed to clear display at row %u", row);
            return ret;
        }
    }

    LOG_INF("Display cleared (%ux%u)", DISPLAY_W, DISPLAY_H);
    return 0;
}

/* ============================================================================
 * ФУНКЦИЯ: app_setup_display
 * Назначение: инициализировать дисплей и согласовать его пиксельный формат
 *             с форматом, используемым камерой.
 * Параметры:
 *   display_dev — указатель на структуру устройства дисплея (const, не изменяем)
 *   pixfmt      — FourCC-код пиксельного формата, который используёт камера
 *                 (например: VIDEO_PIX_FMT_RGB565)
 * Возвращает: 0 при успехе, отрицательный код ошибки при сбое.
 * ============================================================================ */
int app_setup_display(const struct device *const display_dev, const uint32_t pixfmt)
{
	/* struct display_capabilities — структура, описывающая возможности дисплея:
     *   .x_resolution           — ширина дисплея в пикселях
     *   .y_resolution           — высота дисплея в пикселях
     *   .supported_pixel_formats — битовая маска поддерживаемых форматов пикселей
     *   .current_pixel_format    — текущий активный пиксельный формат
     *   .current_orientation     — текущая ориентация (нормальная/повёрнутая) */
	struct display_capabilities capabilities;
	// Переменная для хранения кодов возврата функций. Инициализируется 0 (успех). Если функция возвращает < 0 — ошибка.
	int ret = 0;

	LOG_INF("Display device: %s", display_dev->name);

	/* device_is_ready(display_dev) — проверяет, что драйвер устройства успешно
     * прошёл инициализацию (вызов init-функции завершился без ошибок).
     * Возвращает: true (устройство готово) или false (ошибка инициализации).*/
	if (!device_is_ready(display_dev)) {
		LOG_ERR("%s: display device not ready.", display_dev->name);
		return -ENOSYS;
	}

	/* Запрашивает у драйвера дисплея его возможности и заполняет структуру
     * capabilities. Это синхронный вызов — драйвер отвечает немедленно. */
	display_get_capabilities(display_dev, &capabilities);
	LOG_INF("- Capabilities:");
	LOG_INF("  x_resolution = %u, y_resolution = %u, supported_pixel_formats = %u"
		"  current_pixel_format = %u, current_orientation = %u",
		capabilities.x_resolution, capabilities.y_resolution,
		capabilities.supported_pixel_formats, capabilities.current_pixel_format,
		capabilities.current_orientation);

	/* Turn off blanking if driver supports it */
	/* «Blanking» (гашение) — состояние, при котором дисплей подключён к питанию
     * но не отображает никаких данных (экран чёрный). Это начальное состояние
     * большинства дисплейных контроллеров после сброса/инициализации.
     * display_blanking_off() отключает гашение → экран начинает показывать данные. */
	ret = display_blanking_off(display_dev);
	if (ret == -ENOSYS) {
		LOG_DBG("Display blanking off not available");
		ret = 0;
	}

	return ret;
}

/* ============================================================================
 * ФУНКЦИЯ: app_display_frame
 * Назначение: вывести один видеокадр из буфера камеры на дисплей.
 * Параметры:
 *   display_dev — устройство дисплея
 *   vbuf        — указатель на захваченный видеобуфер (заполнен камерой)
 *   fmt         — формат видеопотока (ширина, высота, питч)
 * Возвращает: 0 при успехе, < 0 при ошибке.
 * ============================================================================ */
int app_display_frame(const struct device *const display_dev,
			     const struct video_buffer *const vbuf,
			     const struct video_format *const fmt)
{
	/* struct display_buffer_descriptor — описание буфера пикселей для функции display_write().
     * Поля:
     *   .buf_size — полный размер данных в буфере в байтах
     *   .width    — ширина изображения в пикселях
     *   .pitch    — количество пикселей от начала одной строки до начала следующей
     *               (может быть >= width если есть padding/выравнивание строк)
     *   .height   — высота изображения в пикселях (вычисляется)
     *
     * ВНИМАНИЕ: .pitch = buf_desc.width — это самоссылка в инициализаторе!
     * В данном контексте buf_desc.width уже инициализировано (fmt->width),
     * поэтому .pitch получает то же значение = нет горизонтального padding.
     */
	// struct display_buffer_descriptor buf_desc = {
	// 	.buf_size = vbuf->bytesused,
	// 	.width = fmt->width,
	// 	.pitch = buf_desc.width,
	// 	.height = vbuf->bytesused / fmt->pitch,
	// };

     struct display_buffer_descriptor buf_desc = {
		.buf_size = DOWNSAMPLE_BUF_SIZE * 9,
		.width = DOWNSAMPLE_WIDTH * 3,
		.pitch = DOWNSAMPLE_WIDTH * 9,
		.height = DOWNSAMPLE_HEIGHT * 3,
	};

	/* Записывает пиксельные данные на дисплей в прямоугольник, начиная с координат (x, y).
     * Параметры:
     *   display_dev           — устройство дисплея
     *   80                   — x-координата левого верхнего угла (пиксели)
     *   180     			   — y-координата: нижней строки изображения.
     *   &buf_desc             — описание буфера/изображения (размеры, питч)
     *   vbuf->buffer          — указатель на начало пиксельных данных кадра
     * Возвращает: 0 при успехе, < 0 при ошибке (например, выход за пределы экрана)*/
	return display_write(display_dev, 112, 40, &buf_desc, vbuf->buffer);
}
