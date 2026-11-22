/********************************************
    Подготовил: Степанюк Денис Борисович
********************************************/

// Стандартный ввод-вывод для логирования
#include <stdio.h>
// Макросы для форматирования целых чисел фиксированного размера.
#include <inttypes.h>
// Основные функции RTOS: потоки, таймеры, семафоры
#include <zephyr/kernel.h>
// Системные утилиты: BIT(), ARRAY_SIZE(), STRINGIFY(), и т.д.
#include "zephyr/sys/util.h"
// Парсинг и генерация JSON.
#include <zephyr/data/json.h>
// Дополнительные макросы (BIT_MASK(n) — маска из n единичных битов. BIT_MASK(2) = 0b11 = 3 (используется для проверки результата json_obj_parse)).
#include <zephyr/sys/util_macro.h>
// Работа с кэшем процессора.
#include <zephyr/cache.h>
// Используется для DEVICE_DT_GET_ANY() — получить указатель на устройство из Device Tree.
#include <zephyr/device.h>
// API видеоподсистемы: video_dequeue, video_enqueue, video_stream_start
#include <zephyr/drivers/video.h>
// Элементы управления камеры (яркость, экспозиция и т.д.)
#include <zephyr/drivers/video-controls.h>
// Логика видеоподсистемы.
#include "video.h"
// Библиотека для обработки пиксельных данных.
#include <mpix/image.h>
// Поток выделенный под пользовательский интерфейс на базе LVGL.
#include "thread_ui.h"

// Регистрируем модуль логирования.
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(MAIN_LOG, LOG_LEVEL_DBG);

// Код компилируется только при включённом через конфиг дисплее в проект.
#if defined(CONFIG_DISPLAY_TRUE)
    // Драйвер дисплея.
    #include <zephyr/drivers/display.h>
    // Логика работы с дисплеем.
    #include "display.h"
#endif /* CONFIG_DISPLAY_TRUE */  

