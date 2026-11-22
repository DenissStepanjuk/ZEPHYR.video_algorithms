/*
 * det_postprocess.h - постпроцессинг детектора (SSD / TTF) для TFLM-int8 моделей.
 * Без динамической памяти, без libm кроме expf/logf. Конфигурация — в detector_config.h
 * (генерируется ноутбуком TFL-Detection.ipynb вместе с model.hpp).
 */
#ifndef DET_POSTPROCESS_H_
#define DET_POSTPROCESS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Результат детекции: координаты нормированы в [0,1] относительно входного кадра (x вправо, y вниз). */
typedef struct {
	float x1, y1, x2, y2;
	float score;
	int   label;      /* 0-based индекс переднего плана: DET_LABELS[label] */
} det_t;

/* int8-тензор модели вместе с параметрами квантования (output->data.int8, params.scale, params.zero_point). */
typedef struct {
	const int8_t *data;
	float         scale;
	int           zero_point;
} det_tensor_t;

/*
 * SSD: cls = логиты [DET_NUM_ANCHORS][DET_NUM_CLASSES+1] (индекс 0 - фон), box = дельты [DET_NUM_ANCHORS][4].
 * TTF: cls = heatmap-логиты [H][W][DET_NUM_CLASSES],       box = ltrb      [H][W][4] (доли кадра, >=0).
 * Возвращает число детекций (<= max_out), отсортированных по убыванию score.
 */
int det_postprocess(const det_tensor_t *cls, const det_tensor_t *box, det_t *out, int max_out);

#ifdef __cplusplus
}
#endif

#endif /* DET_POSTPROCESS_H_ */
