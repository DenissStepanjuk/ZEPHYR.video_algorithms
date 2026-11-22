/* автогенерация TFL-Detection.ipynb - не править вручную */
#ifndef DETECTOR_CONFIG_H_
#define DETECTOR_CONFIG_H_

#define DET_ARCH_SSD 1
#define DET_ARCH_TTF 2
#define DET_ARCH DET_ARCH_TTF

#define DET_NUM_CLASSES 2
#define DET_INPUT_W 32
#define DET_INPUT_H 32
#define DET_INPUT_SCALE 0.00392156886f
#define DET_INPUT_ZERO_POINT -128
/*  квантование входа: q = clamp(round(pix/255/DET_INPUT_SCALE) + DET_INPUT_ZERO_POINT, -128, 127) */

/* индексы выходных тензоров интерпретатора: interpreter->output(i) */
#define DET_OUT_CLS_IDX 1
#define DET_OUT_BOX_IDX 0

#define DET_CONF_THR 0.4000f
#define DET_NMS_IOU 0.4500f
#define DET_PRE_NMS_TOPK 64
#define DET_MAX_DET 10

static const char *const DET_LABELS[DET_NUM_CLASSES] = {"matrjona", "mouse"};

#define DET_GRID_H 8
#define DET_GRID_W 8

#endif /* DETECTOR_CONFIG_H_ */