// Код компилируется только при включённом через конфиг сервере в проект.
#if defined(CONFIG_SERVER_TRUE)
    // Логика WebSocket-сервера.
    #include "server.h"
    /* Высокоуровневая подсистема HTTP-сервера для Zephyr
    - Принимает TCP соединения
    - Парсит HTTP запросы
    - Маршрутизирует по URL → вызывает нужный handler
    - Выполняет WebSocket Upgrade (SHA1, Base64 — скрыто внутри)
    - Управляет потоками клиентов*/
    #include <zephyr/net/http/server.h>
    // Макросы для объявления HTTP/HTTPS сервисов и ресурсов:
    #include <zephyr/net/http/service.h>
    // Работа с IP-адресами и вспомогательными функциями сетевого стека. Нужен для типов адресов (sockaddr_in и т.д.) которые использует сервис.
    #include <zephyr/net/net_ip.h>
    // BSD Sockets API (zsock_*). Используется внутри WebSocket-колбэков для низкоуровневой работы с сокетами (websocket_send_msg и др.).
    #include <zephyr/net/socket.h>
    // Инициализация сетевого стека через конфиг. Используется только при CONFIG_USB_DEVICE_STACK_NEXT.
    #include <zephyr/net/net_config.h>

    // Код компилируется при условии что платформой для проекта выбрана плата STM32.
    #if defined(CONFIG_STM32_TRUE)
        // Логика подключения к интернету по dhcp.
        #include "dhcp.h"
    #endif /* CONFIG_STM32_TRUE */

    // Код компилируется при условии что платформой для проекта выбрана плата ESP32.
    #if defined(CONFIG_ESP32_TRUE)
        // Логика подключения к интернету по wifi.
        #include "wifi.h"

        /* Параметры WiFi */
        #define WIFI_SSID "NAME"
        #define WIFI_PSK "PASSWORD"

    #endif /* CONFIG_ESP32_TRUE */

    /* Сжатая gzip-версия главной HTML страницы. Файл index.html во время сборки преобразуется в массив байтов и вшивается непосредственно в прошивку.*/
    static const uint8_t index_html_gz[] = {
    #include "index.html.gz.inc"
    };

    /* Описание статического HTTP ресурса. При запросе: GET / 
    HTTP Server автоматически отправит содержимое index_html_gz без вызова пользовательского обработчика.*/
    static struct http_resource_detail_static index_html_gz_resource_detail = {
        .common = {
                .type = HTTP_RESOURCE_TYPE_STATIC, // Тип ресурса: статический файл. HTTP Server сам отправит данные без вызова колбэка.
                .bitmask_of_supported_http_methods = BIT(HTTP_GET), // Поддерживаемые HTTP-методы — битовая маска.
                .content_encoding = "gzip", // HTTP заголовок Content-Encoding: gzip. Говорит браузеру что данные сжаты gzip → браузер распакует.
                .content_type = "text/html", // HTTP заголовок Content-Type: text/html. Браузер будет рендерить как HTML.
            },
        .static_data = index_html_gz, // Указатель на данные (наш gzip-массив).
        .static_data_len = sizeof(index_html_gz), // Размер данных в байтах — сервер отправит ровно столько байт.
    };

    // Блок компилируется только при CONFIG_NET_VIDEO_WEBSOCKET_SERVICE=y в prj.conf. Это позволяет собирать проект без WebSocket если он не нужен.
    #if defined(CONFIG_NET_VIDEO_WEBSOCKET_SERVICE)

    /* Буфер WebSocket подсистемы. Используется HTTP Server во время обработки входящих
    WebSocket сообщений. Видеокадры через этот буфер не передаются.*/
    static uint8_t ws_net_video_buffer[128];

    /* Описание WebSocket ресурса.
    После подключения клиента вызывается: ws_net_video_setup() которая запускает периодическую отправку видеокадров.*/
    struct http_resource_detail_websocket ws_net_video_resource_detail = {
        .common = {
                .type = HTTP_RESOURCE_TYPE_WEBSOCKET, // Тип ресурса: WebSocket.
                .bitmask_of_supported_http_methods = BIT(HTTP_GET), // WS-апгрейд начинается с GET-запроса.
            },
        .cb = ws_net_video_setup, // Колбэк: запускает поток, шлющий видеокадры клиенту.
        .data_buffer = ws_net_video_buffer, // Буфер под входящие WS-сообщения.
        .data_buffer_len = sizeof(ws_net_video_buffer), // Размер этого буфера (128 байт).
        .user_data = NULL, // Пользовательские данные не нужны.
    };

    #endif /* CONFIG_NET_VIDEO_WEBSOCKET_SERVICE */

    /* Декларативная регистрация сервера. С помощью макросов собираем "дерево" веб-сервера. 
       Во время компиляции Zephyr сложит их в специальную секцию памяти, и сервер запустится автоматически.*/
    #if defined(CONFIG_NET_VIDEO_HTTP_SERVICE)
    // TCP порт для HTTP сервера. По умолчанию CONFIG_NET_VIDEO_HTTP_SERVER_SERVICE_PORT=80.
    static uint16_t video_http_service_port = CONFIG_NET_VIDEO_HTTP_SERVER_SERVICE_PORT;

    /* Регистрация HTTP сервиса. После вызова http_server_start() сервер автоматически найдёт
    этот сервис и начнёт принимать подключения. */
    HTTP_SERVICE_DEFINE(video_http_service, // Имя сервиса (уникальный идентификатор)
                        NULL, // IP-адрес для привязки (NULL = INADDR_ANY)
                        &video_http_service_port, // Указатель на порт.
                        CONFIG_HTTP_SERVER_MAX_CLIENTS, // Макс. одновременных клиентов из prj.conf.
                        10,  // Backlog (очередь ожидающих соединений)
                        NULL, // Колбэк при старте (не нужен)
                        NULL, // Колбэк при остановке (не нужен)
                        NULL); // Пользовательские данные

    /*  Главная HTML страница.
        HTTP_RESOURCE_DEFINE — макрос, привязывающий URL к ресурсу (статическому, динамическому или WebSocket).*/
    HTTP_RESOURCE_DEFINE(index_html_gz_resource, video_http_service, "/", &index_html_gz_resource_detail);

    #if defined(CONFIG_NET_VIDEO_WEBSOCKET_SERVICE)
    // WebSocket-ресурсы регистрируются точно так же: URL может быть любым, не конфликтующим.
    HTTP_RESOURCE_DEFINE(ws_net_video_resource, video_http_service, "/", &ws_net_video_resource_detail);
    #endif /* CONFIG_NET_VIDEO_WEBSOCKET_SERVICE */
    #endif /* CONFIG_NET_VIDEO_HTTP_SERVICE */

