// API работы с устройствами: struct device, DEVICE_DT_GET.
#include <zephyr/device.h>
// API видеодрайвера: захват кадров, форматы, буферы, управление камерой
#include <zephyr/drivers/video.h>
// Константы управляющих параметров камеры.
#include <zephyr/drivers/video-controls.h>
// подсистема логирования Zephyr.
#include <zephyr/logging/log.h>
// Проверка согласованности сигнатур.
#include "video.h"

// Регистрирует модуль логирования с именем "func_4_video".
LOG_MODULE_REGISTER(VIDEO_LOG, CONFIG_LOG_DEFAULT_LEVEL);


/* ============================================================================
 * ФУНКЦИЯ: app_setup_video_selection
 *
 * Назначение: настроить области crop (обрезка) и compose (масштабирование).
 *
 * КОНЦЕПЦИЯ crop/compose в Video4Linux / Zephyr:
 *   CROP    — вырезать прямоугольник из исходного изображения сенсора.
 *             Например: из сенсора 1920x1080 взять только центральный регион 640x480.
 *   COMPOSE — разместить обработанное изображение в целевой области буфера.
 *             Например: масштабировать crop 640x480 до выходного размера 320x240.
 *
 * Параметры:
 *   video_dev — устройство камеры
 *   fmt       — целевой формат (width/height — желаемый размер выходного кадра)
 * ============================================================================ */
int app_setup_video_selection(const struct device *const video_dev,
				     const struct video_format *const fmt)
{
	/* struct video_selection — описывает прямоугольную область для crop/compose.
     * Поля:
     *   .type   — тип буфера (VIDEO_BUF_TYPE_OUTPUT для потока от камеры)
     *   .target — что именно настраиваем: VIDEO_SEL_TGT_CROP или VIDEO_SEL_TGT_COMPOSE
     *   .rect   — прямоугольник: {.left, .top, .width, .height} в пикселях */
	struct video_selection sel = {
		.type = VIDEO_BUF_TYPE_OUTPUT,
	};

	// Переменная для хранения кодов возврата функций. Инициализируется 0 (успех). Если функция возвращает < 0 — ошибка.
	int ret;

	/* Set the crop setting only if configured */
	/* CONFIG_VIDEO_SOURCE_CROP_WIDTH — значение из KConfig (menuconfig).
     * Если пользователь задал ненулевую ширину crop в конфигурации сборки,
     * то нужно применить crop-окно к сенсору.*/
	if (CONFIG_VIDEO_SOURCE_CROP_WIDTH > 0) {
		// Заполняем параметры crop-окна из KConfig:
		sel.target = VIDEO_SEL_TGT_CROP;
		sel.rect.left = CONFIG_VIDEO_SOURCE_CROP_LEFT;
		sel.rect.top = CONFIG_VIDEO_SOURCE_CROP_TOP;
		sel.rect.width = CONFIG_VIDEO_SOURCE_CROP_WIDTH;
		sel.rect.height = CONFIG_VIDEO_SOURCE_CROP_HEIGHT;

		/* video_set_selection(dev, &sel) — применить настройки crop/compose.
         * Передаём адрес структуры sel с заполненными полями.
         * Возвращает 0 при успехе или отрицательный код ошибки.*/
		ret = video_set_selection(video_dev, &sel);
		if (ret < 0) {
			LOG_ERR("Unable to set selection crop");
			return ret;
		}

		LOG_INF("Crop window set to (%u,%u)/%ux%u",
			sel.rect.left, sel.rect.top, sel.rect.width, sel.rect.height);
	}

	/* Check (if possible) if targeted size is same as crop and if compose is necessary */
	// Указывает аппаратуре камеры произвести обрезку сенсора аппаратно.
	sel.target = VIDEO_SEL_TGT_CROP;
	/* Запрашиваем текущие параметры crop чтобы проверить: нужно ли дополнительно применять compose (масштабирование).
     * video_get_selection(dev, &sel) — заполняет sel.rect текущими значениями именно crop. */
	ret = video_get_selection(video_dev, &sel);
	if (ret < 0 && ret != -ENOSYS) {
		LOG_ERR("Unable to get selection crop");
		return ret;
	}

	/* Если video_get_selection успешен (ret == 0) и размер crop-окна не совпадает
     * с желаемым выходным форматом — нужно compose (масштабирование).*/
	if (ret == 0 && (sel.rect.width != fmt->width || sel.rect.height != fmt->height)) {
		// Указывает аппаратуре камеры произвести масштабирование изображения с сенсора аппаратно.
		sel.target = VIDEO_SEL_TGT_COMPOSE;
		sel.rect.left = 0;
		sel.rect.top = 0;
		sel.rect.width = fmt->width;
		sel.rect.height = fmt->height;

		// Произвести масштабирование изображения.
		ret = video_set_selection(video_dev, &sel);
		if (ret < 0 && ret != -ENOSYS) {
			LOG_ERR("Unable to set selection compose");
			return ret;
		}

		LOG_INF("Compose window set to (%u,%u)/%ux%u",
			sel.rect.left, sel.rect.top, sel.rect.width, sel.rect.height);
	}

	return 0;
}

