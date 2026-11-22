#ifndef THREAD_PROCESSING_H_
#define THREAD_PROCESSING_H_

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <mpix/image.h>
#include <zephyr/drivers/video.h>

/* Формат кадра (разрешение, pixelformat), согласованный с видеодрайвером.
 * Заполняется в main() и используется потоком обработки при выводе на дисплей. */
extern struct video_format shared_fmt;

/* Размер кадра в формате RGB24 (160x120x3 байта на пиксель). Используется
 * и в main() при копировании кадра в поток обработки, и внутри самого
 * потока обработки при разборе буфера через mpix_image_from_buf(). */
#define FRAME_RGB24_SIZE (CONFIG_VIDEO_FRAME_WIDTH * CONFIG_VIDEO_FRAME_HEIGHT * 3)

/* Сырой RGB24-кадр, переданный из main() в поток обработки. Заполняется в main()
 * после video_dequeue() + конвертации в RGB24, пока поток обработки ждёт на
 * allowed_2_process. */
extern uint8_t buf_processing[FRAME_RGB24_SIZE];
// extern uint8_t buf_RGB[FRAME_RGB24_SIZE];

/* Флаг: 1 — поток обработки закончил работу с buf_frame_4_processing и
 * main() может безопасно перезаписать буфер новым кадром. 0 — буфер занят.
 * Если поток не успел, main() просто дропает текущий кадр и не блокируется. */
extern atomic_t frame_processing_is_done;

/* Семафор, которым main() будит поток обработки после того, как новый
 * RGB24-кадр полностью скопирован в buf_frame_4_processing. */
extern struct k_sem allowed_2_process;

/* Формат исходного (нативного) кадра с камеры. Заполняется один раз в main()
 * после согласования формата с видеодрайвером. Поток обработки использует его,
 * чтобы сконвертировать обработанный кадр обратно в нативный формат. */
extern struct mpix_format my_fmt;

/* Создаёт и запускает поток обработки изображения. Поток спит на
 * allowed_2_process, пока main() не подготовит очередной RGB24-кадр. */
void processing_thread_init(void);

#endif /* THREAD_PROCESSING_H_ */