#endif /* CONFIG_SERVER_TRUE */

// Cтатический массив байтов, в который главный цикл main() копирует уже обработанный кадр.
// Из него кадр затем: копируется в shared_frame_buf для отправки по WebSocket и выводится на дисплей через display_vbuf.
#if defined(CONFIG_ESP32_TRUE)
    // Для ESP32 буфер следует разместить во внешнюю PSRAM.
    static uint8_t buf_out[SHARED_FRAME_BUF_SIZE]
        __attribute__((aligned(4)))
        Z_GENERIC_SECTION(.ext_ram.bss);
#endif /* CONFIG_ESP32_TRUE */
#if defined(CONFIG_STM32_TRUE)
    static uint8_t buf_out[SHARED_FRAME_BUF_SIZE];     
#endif /* CONFIG_STM32_TRUE */

// Код компилируется только при включённом через конфиг дисплее в проект.
#if defined(CONFIG_DISPLAY_TRUE)
    // Устройство дисплея из Device Tree (/chosen zephyr,display).
    const struct device *const display_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_display));
#endif /* CONFIG_DISPLAY_TRUE */  

// Порог для детектирования изменения кадра по гистограмме mpix_stats.
#define MPIX_STATS_MOTION_THRESHOLD 5500
// Сравнить две гистограммы mpix_stats поканально.
// Чем больше значение - тем сильнее "разъехалось" распределение яркости между двумя кадрами.
static uint32_t custom_mpix_stats_diff(const struct mpix_stats *a, const struct mpix_stats *b) {
	uint32_t diff = 0;

	for (int i = 0; i < 64; i++) {
		int32_t d = (int32_t)a->y_histogram[i] - (int32_t)b->y_histogram[i];

		diff += (uint32_t)(d < 0 ? -d : d);
	}

	return diff;
}

