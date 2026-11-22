/* автогенерация TFL-Segmentation.ipynb — не править вручную  */
#ifndef SEG_CONFIG_H_
#define SEG_CONFIG_H_

#include <stdint.h>

#define SEG_NUM_CLASSES 2
#define SEG_INPUT_W 32
#define SEG_INPUT_H 32
#define SEG_INPUT_SCALE 0.00392156886f
#define SEG_INPUT_ZERO_POINT -128
/* квантование входа: q = clamp(round(pix/255/SEG_INPUT_SCALE) + SEG_INPUT_ZERO_POINT, -128, 127) */

#define SEG_OUT_W 32
#define SEG_OUT_H 32
/* параметры выходного тензора: на плате сверить с output(0)->params (пороги ниже посчитаны именно для них) */
#define SEG_OUT_SCALE 0.124620594f
#define SEG_OUT_ZERO_POINT 33

/* пиксель принадлежит классу c, если int8-логит >= SEG_Q_THR[c] (128 = никогда, -128 = всегда).
   Пороги вероятности: matrjona=0.60, mouse=0.40 */
static const int SEG_Q_THR[SEG_NUM_CLASSES] = {37, 30};

static const char *const SEG_LABELS[SEG_NUM_CLASSES] = {"matrjona", "mouse"};
static const uint8_t SEG_COLORS[SEG_NUM_CLASSES][3] = {{255, 0, 0}, {0, 120, 255}};
#define SEG_OVERLAY_ALPHA 128      /* прозрачность наложения 0..256 */
#define SEG_RGB565_BYTESWAP 0      /* 1, если дисплей ожидает RGB565 со сменой порядка байт  */

#endif /* SEG_CONFIG_H_ */