/* ============================================================================
 * ФУНКЦИЯ: app_query_video_info
 * Назначение: получить информацию о возможностях камеры и сформировать
 *             итоговый формат видеопотока на основе конфигурации.
 * Параметры:
 *   video_dev — устройство камеры
 *   caps      — выходной параметр: возможности устройства (заполняется функцией)
 *   fmt       — выходной параметр: итоговый формат потока (заполняется функцией)
 * ============================================================================ */
int app_query_video_info(const struct device *const video_dev,
				struct video_caps *const caps,
				struct video_format *const fmt)
{
	// Переменная для хранения кодов возврата функций. Инициализируется 0 (успех). Если функция возвращает < 0 — ошибка.
	int ret;

	LOG_INF("Video device: %s", video_dev->name);

	/* Проверяем готовность видеоустройства. Если камера не инициализирована — прекращаем работу. */
	if (!device_is_ready(video_dev)) {
		LOG_ERR("%s: video device is not ready", video_dev->name);
		return -ENOSYS;
	}

	/* Get capabilities (Запрашивает возможности видеоустройства (массив поддерживаемых форматов)). */
	ret = video_get_caps(video_dev, caps);
	if (ret < 0) {
		LOG_ERR("Unable to retrieve video capabilities");
		return ret;
	}

	// Выводит возможности видеоустройства (массив поддерживаемых форматов)).
	LOG_INF("- Capabilities:");
	for (int i = 0; caps->format_caps[i].pixelformat; i++) {
		const struct video_format_cap *fcap = &caps->format_caps[i];

		LOG_INF("  %s width [%u; %u; %u] height [%u; %u; %u]",
			VIDEO_FOURCC_TO_STR(fcap->pixelformat),
			fcap->width_min, fcap->width_max, fcap->width_step,
			fcap->height_min, fcap->height_max, fcap->height_step);
			k_msleep(50);
	}

	/* Get default/native format (Получить текущий (нативный/по умолчанию) формат видеопотока.) */
	ret = video_get_format(video_dev, fmt);
	if (ret < 0) {
		LOG_ERR("Unable to retrieve video format");
	}

	/* Adjust video format according to the configuration */
	/* Переопределяем параметры формата из KConfig (если заданы пользователем). */
	if (CONFIG_VIDEO_FRAME_HEIGHT > 0) {
		fmt->height = CONFIG_VIDEO_FRAME_HEIGHT;
	}
	if (CONFIG_VIDEO_FRAME_WIDTH > 0) {
		fmt->width = CONFIG_VIDEO_FRAME_WIDTH;
	}
	if (strcmp(CONFIG_VIDEO_PIXEL_FORMAT, "") != 0) {
		fmt->pixelformat = VIDEO_FOURCC_FROM_STR(CONFIG_VIDEO_PIXEL_FORMAT);
	}

	return 0;
}

/* ============================================================================
 * ФУНКЦИЯ: app_setup_video_format
 * Назначение: применить выбранный формат видеопотока к устройству камеры.
 * Параметры:
 *   video_dev — устройство камеры
 *   fmt       — выходной параметр: итоговый формат потока
 * ============================================================================ */
