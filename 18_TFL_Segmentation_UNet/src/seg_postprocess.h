/*
 * seg_postprocess.h — постобработка сегментации (UNet) для TFLM-int8 модели.
 * Без динамической памяти и без float: пороги заданы в целых числах в seg_config.h
 * (генерируется ноутбуком TFL-Segmentation.ipynb вместе с model.hpp).
 */
#ifndef SEG_POSTPROCESS_H_
#define SEG_POSTPROCESS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * logits    — int8-выход модели, NHWC [SEG_OUT_H][SEG_OUT_W][SEG_NUM_CLASSES] (output(0)->data.int8).
 * mask      — SEG_OUT_H*SEG_OUT_W байт; бит k байта = пиксель принадлежит классу k (классы могут перекрываться).
 * class_px  — необязательно (может быть NULL): число пикселей каждого класса [SEG_NUM_CLASSES].
 * Возвращает число пикселей, отнесённых хотя бы к одному классу.
 */
int seg_postprocess(const int8_t *logits, uint8_t *mask, int *class_px);

/*
 * Накладывает маску на RGB565-кадр SEG_OUT_W x SEG_OUT_H (цвета классов и прозрачность — в seg_config.h).
 */
void seg_overlay_rgb565(const uint8_t *mask, uint16_t *img565);

#ifdef __cplusplus
}
#endif

#endif /* SEG_POSTPROCESS_H_ */