int main(void)
{
    // Устройство камеры, выбранное в Device Tree через /chosen { zephyr,camera = ... }.
    const struct device *const video_dev   = DEVICE_DT_GET(DT_CHOSEN(zephyr_camera));

    // Указатель на буфер кадра. Изначально указывает на пустую структуру; video_dequeue() перезапишет его.
    struct video_buffer *vbuf = &(struct video_buffer){};
    // Указываем тип буфера — выходной (кадры идут от камеры к приложению).
    vbuf->type = VIDEO_BUF_TYPE_OUTPUT;
    // Формат кадра (разрешение, pixelformat). Заполнится при инициализации камеры.
    struct video_format fmt   = { .type = VIDEO_BUF_TYPE_OUTPUT };
    // Возможности камеры (поддерживаемые форматы и т.п.). Заполнятся при инициализации.
    struct video_caps   caps  = { .type = VIDEO_BUF_TYPE_OUTPUT };
    // Переменная для хранения кодов возврата.
    int ret;

    // Выполнить полный цикл инициализации видеоустройства (камеры) перед запуском видеопотока.
    ret = app_init_video(video_dev, &caps, &fmt);
    if (ret < 0) { goto err; }

    // Код компилируется только при включённом через конфиг дисплее в проект.
    #if defined(CONFIG_DISPLAY_TRUE)
        // Проверка: описан ли дисплей в Device Tree.
        if (DT_HAS_CHOSEN(zephyr_display)) {
            // Настраиваем дисплей под формат пикселей камеры.
            ret = app_setup_display(display_dev, fmt.pixelformat);
            if (ret < 0) { goto err; }
            // Очищаем экран перед началом вывода от шумов.
            ret = app_clear_display(display_dev);
            if (ret < 0) { goto err; }
        }
    #endif /* CONFIG_DISPLAY_TRUE */  

    // Поднимаем сеть, DHCP или WiFi, в зависимости от того какая платформа была выбрана для проекта.
    // Код компилируется только при включённом через конфиг сервере в проект.
    #if defined(CONFIG_SERVER_TRUE)
        // Код компилируется при условии что платформой для проекта выбрана плата STM32.
        #if defined(CONFIG_STM32_TRUE)
            // Указатель на сетевой интерфейс (Ethernet).
            struct net_if *iface;
            // Инициализируем колбэк: handler вызовется, когда интерфейсу назначат IPv4-адрес.
            net_mgmt_init_event_callback(&mgmt_cb, handler, NET_EVENT_IPV4_ADDR_ADD);
            // Регистрируем колбэк в сетевом менеджере событий.
            net_mgmt_add_event_callback(&mgmt_cb);
            // Получаем сетевой интерфейс по умолчанию.
            iface = net_if_get_default();
            // Запускаем DHCP-клиент (адрес придёт асинхронно). 
            net_dhcpv4_start(iface);
        #endif /* CONFIG_STM32_TRUE */

        // Код компилируется при условии что платформой для проекта выбрана плата ESP32.
        #if defined(CONFIG_ESP32_TRUE)
            // Регистрируем WiFi-колбэки.
            wifi_init();

            /* 2. Запускаем подключение к роутеру */
            printk("Connecting to WiFi: %s\r\n", WIFI_SSID);
            if (wifi_connect(WIFI_SSID, WIFI_PSK) != 0) {
                printk("Error: WiFi connect failed\r\n");
                return 0;
            }

            /* 3. Ждем получения IP-адреса от DHCP (блокирующе) */
            wifi_wait_for_ip_addr();
            printk("WiFi Connected and IP obtained! Starting HTTP Server...\n");
        #endif /* CONFIG_ESP32_TRUE */
   
        // ГЛАВНАЯ ФУНКЦИЯ — запускает HTTP/WebSocket сервер.
        http_server_start();
    #endif /* CONFIG_SERVER_TRUE */

    // Запуск видеопотока камеры, камера начинает захватывать кадры в буферы через DMA.
    LOG_INF("Capture started");
    ret = video_stream_start(video_dev, VIDEO_BUF_TYPE_OUTPUT);
    if (ret < 0) {
        LOG_ERR("Unable to start capture (interface)");
        goto err;
    }

    // Создаётся и сразу запускается отдельный поток ядра Zephyr, в котором работает пользовательский интерфейс на LVGL.
    k_thread_create(&ui_thread_data, // адрес управляющей структуры потока (struct k_thread). В ней ядро хранит состояние потока: сохранённые регистры, приоритет, состояние планировщика и т.д.
                    ui_thread_stack, // область памяти под стек потока.
                    K_THREAD_STACK_SIZEOF(ui_thread_stack), // макрос Zephyr, который возвращает ПОЛЕЗНЫЙ размер стека в байтах.
                    ui_thread_entry, // функция-точка входа потока, нутри неё: инициализация LVGL, создание виджетов и бесконечный цикл с lv_timer_handler() и k_msleep().
                    NULL, NULL, NULL, // три произвольных указателя, которые ядро передаст в ui_thread_entry как параметры.
                    UI_THREAD_PRIORITY, // приоритет потока.
                    0, // битовая маска опций потока, 0 - без особых опций (K_USER, K_FP_REGS, K_ESSENTIAL, K_INHERIT_PERMS и др.).
                    K_NO_WAIT); // delay — задержка старта потока.

    // Потоку присваивается читаемое имя (видно в отладчике, в shell-команде "kernel threads", в Thread Analyzer).
    k_thread_name_set(&ui_thread_data, "ui_thread");

    // Структура библиотеки libmpix, описывает изображение и цепочку операций над ним.
    struct mpix_image img = {};
    // Переменная для кодов возврата функций.
    int err;

    // Статистика эталонного кадра, с которым сравнивается каждый текущий.
    struct mpix_stats stats_reference = {};
    // Статистика текущего (только что захваченного) кадра.
    struct mpix_stats stats_current = {};
    // Флаг: эталонный кадр ещё ни разу не задавался.
    bool have_reference = false;
    // Переменная для отслеживания изменения состояния индикатора в цикле.
    bool prev_state = state;

	/* ============================================================
     * ГЛАВНЫЙ ЦИКЛ ЗАХВАТА КАДРОВ
     *   1. video_dequeue() — извлечь готовый кадр
     *   2. (опционально) memcpy → k_sem_give — передать кадр для дальнейшей обработки
     *   3. video_enqueue() — вернуть буфер драйверу
     * ============================================================ */
    while (1) {
        /* Блокирующее ожидание следующего готового кадра от камеры. */
        ret = video_dequeue(video_dev, &vbuf, K_FOREVER);
        if (ret < 0) {
            LOG_ERR("Unable to dequeue video buf");
            goto err;
        }

        /* Инвалидируем D-Cache, чтобы CPU (через libmpix) прочитал из RAM свежие данные, записанные туда DMA камеры. */
        sys_cache_data_invd_range(vbuf->buffer, vbuf->bytesused);

        /* Структура libmpix, описывающая формат изображения. */
        struct mpix_format my_fmt = {
            // .fourcc = fmt.pixelformat, // код формата пикселей в виде FourCC.
            .fourcc = (fmt.pixelformat == VIDEO_PIX_FMT_RGB565) ? VIDEO_PIX_FMT_RGB565X : fmt.pixelformat,
            .width  = CONFIG_VIDEO_FRAME_WIDTH, // ширина кадра в пикселях.
            .height = CONFIG_VIDEO_FRAME_HEIGHT, // высота кадра в пикселях.
        };

        // Привязка входных данных к контексту libmpix. Заполняет struct mpix_image:
        // запоминает указатель на входные данные, их размер и формат и сбрасывает цепочку операций.
        mpix_image_from_buf(&img, (const uint8_t *)vbuf->buffer, vbuf->bytesused, &my_fmt);

        // Конвертация изображения в RGB24 (по 3 байта на пиксель: R, G, B).
        err = mpix_image_convert(&img, MPIX_FMT_RGB24);
        if (err) {
            LOG_ERR("Failed to convert to RGB24: %d", err);
            return err;
        }

        /* Кол-во точек на изображении для расчёта статистики (0 - использовать значение по умолчанию (1000)). */
        stats_current.nvals = 3000; 
        /* Рассчитать статистику текущего кадра. */
        mpix_stats_from_buf(&stats_current, (const uint8_t *)img.buffer, &img.fmt);

        // Переменная btn_set переходит в состояние true при клике по кнопке.
        if (btn_set) {
            // Зафиксировать статистику текущего кадра как новый эталон для сравнения.
            stats_reference = stats_current;
            // Эталонный кадр задан.
            have_reference = true;
            // Флаг обработан - сбрасываем, чтобы не пересчитывать эталон на каждом следующем кадре подряд.
            btn_set = false;
            LOG_INF("Reference frame captured (nvals=%u)", stats_reference.nvals);
        } else if (have_reference) {
            // Сравнить текущий кадр с эталоном по гистограмме.
            diff = custom_mpix_stats_diff(&stats_reference, &stats_current);

            LOG_INF("stats diff = %u (threshold = %u)", diff, MPIX_STATS_MOTION_THRESHOLD);

            // В зависимости от того превышает ли рассчитанная между двумя кадрами метрика 
            // пороговое значение меняем состояние state чтобы отобразить изменилась ли композиция кадра или нет.
            if (diff > MPIX_STATS_MOTION_THRESHOLD) {
                state = false;
            } else {
                state = true;
            }
        }

        // При изменении состояния state:
        if (state != prev_state) {
            // Вызываем ws_broadcast_state() чтобы отобразить изменение на вебстраничке.
            // Перенёс ws_broadcast_state в поток thread_ui.c чтобы не зависал главный поток.
            // ws_broadcast_state();
            // Переводим флаг lvgl_dirty_state чтобы отобразить изменение на дисплее.
            atomic_set(&lvgl_dirty_state, 1);
            // Сохраняем новое состояние.
            prev_state = state;
        }

        // Конвертируем обратно в формат, который использует камера/дисплей.
        // err = mpix_image_convert(&img, fmt.pixelformat);
        err = mpix_image_convert(&img, (fmt.pixelformat == VIDEO_PIX_FMT_RGB565) ? VIDEO_PIX_FMT_RGB565X : fmt.pixelformat);
        if (err) {
            LOG_ERR("Failed to convert back to original format: %d", err);
            return err;
        }

        // Записываем обработанный кадр в buf_out.
        err = mpix_image_to_buf(&img, buf_out, sizeof(buf_out));
        mpix_image_free(&img);
        if (err) {
            LOG_ERR("Failed to write to buf_out: %d", err);
            return err;
        }

        // Код компилируется только при включённом через конфиг сервере в проект.
        #if defined(CONFIG_SERVER_TRUE)
            /* Если предыдущий кадр отправлен */
            if (atomic_cas(&frame_is_sended, 1, 0)) {

                /* Инвалидируем D-Cache, чтобы процессор прочитал из RAM свежие данные DMA */
                sys_cache_data_invd_range(buf_out, SHARED_FRAME_BUF_SIZE);
                /* Копируем кадр из буфера камеры в общий буфер для сетевого потока. */
                memcpy(shared_frame_buf, buf_out, SHARED_FRAME_BUF_SIZE);
                // Сбрасываем кэш в RAM, чтобы сетевой стек/DMA увидел записанные нами данные.
                sys_cache_data_flush_range(shared_frame_buf, SHARED_FRAME_BUF_SIZE);
                // Сообщаем отправляющему потоку реальный размер кадра.
                shared_frame_bytesused = SHARED_FRAME_BUF_SIZE;
                //  Будим поток WebSocket: разрешаем отпраку кадра по соединению вебсокетов.
                k_sem_give(&allowed_2_send);
            }
        #endif /* CONFIG_SERVER_TRUE */

        // Код компилируется только при включённом через конфиг дисплее в проект.
        #if defined(CONFIG_DISPLAY_TRUE)
            // Проверка дисплея (если он задан в Device Tree).
            if (DT_HAS_CHOSEN(zephyr_display)) {
                /* Структура Zephyr, описывающая формат изображения. */
                struct video_buffer display_vbuf = {
                    .buffer    = buf_out, // указатель на область памяти с пикселями.
                    .bytesused = sizeof(buf_out), // сколько байт в нём полезных данных (заполняет драйвер).
                    .size      = sizeof(buf_out), // ёмкость буфера в байтах (сколько вообще влезает).
                    .type      = VIDEO_BUF_TYPE_OUTPUT, // направление буфера (VIDEO_BUF_TYPE_INPUT/OUTPUT).
                };

                // Вывести один видеокадр из буфера камеры на дисплей.
                ret = app_display_frame(display_dev, &display_vbuf, &fmt);
                if (ret != 0) {
                    LOG_WRN("Failed to display this frame");
                }
            }
        #endif /* CONFIG_DISPLAY_TRUE */    

        /* Возвращаем буфер видеодрайверу для следующего захвата DMA.
         * После этого vbuf->buffer принадлежит драйверу — не трогаем! */
        ret = video_enqueue(video_dev, vbuf);
        if (ret < 0) {
            LOG_ERR("Unable to requeue video buf");
            goto err;
        }
    }

err:
    LOG_ERR("Aborting sample");
    return 0;
}