int app_setup_video_format(const struct device *const video_dev,
				  struct video_format *const fmt)
{
	// Переменная для хранения кодов возврата функций. Инициализируется 0 (успех). Если функция возвращает < 0 — ошибка.
	int ret;

	LOG_INF("- Video format: %s %ux%u",
		VIDEO_FOURCC_TO_STR(fmt->pixelformat), fmt->width, fmt->height);

	/* video_set_compose_format(dev, fmt)
     * Устанавливает формат выходного (compose) видеопотока. Драйвер камеры может скорректировать fmt 
	 * (например, округлить разрешение до кратного значения) — поэтому fmt передаётся НЕ как const.
     * После успешного вызова fmt->pitch и fmt->size обновляются драйвером:
     *   fmt->pitch = width * bytes_per_pixel (с учётом возможного alignment)
     *   fmt->size  = pitch * height */
	ret = video_set_compose_format(video_dev, fmt);
	if (ret < 0) {
		LOG_ERR("Unable to set format");
		return ret;
	}

	return 0;
}

/* ============================================================================
 * ФУНКЦИЯ: app_setup_video_frmival
 * Назначение: запросить поддерживаемые интервалы кадров (frame intervals)
 *             и вывести текущий FPS в лог (30 fps → interval = 1/30 ≈ 33.3 мс).
 * Параметры:
 *   video_dev — устройство камеры
 *   fmt       — выходной параметр: итоговый формат потока
 * ============================================================================ */
int app_setup_video_frmival(const struct device *const video_dev,
				   struct video_format *const fmt)
{
	/* struct video_frmival — один интервал кадров (Интервал в секундах = numerator / denominator):
     *   .numerator   — числитель дроби (обычно = 1)
     *   .denominator — знаменатель дроби (обычно = FPS) */
	struct video_frmival frmival = {};
	/* Структура для перечисления интервалов. Используется как итератор: при каждом вызове video_enum_frmival()
     * возвращает следующий поддерживаемый интервал.
     * Поля:
     *   .format  — формат, для которого перечисляем интервалы
     *   .index   — текущий индекс (инкрементируем вручную)
     *   .type    — тип результата: VIDEO_FRMIVAL_TYPE_DISCRETE или STEPWISE
     *   .discrete    — конкретное значение (если type = DISCRETE)
     *   .stepwise    — диапазон min/max/step (если type = STEPWISE) */
	struct video_frmival_enum fie = {
		.format = fmt, /* Запрашиваем интервалы для каждого формата */
	};
	// Переменная для хранения кодов возврата функций. Инициализируется 0 (успех). Если функция возвращает < 0 — ошибка.
	int ret;

	LOG_INF("- Supported frame intervals for the default format:");
    /* Цикл перечисления поддерживаемых интервалов кадров.
     * video_enum_frmival(dev, &fie) — при каждом вызове:
     *   - заполняет fie.type, fie.discrete / fie.stepwise
     *   - возвращает 0 пока есть элементы, иначе -EINVAL или -ENOTSUP
     * VIDEO_FRMIVAL_TYPE_DISCRETE — Если камера поддерживает только фиксированные FPS, например, только 15 и 30 fps.
     * Иначе (STEPWISE) — диапазон с шагом:
     *   min = минимальный интервал (максимальный FPS)
     *   max = максимальный интервал (минимальный FPS)
     *   step = шаг изменения интервала */
	while (video_enum_frmival(video_dev, &fie) == 0) {
		if (fie.type == VIDEO_FRMIVAL_TYPE_DISCRETE) {
			LOG_INF("   %u/%u", fie.discrete.numerator, fie.discrete.denominator);
		} else {
			LOG_INF("   [min = %u/%u; max = %u/%u; step = %u/%u]",
				fie.stepwise.min.numerator, fie.stepwise.min.denominator,
				fie.stepwise.max.numerator, fie.stepwise.max.denominator,
				fie.stepwise.step.numerator, fie.stepwise.step.denominator);
		}
		// Переходим к следующему элементу в конце итерации.
		fie.index++;
	}

