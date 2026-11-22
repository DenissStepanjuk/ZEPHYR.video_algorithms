// Предоставляет API встроенного HTTP-сервера Zephyr. 
#include <zephyr/net/http/server.h>
// Реализует атомарные операции над целочисленными переменными.
#include <zephyr/sys/atomic.h>
// Главный заголовок ядра Zephyr RTOS. 
#include <zephyr/kernel.h>
// Видео-подсистема Zephyr.Содержит: struct video_buffer, struct video_format, video_enqueue(), video_dequeue() и другие API видеоустройств.
#include <zephyr/drivers/video.h>

/* Размер буфера для одного видеокадра (байт). */
#define SHARED_FRAME_BUF_SIZE (CONFIG_VIDEO_BUFFER_POOL_HEAP_SIZE / CONFIG_VIDEO_BUFFER_POOL_NUM_MAX)

/* Разделяемые данные между main и потоком вывода */
/* Общий буфер кадров.
	* В этот буфер поток захвата камеры копирует последний полученный кадр.
	* WebSocket-потоки читают данные отсюда и отправляют их клиентам.*/
extern uint8_t shared_frame_buf[SHARED_FRAME_BUF_SIZE];
/* Количество реально используемых байт в текущем кадре. */
extern size_t shared_frame_bytesused;
// Флаг дающий знать то, что кадр отправлен.
extern atomic_t frame_is_sended;
// Семафор разрешающий отправку кадра.
extern struct k_sem allowed_2_send;

/*  Функции настройки WebSocket (объявлены в func_4_server.c)
    Обработчик входящих подключений: занять слот, сохранить сокет, запланировать первую отправку. */
int ws_net_video_setup(int ws_socket, struct http_request_ctx *request_ctx, void *user_data);



/* ------------------------------------------------------------------------------------------------- */

extern volatile float red_balance;
extern volatile float blue_balance;
extern volatile float gamma_level;


// "Грязные" флаги: LVGL-слайдеру нужно подтянуть значение, пришедшее
// с веб-страницы. Пишутся из потока веб-сервера, читаются в ui_thread -
// поэтому atomic_t, а не обычный bool.
extern atomic_t lvgl_dirty_red;
extern atomic_t lvgl_dirty_blue;
extern atomic_t lvgl_dirty_gamma;

// Разослать новое значение слайдера всем подключённым веб-клиентам.
// Вызывается из ui_thread, когда пользователь подвинул слайдер на экране.
void ws_broadcast_slider(const char *type, float value);

/* ------------------------------------------------------------------------------------------------- */