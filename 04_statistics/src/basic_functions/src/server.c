// Стандартный ввод-вывод для логирования
#include <stdio.h>
// POSIX-совместимые сокетные типы (socklen_t, sockaddr и т.д.). Определяет базовые сетевые структуры и типы, принятые в UNIX-системах.
#include <zephyr/posix/sys/socket.h>
// Функция poll() позволяет "слушать" несколько сокетов одновременно, не блокируя поток наглухо, если данных нет.
#include <zephyr/posix/poll.h>
// Основные функции RTOS: потоки, таймеры, семафоры
#include <zephyr/kernel.h>
// TLS учётные данные (нужны для компиляции STACK_SIZE). Transport lLayer Security — протокол защиты транспортного уровня
#include <zephyr/net/tls_credentials.h>
// Встроенный HTTP server subsystem Zephyr.
#include <zephyr/net/http/server.h>
// Регистрация HTTP-сервисов и ресурсов.
#include <zephyr/net/http/service.h>
// IP-адреса и сетевые примитивы.
#include <zephyr/net/net_ip.h>
// Zephyr-специфичные сокетные функции (zsock_*)
#include <zephyr/net/socket.h>
// Обертка Zephyr для работы с протоколом WebSocket.
#include <zephyr/net/websocket.h>
// Сетевая статистика и управление сетью.
#include <zephyr/net/net_mgmt.h>
// #include <zephyr/net/net_stats.h>
// Автоматическая инициализация подсистем.
#include <zephyr/init.h>
// Видео-подсистема Zephyr.Содержит: struct video_buffer, struct video_format, video_enqueue(), video_dequeue() и другие API видеоустройств.
#include <zephyr/drivers/video.h> 

// Регистрируем модуль логирования.
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(SERVER_LOG, LOG_LEVEL_DBG);


// Заголовок проекта
#include "server.h"


#if defined(CONFIG_STM32_TRUE)
	/* Общий буфер кадров.
	* В этот буфер поток захвата камеры копирует последний полученный кадр.
	* WebSocket-потоки читают данные отсюда и отправляют их клиентам.*/
	uint8_t shared_frame_buf[SHARED_FRAME_BUF_SIZE];
#endif /* CONFIG_STM32_TRUE */


#if defined(CONFIG_ESP32_TRUE)
	/* Общий буфер кадров.*/
	uint8_t shared_frame_buf[SHARED_FRAME_BUF_SIZE]
    __attribute__((aligned(32)))	/* Буфер размещается во внешней PSRAM. aligned(32) Выравнивание по 32 байтам для DMA и кэша.*/
    __attribute__((section(".ext_ram.bss")));	/* section(".ext_ram.bss")Помещает буфер во внешнюю память ESP32-S3.*/
#endif /* CONFIG_ESP32_TRUE */

/* Количество реально используемых байт в текущем кадре. */
size_t shared_frame_bytesused;

// Флаг дающий знать то, что кадр отправлен.
atomic_t frame_is_sended = ATOMIC_INIT(1);
// Семафор разрешающий отправку кадра.
K_SEM_DEFINE(allowed_2_send, 0, 1);

// Размер стека для потоков WebSocket обработчиков.
#define STACK_SIZE 2048

// Приоритет потоков WebSocket обработчиков (кооперативный или вытесняющий приоритет).
// #define THREAD_PRIORITY K_PRIO_COOP(CONFIG_NUM_COOP_PRIORITIES - 1)
// PREEMPT означает вытесняемый поток.
#define THREAD_PRIORITY K_PRIO_PREEMPT(8)


// Без userspace: аннотации ничего не делают (пустые макросы).
#define APP_BMEM
#define APP_DMEM

// Максимальное количество одновременных WebSocket клиентов. Определяется в prj.conf.
#define MAX_CLIENT_QUEUE CONFIG_NET_VIDEO_NUM_WEBSOCKET_HANDLERS
// Размер буфера приёма для echo обработчика в байтах. 1280 байт — типичный MTU для IPv6 (минимальный MTU по стандарту).
#define RECV_BUFFER_SIZE 1280

// Контекст одного подключённого WebSocket клиента для видео сервера.
struct ws_net_video_ctx {
	// Дескриптор WebSocket соединения клиента.
	int sock; 
	// Отложенная задача (таймер)
	struct k_work_delayable work;
};

/* Массив стеков для потоков WebSocket обработчиков.
 * Размер массива: CONFIG_NET_VIDEO_NUM_WEBSOCKET_HANDLERS соответствует максимально допустимому количеству одновременно
 * подключённых WebSocket клиентов. Каждый обработчик получает собственный стек размером STACK_SIZE. */