	// Получить текущий (активный) интервал.
	ret = video_get_frmival(video_dev, &frmival);
	if (ret == -ENOTSUP || ret == -ENOSYS) {
		LOG_WRN("The video source does not support frame rate control");
	} else if (ret < 0) {
		LOG_ERR("Error while getting the frame interval");
		return ret;
	} else if (ret == 0) {
		LOG_INF("- Default frame rate : %f fps",
			1.0 * frmival.denominator / frmival.numerator);
	}

	return 0;
}

/* ============================================================================
 * ФУНКЦИЯ: app_setup_video_controls
 * Назначение: вывести список поддерживаемых параметров управления камерой
 *             и применить настройки (flip, test pattern) из конфигурации.
 * Video controls — аналог V4L2 controls: яркость, контраст, насыщенность,
 * ориентация изображения и т.д. Каждый параметр имеет числовой ID (VIDEO_CID_*).
 * ============================================================================ */
int app_setup_video_controls(const struct device *const video_dev)
{
	// Переменная для хранения кодов возврата функций. Инициализируется 0 (успех). Если функция возвращает < 0 — ошибка.
	int ret;

	/* Get supported controls */
	LOG_INF("- Supported controls:");
	// Указатель для отслеживания смены устройства при итерации (некоторые параметры могут быть у подустройств).
	const struct device *last_dev = NULL;
	// Структура запроса информации об одном параметре управления камерой.
	struct video_ctrl_query cq = {.dev = video_dev, .id = VIDEO_CTRL_FLAG_NEXT_CTRL};

	/* Цикл перечисления поддерживаемых параметров управления камерой.
     * video_query_ctrl(&cq) — заполняет cq информацией о параметре с ID = cq.id:
     *   - cq.dev может меняться (указывает на подустройство-владельца)
     *   - cq.id обновляется на найденный control
     *   Возвращает 0 пока есть параметр, иначе -EINVAL.
     *

     * cq.id |= VIDEO_CTRL_FLAG_NEXT_CTRL — добавляем флаг к ТЕКУЩЕМУ id,
     * чтобы следующий вызов вернул control ПОСЛЕ текущего (итерация).
     */
	while (video_query_ctrl(&cq) == 0) {
		// При смене устройства (cq.dev != last_dev) — печатаем имя нового устройства.
		if (cq.dev != last_dev) {
			last_dev = cq.dev;
			LOG_INF("\t\tdevice: %s", cq.dev->name);
		}
		// Форматирует и выводит в лог информацию о текущем параметре управления камерой:
     	// ID, название, тип (int/bool/menu), мин, макс, шаг, значение по умолчанию.
		video_print_ctrl(&cq);
		// Добавляем флаг к текущему id, чтобы следующий вызов вернул следующий параметр (итерация).
		cq.id |= VIDEO_CTRL_FLAG_NEXT_CTRL;
	}

	/* Set controls */
	/* Структура для установки значения параметра управления камерой.
     * Поля:
     *   .id  — ID управляющего параметра (VIDEO_CID_*)
	 * 		VIDEO_CID_HFLIP — горизонтальное зеркальное отражение (mirror)
     *   .val — новое значение параметра (int32_t)
	 * 		val = 1 — включить*/
	struct video_control ctrl = {.id = VIDEO_CID_HFLIP, .val = 1};

	/* KConfig-макрос возвращает 1 если CONFIG_VIDEO_CTRL_HFLIP=y в конфигурации, иначе 0.
     * Позволяет включать/выключать функциональность на этапе компиляции.*/
	if (IS_ENABLED(CONFIG_VIDEO_CTRL_HFLIP)) {
		// Применить параметра управления к устройству.
		ret = video_set_ctrl(video_dev, &ctrl);
		if (ret < 0) {
			LOG_ERR("Failed to set horizontal flip");
			return ret;
		}
	}

	if (IS_ENABLED(CONFIG_VIDEO_CTRL_VFLIP)) {
		ctrl.id = VIDEO_CID_VFLIP;
		ret = video_set_ctrl(video_dev, &ctrl);
		if (ret < 0) {
			LOG_ERR("Failed to set vertical flip");
			return ret;
		}
	}

	if (IS_ENABLED(CONFIG_TEST)) {
		ctrl.id = VIDEO_CID_TEST_PATTERN;
		ret = video_set_ctrl(video_dev, &ctrl);
		if (ret < 0 && ret != -ENOTSUP) {
			LOG_WRN("Failed to set the test pattern");
		}
	}

	return 0;
}

