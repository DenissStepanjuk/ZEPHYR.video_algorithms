#ifndef TFLM_WRAPPER_H
#define TFLM_WRAPPER_H

#include <stdint.h>

/* Параметры модели (размеры, число классов, цвета, пороги) берутся из seg_config.h, который генерирует
 * TFL-Segmentation.ipynb вместе с model.hpp. Значения здесь НЕ дублируются: рассинхронизация model.hpp и
 * seg_config.h ловится в tflm_wrapper_init() по формам и параметрам квантования тензоров. */
#include "seg_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TFLM_INPUT_W          SEG_INPUT_W
#define TFLM_INPUT_H          SEG_INPUT_H
#define TFLM_INPUT_C          3
#define TFLM_INPUT_SIZE       (TFLM_INPUT_W * TFLM_INPUT_H * TFLM_INPUT_C)
#define TFLM_NUM_CLASSES      SEG_NUM_CLASSES                 /* без фона: Matrjona, Mouse */
#define TFLM_MASK_W           SEG_OUT_W
#define TFLM_MASK_H           SEG_OUT_H
#define TFLM_MASK_SIZE        (TFLM_MASK_W * TFLM_MASK_H)     /* размер маски в байтах: 1 байт на пиксель */

/* Возвращается tflm_wrapper_selftest(), если seg_selftest.h не добавлен в проект. */
#define TFLM_SELFTEST_UNAVAILABLE (-100)

/* Наименование класса по индексу 0..TFLM_NUM_CLASSES-1 ("?" если вне диапазона). */
const char *tflm_wrapper_label(int idx);

/* Инициализация интерпретатора. 0 - успех, иначе отрицательный код:
 *  -1 версия схемы модели, -5 регистрация операций, -2 AllocateTensors (мало арены),
 *  -3 неверные входы/выходы модели, -4 форма/тип выхода не совпадает с seg_config.h,
 *  -6 параметры квантования выхода не совпадают с seg_config.h (пороги были бы неверными).
 * Вызывать один раз, из того же потока, который потом вызывает segment. */
int tflm_wrapper_init(void);

/* Сегментация изображения 32x32 RGB24 (порядок R,G,B, значения 0..255).
 * mask  - TFLM_MASK_SIZE байт (32x32, построчно): бит k байта = пиксель принадлежит классу k
 *         (классы могут перекрываться), 0 = фон.
 * class_px - необязательно (может быть NULL): число пикселей каждого класса [TFLM_NUM_CLASSES].
 * Возвращает число пикселей переднего плана (>=0) либо отрицательный код ошибки. */
int tflm_wrapper_segment(const uint8_t *rgb, uint8_t *mask, int *class_px);

/* Время последнего вызова segment, мкс: чистый Invoke() и постпроцессинг (пороги -> маска). */
void tflm_wrapper_get_timing(uint32_t *invoke_us, uint32_t *post_us);

/* Самопроверка по контрольным кадрам из seg_selftest.h: гонит их через модель и сравнивает маску с ожидаемой
 * из ноутбука. Возвращает суммарное число отличающихся пикселей (>=0; единицы - норма из-за разницы референсных
 * ядер, десятки и больше - ошибка препроцессинга/op-resolver/параметров), отрицательный код при ошибке
 * или TFLM_SELFTEST_UNAVAILABLE. Вызывать после успешного tflm_wrapper_init(). */
int tflm_wrapper_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* TFLM_WRAPPER_H */