K_THREAD_STACK_ARRAY_DEFINE(ws_handler_stack, // массив стеков ws_handler_stack[NUM_CLIENTS][STACK_SIZE].
			    			CONFIG_NET_VIDEO_NUM_WEBSOCKET_HANDLERS, // Максимальное количество одновременных WebSocket клиентов.
			    			STACK_SIZE); 			// Размер стека для потоков WebSocket обработчиков.


// Массив контекстов для WebSocket клиентов. {sock, work} — Каждый элемент массива соответствует одному подключённому клиенту..
static struct ws_net_video_ctx net_video_ctx[CONFIG_NET_VIDEO_NUM_WEBSOCKET_HANDLERS];


// Найти первый свободный слот в массиве контекстов net_video_ctx.
static int get_free_net_video_slot(void)
{
	/* Возвращает: >= 0 индекс свободного элемента, -1 свободных слотов нет. */
	for (int i = 0; i < CONFIG_NET_VIDEO_NUM_WEBSOCKET_HANDLERS; i++) {
		if (net_video_ctx[i].sock < 0) {
			return i;
		}
	}
	return -1;
}

/* Размер стека потока Work Queue. */
#define WS_NET_VIDEO_STACK_SIZE 2048
/* Стек потока Work Queue. Все k_work_delayable задачи отправки видеокадров выполняются внутри этого потока. */
K_THREAD_STACK_DEFINE(ws_net_video_stack, WS_NET_VIDEO_STACK_SIZE);
/* Очередь отложенных задач отправки видеокадров. Именно этот поток вызывает net_video_handler().
   Управляет очередью задач (k_work_delayable) и выполняет их в своём потоке когда наступает время.*/
struct k_work_q ws_net_video_queue;



/* ------------------------------------------------------------------------------------------------- */
// atoi — преобразование строки в целое число
#include <stdlib.h>
// strcmp — сравнение строк
#include <string.h> 
// парсинг JSON — десериализация входящих сообщений от веб-клиента
#include <zephyr/data/json.h>

/* Булево значение отображает состояние кнопки SET на HTML-странице или на дисплее через LVGL.
   Если булево значение true, то значит кнопка была нажата, кадр нужно зафиксировать как референс для последующего сравнения. */
volatile bool btn_set = true;
// Булево значение показывает изменилась ли композиция кадра или нет. 
volatile bool state = false;
/* Флаг для UI-потока чтобы LVGL дисплей мог дать ту же визуальную обратную связь, что и html вебстраничка. */
atomic_t lvgl_dirty_state = ATOMIC_INIT(0);
// Переменная чтобы хранить метрику сравнения текущего кадра с эталоном по гистограмме.
volatile uint32_t diff = 0;


/* Сериализует все отправки в видео-сокеты: и периодическую отправку
 * кадров (net_video_handler), и рассылку значений слайдеров
 * (ws_broadcast_slider). Без мьютекса два потока могут одновременно
 * писать в один сокет и испортить WebSocket-поток. */
K_MUTEX_DEFINE(ws_send_lock);

// Функция передающая новое состояние state на вебстраничку.
void ws_broadcast_state(void)
{
	char buf[64];
	int len = snprintf(buf, sizeof(buf), "{\"type\":\"state\",\"value\":%s}", state ? "true" : "false");

	k_mutex_lock(&ws_send_lock, K_FOREVER);
	for (int i = 0; i < CONFIG_NET_VIDEO_NUM_WEBSOCKET_HANDLERS; i++) {
		if (net_video_ctx[i].sock < 0) {
			continue;
		}
		websocket_send_msg(net_video_ctx[i].sock, (uint8_t *)buf, len,
				    WEBSOCKET_OPCODE_DATA_TEXT, false, true, SYS_FOREVER_MS);
	}
	k_mutex_unlock(&ws_send_lock);
}


// Структура под входящее сообщение от кнопки SET на веб-странице.
struct btn_set_msg {
	// Указатель на строку с типом параметра.
	const char *type;
};

// Статический массив дескрипторов полей JSON, который описывает структуру JSON-объекта для парсера Zephyr
static const struct json_obj_descr btn_set_msg_descr[] = {
	// Описывает поле type как JSON-строку.
	JSON_OBJ_DESCR_PRIM(struct btn_set_msg, type, JSON_TOK_STRING),
};

// Функция разбирает одно сообщение и применяет его к нужной переменной.
static void apply_btn_set_msg(char *buf, size_t len) {
	struct btn_set_msg msg = {0};
	int ret;

	ret = json_obj_parse(buf, len, btn_set_msg_descr,
			      ARRAY_SIZE(btn_set_msg_descr), &msg);
	if (ret != BIT_MASK(ARRAY_SIZE(btn_set_msg_descr))) {
		LOG_WRN("Bad btn_set JSON (ret=%d): %.*s", ret, (int)len, buf);
		return;
	}

	if (strcmp(msg.type, "btn_set") == 0) {
		btn_set = true;
		// atomic_set(&lvgl_dirty_state, 1);
		LOG_INF("btn_set = true (from web)");
		return;
	}
}