/* ============================================================================
 * ФУНКЦИЯ: app_setup_video_buffers
 * Назначение: выделить пул видеобуферов и поставить их в очередь.
 * Параметры:
 *   video_dev — устройство камеры
 * 	 caps      — возможности/параметры устройства (capabilities)
 *   fmt       — итоговый формат потока
 * ПРИНЦИП РАБОТЫ БУФЕРНОГО ПУЛА:
 *   - Выделяем N буферов заранее (CONFIG_VIDEO_BUFFER_POOL_NUM_MAX).
 *   - Каждый буфер ставим в очередь устройству (video_enqueue).
 *   - Когда камера заполнит буфер данными кадра, он переходит в
 *     очередь "готовых" буферов (dequeue-очередь).
 *   - Приложение забирает буфер (video_dequeue), обрабатывает кадр,
 *     затем снова ставит буфер в очередь (video_enqueue).
 *   - Пока буфер у приложения — камера использует другие буферы пула.
 *   Это стандартная схема zero-copy видеозахвата (V4L2 MMAP/USERPTR).
 * ============================================================================ */
int app_setup_video_buffers(const struct device *const video_dev,
				   struct video_caps *const caps,
				   struct video_format *const fmt)
{
	// Переменная для хранения кодов возврата функций. Инициализируется 0 (успех). Если функция возвращает < 0 — ошибка.
	int ret;

	/* Проверка, кол-во буферов необходимое для корректной работы аппаратного DMA видеозахвата
	 * должно быть не больше, чем максимальное число буферов в пуле заданное через KConfig*/
	if (caps->min_vbuf_count > CONFIG_VIDEO_BUFFER_POOL_NUM_MAX) {
		LOG_ERR("Not enough buffers to start streaming");
		return -EINVAL;
	}

	/* Цикл выделения и постановки в очередь CONFIG_VIDEO_BUFFER_POOL_NUM_MAX буферов. */
	for (int i = 0; i < CONFIG_VIDEO_BUFFER_POOL_NUM_MAX; i++) {
		/* struct video_buffer — структура одного видеобуфера:
         *   .buffer     — указатель на память с пиксельными данными
         *   .size       — полный выделенный размер буфера (байт)
         *   .bytesused  — реально использованных байт в последнем захваченном кадре
         *   .timestamp  — метка времени захвата кадра (мс от старта)
         *   .line_offset — смещение первой строки данных в буфере
         *   .type       — тип буфера (OUTPUT/CAPTURE и т.д.) */
		struct video_buffer *vbuf;

		/* For some hardwares, such as the PxP used on i.MX RT1170 to do image rotation,
		 * buffer alignment is needed in order to achieve the best performance */
		/* Выделяет видеобуфер из специального пула памяти Zephyr Video subsystem.
         * Параметры:
         *   fmt->size  — размер буфера в байтах (= fmt->pitch * fmt->height).
         *                Обновлён драйвером после video_set_compose_format().
         *   CONFIG_VIDEO_BUFFER_POOL_ALIGN — выравнивание адреса буфера (байт).
         *                Некоторое железо (например, PxP на i.MX RT1170) требует
         *                DMA-буферы по адресу кратному 64 или 128 байт.
         *   K_NO_WAIT  — не ждать освобождения памяти; если нет свободных буферов
         *                в пуле — сразу вернуть NULL.
         * Возвращает: указатель на struct video_buffer или NULL при ошибке.
         * ВАЖНО: память для данных кадра (vbuf->buffer) уже включена в эту структуру
         * и выровнена по заданному адресу. Пул буферов — это статически выделенный
         * регион в SRAM, размер определяется KConfig.*/
		vbuf = video_buffer_aligned_alloc(fmt->size, CONFIG_VIDEO_BUFFER_POOL_ALIGN, K_NO_WAIT);
		if (vbuf == NULL) {
			LOG_ERR("Unable to alloc video buffer");
			return -ENOMEM;
		}

		/* Помечаем буфер как выходной (от устройства захвата к приложению).
         * VIDEO_BUF_TYPE_OUTPUT — данные идут ОТ устройства (камеры) к приложению. */
		vbuf->type = VIDEO_BUF_TYPE_OUTPUT;

		/* Передаёт буфер видеодрайверу для заполнения.
         * После этого вызова:
         *   - Буфер принадлежит драйверу/DMA
         *   - Приложение НЕ ДОЛЖНО обращаться к vbuf->buffer
         *   - Когда DMA заполнит буфер кадром, он станет доступен через dequeue()
         * Возвращает: 0 при успехе, < 0 при ошибке. */
		ret = video_enqueue(video_dev, vbuf);
		if (ret < 0) {
			LOG_ERR("Failed to enqueue video buffer");
			return ret;
		}
	}

