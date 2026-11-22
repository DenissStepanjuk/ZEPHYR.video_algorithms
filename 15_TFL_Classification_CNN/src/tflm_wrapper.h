#ifndef TFLM_WRAPPER_H
#define TFLM_WRAPPER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Параметры входа/выхода модели (должны совпадать с обучением в TFL-Classification.ipynb).
 * Макросы, а не constexpr: используются в C-коде (размеры массивов в thread_processing.c). */
#define TFLM_INPUT_W       32
#define TFLM_INPUT_H       32
#define TFLM_INPUT_C       3
#define TFLM_INPUT_SIZE    (TFLM_INPUT_W * TFLM_INPUT_H * TFLM_INPUT_C)
#define TFLM_NUM_CLASSES   3

/* Результат классификации. */
struct tflm_result {
	/* Сырой выход модели (int8, -128..127), как в Arduino-проекте. */
	int8_t  raw[TFLM_NUM_CLASSES];
	/* Вероятность каждого класса в процентах, 0..100. */
	uint8_t percent[TFLM_NUM_CLASSES];
	/* Индекс класса с максимальной вероятностью. */
	int     best;
};

/* Наименование класса по индексу ("?" если индекс вне диапазона). */
const char *tflm_wrapper_label(int idx);

/* Инициализация интерпретатора. 0 - успех, иначе отрицательный код.
 * Вызывать один раз, из того же потока, который потом вызывает classify. */
int tflm_wrapper_init(void);

/* Классификация изображения 32x32 RGB24 (порядок R,G,B, значения 0..255).
 * Возвращает 0 при успехе. */
int tflm_wrapper_classify(const uint8_t *rgb, struct tflm_result *res);

/* Время последнего вызова, мкс: чистый Invoke() и постпроцессинг. */
void tflm_wrapper_get_timing(uint32_t *invoke_us, uint32_t *post_us);

#ifdef __cplusplus
}
#endif

#endif /* TFLM_WRAPPER_H */