/*
 * Неблокирующая проверка: не прислал ли клиент клик по кнопке SET.
 */
static void check_btn_set_commands(int sock) {
	static APP_BMEM char recv_buffer[RECV_BUFFER_SIZE];
	uint32_t message_type;
	uint64_t remaining;
	int ret;

	while (1) {
		ret = websocket_recv_msg(sock, recv_buffer, sizeof(recv_buffer) - 1,
					  &message_type, &remaining, ZSOCK_MSG_DONTWAIT);
		if (ret < 0) {
			break; // нет данных - нормальная ситуация
		}

		if (message_type & WEBSOCKET_FLAG_TEXT) {
			recv_buffer[ret] = '\0';
			apply_btn_set_msg(recv_buffer, ret);
		}
	}
}
/* ------------------------------------------------------------------------------------------------- */




/* Обработчик периодической отправки видеокадров клиенту. Вызывается потоком Work Queue.
 * Алгоритм работы:
 *   1. Дождаться готового кадра.
 *   2. Отправить кадр через WebSocket.
 *   3. Переключиться на следующий буфер.
 *   4. Разрешить запись нового кадра.
 *   5. Запланировать следующую отправку.*/
static void net_video_handler(struct k_work *work)
{
	// Объявляем локальную переменную ret.
	int ret;

	/* Получить указатель на k_work_delayable, которому принадлежит данный work.*/
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);

	/* Получить указатель на клиентский контекст. dwork является полем структуры ws_net_video_ctx, 
	поэтому через CONTAINER_OF() восстанавливаем адрес всей структуры целиком. */
    struct ws_net_video_ctx *ctx = CONTAINER_OF(dwork, struct ws_net_video_ctx, work);

	LOG_INF("net_video_handler fired, sock=%d, bytes=%u", ctx->sock, shared_frame_bytesused);


	/* ------------------------------------------------------------------------------------------------- */

	/* Проверить, не прислал ли клиент новое значение слайдера. */
	check_btn_set_commands(ctx->sock);

	/* ------------------------------------------------------------------------------------------------- */


	/* Ждать пока поток камеры подготовит новый кадр. */
	k_sem_take(&allowed_2_send, K_FOREVER);

	// Захватывает мьютекс. Блокирует выполнение, пока поток не освободит его.
	k_mutex_lock(&ws_send_lock, K_FOREVER);
	/* Отправить текущий кадр как бинарный WebSocket пакет. */
    ret = websocket_send_msg(ctx->sock,
                             shared_frame_buf,    /* данные */
                             shared_frame_bytesused,         		/* размер */
                             WEBSOCKET_OPCODE_DATA_BINARY,
                             false,                          /* не маскировать */
                             true,                           /* финальный фрагмент */
                             SYS_FOREVER_MS);
	// Освобождает мьютекс. Теперь другой поток может отправить данные.
	k_mutex_unlock(&ws_send_lock);/* ------------------------------------------------------------------------------------------------- */

	/* Разрешить запись нового кадра. 
     * Сообщить потоку камеры, что предыдущий кадр полностью отправлен.
     * После этого камера может записывать новый кадр.*/
	atomic_set(&frame_is_sended, 1);
	
	/* Ошибка отправки обычно означает,
     * что клиент закрыл соединение.*/
    if (ret < 0) {
        LOG_INF("Couldn't send websocket msg (%d), closing connection", ret);
        goto unregister;
    }

    /* Перепланировать задачу через заданный интервал CONFIG_NET_VIDEO_WEBSOCKET_FRAME_INTERVAL.
     * Запланировать следующую отправку кадра. Через заданный интервал net_video_handler будет вызван повторно для отправки кадра.*/
    ret = k_work_reschedule_for_queue(&ws_net_video_queue, &ctx->work,
                                      K_MSEC(CONFIG_NET_VIDEO_WEBSOCKET_FRAME_INTERVAL));

	LOG_INF("reschedule ret=%d, interval=%d ms", ret, CONFIG_NET_VIDEO_WEBSOCKET_FRAME_INTERVAL);
    if (ret < 0) {
        LOG_ERR("Failed to schedule net video work, err %d", ret);
        goto unregister;
    }


    return;

unregister:
 	/* Удалить WebSocket соединение из подсистемы.*/
    (void)websocket_unregister(ctx->sock);
	/* Пометить слот клиента как свободный. */
    ctx->sock = -1;
}


