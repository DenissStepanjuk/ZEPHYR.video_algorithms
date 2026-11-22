#ifndef TFLM_WRAPPER_H
#define TFLM_WRAPPER_H

#include <stdint.h>

/* det_t (бокс + score + класс) и API постпроцессинга. Этот заголовок НЕ тянет detector_config.h,
 * поэтому безопасен для включения из нескольких .c/.cpp (таблица anchors не дублируется). */
#include "det_postprocess.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Параметры входа модели (должны совпадать с custom_ssd_v3.ipynb; согласованность с detector_config.h
 * проверяется static_assert'ами в tflm_wrapper.cpp). Макросы, а не constexpr: используются в C-коде. */
#define TFLM_INPUT_W          32
#define TFLM_INPUT_H          32
#define TFLM_INPUT_C          3
#define TFLM_INPUT_SIZE       (TFLM_INPUT_W * TFLM_INPUT_H * TFLM_INPUT_C)
#define TFLM_NUM_CLASSES      2     /* без фона: Matrjona, Mouse */
#define TFLM_MAX_DETECTIONS   10    /* размер массива det_t в вызывающем коде */

/* Наименование класса по индексу переднего плана 0..TFLM_NUM_CLASSES-1 ("?" если вне диапазона). */
const char *tflm_wrapper_label(int idx);

/* Инициализация интерпретатора. 0 - успех, иначе отрицательный код.
 * Вызывать один раз, из того же потока, который потом вызывает detect. */
int tflm_wrapper_init(void);

/* Детекция на изображении 32x32 RGB24 (порядок R,G,B, значения 0..255).
 * Заполняет dets[] (до max_dets штук, по убыванию score), координаты в долях кадра [0..1].
 * Возвращает число детекций (>=0) либо отрицательный код ошибки. */
int tflm_wrapper_detect(const uint8_t *rgb, det_t *dets, int max_dets);

/* Время последнего вызова detect, мкс: чистый Invoke() и постпроцессинг (decode + NMS). */
void tflm_wrapper_get_timing(uint32_t *invoke_us, uint32_t *post_us);

#ifdef __cplusplus
}
#endif

#endif /* TFLM_WRAPPER_H */



















// #ifndef TFLM_WRAPPER_H
// #define TFLM_WRAPPER_H

// #include <stdint.h>

// #ifdef __cplusplus
// extern "C" {
// #endif

// /* Параметры входа/выхода модели (должны совпадать с обучением в TFL-Classification.ipynb).
//  * Макросы, а не constexpr: используются в C-коде (размеры массивов в thread_processing.c). */
// #define TFLM_INPUT_W       32
// #define TFLM_INPUT_H       32
// #define TFLM_INPUT_C       3
// #define TFLM_INPUT_SIZE    (TFLM_INPUT_W * TFLM_INPUT_H * TFLM_INPUT_C)
// #define TFLM_NUM_CLASSES   3

// /* Результат классификации. */
// struct tflm_result {
// 	/* Сырой выход модели (int8, -128..127), как в Arduino-проекте. */
// 	int8_t  raw[TFLM_NUM_CLASSES];
// 	/* Вероятность каждого класса в процентах, 0..100. */
// 	uint8_t percent[TFLM_NUM_CLASSES];
// 	/* Индекс класса с максимальной вероятностью. */
// 	int     best;
// };

// /* Наименование класса по индексу ("?" если индекс вне диапазона). */
// const char *tflm_wrapper_label(int idx);

// /* Инициализация интерпретатора. 0 - успех, иначе отрицательный код.
//  * Вызывать один раз, из того же потока, который потом вызывает classify. */
// int tflm_wrapper_init(void);

// /* Классификация изображения 32x32 RGB24 (порядок R,G,B, значения 0..255).
//  * Возвращает 0 при успехе. */
// int tflm_wrapper_classify(const uint8_t *rgb, struct tflm_result *res);

// #ifdef __cplusplus
// }
// #endif

// #endif /* TFLM_WRAPPER_H */