	return 0;
}

/* ============================================================================
 * ФУНКЦИЯ: app_init_video
 * Назначение: Выполнить полный цикл инициализации видеоустройства (камеры) перед запуском видеопотока.
 * ============================================================================ */
int app_init_video(const struct device *const video_dev,
                   struct video_caps *const caps,
                   struct video_format *const fmt)
{
	/* Переменная для хранения кодов возврата. */
    int ret;

	/* ------------------------------------------------------------------------
     * Получение информации о видеоустройстве.
     * Функция:
     *   - проверяет готовность устройства;
     *   - получает capabilities камеры;
     *   - получает текущий формат;
     *   - применяет настройки формата из Kconfig.
     *
     * После успешного завершения будут заполнены:
     *   caps->...
     *   fmt->width
     *   fmt->height
     *   fmt->pixelformat
     * ------------------------------------------------------------------------ */
    ret = app_query_video_info(video_dev, caps, fmt);
    if (ret < 0) {
        return ret;
    }

	/* ------------------------------------------------------------------------
     * Настройка crop/compose.
     * Crop: вырезает часть изображения сенсора.
     * Compose: масштабирует изображение до нужного размера.
     * ------------------------------------------------------------------------ */
    ret = app_setup_video_selection(video_dev, fmt);
    if (ret < 0) {
        return ret;
    }

	/* ------------------------------------------------------------------------
     * Применение формата видеопотока.
     * Драйвер получает:
     *   - разрешение
     *   - формат пикселей
     * После успешного вызова драйвер может дополнительно заполнить:
     *   fmt->pitch - количество байт в одной строке изображения.
     *   fmt->size - полный размер одного кадра в байтах.
     * ------------------------------------------------------------------------ */
    ret = app_setup_video_format(video_dev, fmt);
    if (ret < 0) {
        return ret;
    }

	/* ------------------------------------------------------------------------
     * Получение информации о поддерживаемых FPS.
     * Функция:
     *   - перечисляет доступные интервалы кадров;
     *   - выводит их в лог;
     *   - получает текущий активный FPS.
     * На конфигурацию буферов этот этап напрямую не влияет, но полезен для диагностики камеры.
     * ------------------------------------------------------------------------ */
    ret = app_setup_video_frmival(video_dev, fmt);
    if (ret < 0) {
        return ret;
    }

	/* ------------------------------------------------------------------------
     * Настройка управляющих параметров камеры.
     * Например:
     *   VIDEO_CID_HFLIP
     *   VIDEO_CID_VFLIP
     *   VIDEO_CID_TEST_PATTERN
     * Конкретный набор параметров зависит от драйвера камеры.
     * ------------------------------------------------------------------------ */
    ret = app_setup_video_controls(video_dev);
    if (ret < 0) {
        return ret;
    }

    /* ------------------------------------------------------------------------
     * Создание видеобуферов. Используются данные из:
     *   caps - содержит требования драйвера к минимальному количеству видеобуферов.
     *   fmt - содержит размер одного кадра (fmt->size).
     * Функция:
     *   - выделяет память под кадры;
     *   - создаёт пул видеобуферов;
     *   - ставит каждый буфер в очередь драйверу.
     * После завершения этого этапа камера полностью готова к запуску видеопотока через video_stream_start().
     * ------------------------------------------------------------------------ */
    ret = app_setup_video_buffers(video_dev, caps, fmt);
    if (ret < 0) {
        return ret;
    }

	/* Все этапы успешно завершены. Видеоустройство полностью подготовлено к работе. */
    return 0;
}