/* Инициализация подсистемы видеостриминга через WebSocket.
 * Выполняется автоматически во время загрузки системы через SYS_INIT(), ещё до входа в main().
 * Функция:
 *   1. Создаёт и запускает Work Queue.
 *   2. Инициализирует все клиентские контексты.
 *   3. Подготавливает отложенные задачи отправки видеокадров.
 * Возвращает:0 - успешная инициализация.*/
int ws_net_video_init(void)
{
	// Конфигурация потока Work Queue.
	// ws_net_video_q - имя потока отображается в отладочных инструментах и помогает идентифицировать его во время анализа системы. (видно в k_thread_list).
	struct k_work_queue_config cfg = {.name = "ws_net_video_q"};

	/* Инициализировать структуру очереди задач. Подготавливает внутренние списки и служебные данные Work Queue. */
	k_work_queue_init(&ws_net_video_queue);

	// Запустить поток Work Queue. 
	// Поток будет спать пока нет задач, просыпаться и выполнять их.
	// ws_net_video_queue - объект очереди задач.
    // ws_net_video_stack - стек для потока WQ.
    // WS_NET_VIDEO_STACK_SIZE - размер стека (2048 байт).
    // 0                  - приоритет 0 (высокий, чтобы не задерживать отправку).
    // &cfg               - конфигурация (с именем потока).
	k_work_queue_start(&ws_net_video_queue, ws_net_video_stack, WS_NET_VIDEO_STACK_SIZE, 0, &cfg);

	/* Инициализировать массив клиентских контекстов. */
	for (int i = 0; i < CONFIG_NET_VIDEO_NUM_WEBSOCKET_HANDLERS; i++) {
		// Инициализировать все слоты как свободные.
		net_video_ctx[i].sock = -1;
		// Инициализировать отложенную задачу для каждого слота. После инициализации задача готова к планированию через k_work_reschedule_for_queue().
		k_work_init_delayable(&net_video_ctx[i].work, net_video_handler);
	}
	// SYS_INIT требует возврата 0 при успехе.
	return 0;
}

// Зарегистрировать ws_net_video_init как функцию инициализации.
//   fn       — функция инициализации
//   level    — уровень инициализации (порядок запуска): PRE_KERNEL_1, PRE_KERNEL_2, POST_KERNEL, (APPLICATION - Выполнить после запуска ядра и основных подсистем.)
//   priority — Приоритет внутри уровня APPLICATION. Чем меньше число, тем раньше будет вызвана функция.
SYS_INIT(ws_net_video_init, APPLICATION, 8);



// Обработчик входящих подключений: занять слот, сохранить сокет, запланировать первую отправку.
//  1. Выделяет свободный клиентский слот.
//  2. Сохраняет сокет клиента.
//  3. Запускает первую задачу отправки видеокадров.
// 		ws_socket    — дескриптор WebSocket соединения.
// 		request_ctx  — контекст HTTP запроса (заголовки, URL и т.д.) — не используем.
// 		user_data    — пользовательские данные (ws_echo_resource_detail.user_data = NULL) — не используем.
int ws_net_video_setup(int ws_socket, struct http_request_ctx *request_ctx, void *user_data)
{
	// Объявляем локальную переменную ret.
	int ret;
	// Объявляем локальную переменную slot.
	int slot;

	/* Найти свободный слот для нового клиента. */
	slot = get_free_net_video_slot();
	// Все доступные слоты заняты. Сервер достиг максимально допустимого количества одновременно подключённых клиентов.
	if (slot < 0) {
		LOG_ERR("Cannot accept more net video websocket connections");
		return -ENOENT;
	}

	// Сохранить дескриптор WebSocket соединения клиента в его контексте.
	net_video_ctx[slot].sock = ws_socket;

	// Запланировать первую отправку кадра.
	// Work Queue поток проснётся и вызовет net_video_handler() немедленно.
    // После первой отправки net_video_handler сам перепланирует себя через INTERVAL.
	ret = k_work_reschedule_for_queue(&ws_net_video_queue, 			// Очередь отложенных задач (запущена в ws_net_video_init).
										&net_video_ctx[slot].work, 	// Отложенная задача для этого клиента.
										K_NO_WAIT);					// Выполнить как можно скорее.

	// Не удалось запланировать выполнение операции net_video_handler для подключившегося клиента.
	if (ret < 0) {
		LOG_ERR("Failed to schedule net video work, err %d", ret);
		return ret;
	}

	// Установлено соединение WebSocket для отправки кадров по сети. 
	LOG_INF("Accepted websocket connection for net video");
	return 0;
}


