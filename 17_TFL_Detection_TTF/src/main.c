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
// Поток выделенный под обработку изображения.
#include "thread_processing.h"

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

// Код компилируется только при включённом через конфиг дисплее в проект.
#if defined(CONFIG_DISPLAY_TRUE)
    // Устройство дисплея из Device Tree (/chosen zephyr,display).
    const struct device *const display_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_display));
#endif /* CONFIG_DISPLAY_TRUE */  

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

    /* Запускаем поток обработки изображения (thread_processing.c). Он будет
     * спать на allowed_2_process, пока main() не подготовит очередной RGB24-кадр. */
    processing_thread_init();

    // Структура библиотеки libmpix, описывает изображение и цепочку операций над ним.
    struct mpix_image img = {};
    // Переменная для кодов возврата функций.
    int err;

    /* Формат нативного кадра камеры (fourcc/ширина/высота) — заполняем
     * один раз до входа в цикл захвата, т.к. эти значения не меняются
     * от кадра к кадру. Используется и здесь, и в потоке обработки. */
    // my_fmt.fourcc = fmt.pixelformat;
    my_fmt.fourcc = (fmt.pixelformat == VIDEO_PIX_FMT_RGB565) ? VIDEO_PIX_FMT_RGB565X : fmt.pixelformat;
    my_fmt.width  = CONFIG_VIDEO_FRAME_WIDTH;
    my_fmt.height = CONFIG_VIDEO_FRAME_HEIGHT;


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

        // Если предыдущий кадр обработан, то передаём текущий в поток обработки.
        if (atomic_cas(&frame_processing_is_done, 1, 0)) {
        /* ---------------- Захват + конвертация в RGB24 ---------------- */
            mpix_image_from_buf(&img, (const uint8_t *)vbuf->buffer, vbuf->bytesused, &my_fmt);

            // Преобразуем текущий кадр в формат MPIX_FMT_RGB24.
            err = mpix_image_convert(&img, MPIX_FMT_RGB24);
            if (err) {
                LOG_ERR("Failed to convert to RGB24: %d", err);
                mpix_image_free(&img);
                ret = video_enqueue(video_dev, vbuf);
                if (ret < 0) {
                    LOG_ERR("Unable to requeue video buf");
                    goto err;
                }
                continue;
            }
 
            /* Передаём RGB24 кадр в поток обработки. */
            err = mpix_image_to_buf(&img, buf_processing, FRAME_RGB24_SIZE);
            if (err) {
                LOG_ERR("Failed to materialize RGB24 frame: %d", err);
                /* Освобождаем слот обратно, раз кадр не подготовлен. */
                atomic_set(&frame_processing_is_done, 1);
            } else {
                sys_cache_data_flush_range(buf_processing, FRAME_RGB24_SIZE);
                shared_fmt = fmt;
                k_sem_give(&allowed_2_process);
            }
        }
        // Освобождаем структуру mpix_image.
        mpix_image_free(&img);

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