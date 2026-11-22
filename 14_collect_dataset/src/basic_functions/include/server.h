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



// Переменная хранящая ширину уменьшеного изображения.
#define DOWNSAMPLE_WIDTH   32
// Переменная хранящая высоту уменьшеного изображения.
#define DOWNSAMPLE_HEIGHT  32
// Размер буфера для уменьшеного изображения.
#define DOWNSAMPLE_BUF_SIZE (DOWNSAMPLE_WIDTH * DOWNSAMPLE_HEIGHT * 3)



/* Разделяемые данные между main и потоком вывода */
/* Общий буфер кадров.
	* В этот буфер поток захвата камеры копирует последний полученный кадр.
	* WebSocket-потоки читают данные отсюда и отправляют их клиентам.*/
extern uint8_t shared_frame_buf[DOWNSAMPLE_BUF_SIZE];
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
/* Булево значение отображает состояние кнопки SET на HTML-странице или на дисплее через LVGL.
   Если булево значение true, то значит кнопка была нажата, кадр нужно зафиксировать как референс для последующего сравнения. */
// extern volatile bool btn_set;
// Булево значение показывает изменилась ли композиция кадра или нет. 
// extern volatile bool state;
// /* Флаг для UI-потока чтобы LVGL дисплей мог дать ту же визуальную обратную связь, что и html вебстраничка. */
// extern atomic_t lvgl_dirty_state;
// // Переменная чтобы хранить метрику сравнения текущего кадра с эталоном по гистограмме.
// extern volatile uint32_t diff;
void ws_broadcast_state(void);

// // Переменная хранящая ширину уменьшеного изображения.
// extern volatile uint32_t downsample_width;
// // Переменная хранящая высоту уменьшеного изображения.
// extern volatile uint32_t downsample_height;




/* ------------------------------------------------------------------------------------------------- */