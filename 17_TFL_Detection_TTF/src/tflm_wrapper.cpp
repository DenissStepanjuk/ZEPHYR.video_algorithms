#include "tflm_wrapper.h"

#include <cmath>
#include <cstdint>

#include <zephyr/kernel.h>
#include <zephyr/toolchain.h>

#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/micro_log.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/system_setup.h>
#include <tensorflow/lite/schema/schema_generated.h>

#include "model.hpp"            /* g_model - модель, сгенерированная custom_ssd_v3.ipynb */
#include "detector_config.h"    /* DET_ARCH, DET_NUM_ANCHORS, DET_OUT_*_IDX, DET_INPUT_* ... */

/* Защита от рассинхронизации: model.hpp / detector_config.h / tflm_wrapper.h должны быть из одного запуска ноутбука. */
static_assert(DET_NUM_CLASSES == TFLM_NUM_CLASSES, "detector_config.h: число классов != TFLM_NUM_CLASSES");
static_assert(DET_INPUT_W == TFLM_INPUT_W && DET_INPUT_H == TFLM_INPUT_H, "detector_config.h: размер входа != TFLM_INPUT_*");
static_assert(DET_OUT_CLS_IDX != DET_OUT_BOX_IDX, "detector_config.h: индексы выходов совпадают");

namespace {

const tflite::Model *model = nullptr;
tflite::MicroInterpreter *interpreter = nullptr;
TfLiteTensor *input = nullptr;
TfLiteTensor *out_cls = nullptr;   /* SSD: cls_logits [A][NC+1]  | TTF: heatmap-логиты [H][W][NC] */
TfLiteTensor *out_box = nullptr;   /* SSD: box_deltas [A][4]     | TTF: ltrb          [H][W][4]  */
bool ready = false;

/* Арена тензоров. Пик задаёт первая свёртка backbone (вход 3 КБ + выход 16 КБ) плюс удерживаемая карта 8x8x64
 * для FPN. Начинаем с 40 КБ; смотрите лог "arena used" и уменьшайте (допустимый потолок проекта - 100 КБ).
 * Арена лежит во ВНУТРЕННЕЙ SRAM (быстрее PSRAM). */
constexpr int kTensorArenaSize = 31 * 1024;

uint8_t tensor_arena[kTensorArenaSize]
	__attribute__((aligned(16)));

/* LUT квантования входа: uint8 пиксель -> int8. Строится по ФАКТИЧЕСКИМ параметрам входного тензора:
 * q = clamp(round(pix/255/scale) + zero_point). При scale=1/255, zp=-128 это ровно (pix - 128). */
int8_t input_lut[256];

uint32_t last_invoke_us = 0;
uint32_t last_post_us = 0;

/* Наименования классов (порядок = порядок fg_classes в ноутбуке: matrjona, mouse). */
// const char *const kCategoryLabels[TFLM_NUM_CLASSES] = {"Matrjona", "Mouse"};
const char *const kCategoryLabels[TFLM_NUM_CLASSES] = {"Матрёшка", "Мышка"};

#if DET_ARCH == DET_ARCH_SSD
constexpr size_t kClsBytes = (size_t)DET_NUM_ANCHORS * (DET_NUM_CLASSES + 1);
constexpr size_t kBoxBytes = (size_t)DET_NUM_ANCHORS * 4;
#else
constexpr size_t kClsBytes = (size_t)DET_GRID_H * DET_GRID_W * DET_NUM_CLASSES;
constexpr size_t kBoxBytes = (size_t)DET_GRID_H * DET_GRID_W * 4;
#endif

} /* namespace */

extern "C" const char *tflm_wrapper_label(int idx)
{
	if (idx < 0 || idx >= TFLM_NUM_CLASSES) {
		return "?";
	}
	return kCategoryLabels[idx];
}

extern "C" void tflm_wrapper_get_timing(uint32_t *invoke_us, uint32_t *post_us)
{
	if (invoke_us) { *invoke_us = last_invoke_us; }
	if (post_us)   { *post_us = last_post_us; }
}

extern "C" int tflm_wrapper_init(void)
{
	model = tflite::GetModel(g_model);
	if (model->version() != TFLITE_SCHEMA_VERSION) {
		MicroPrintf("Model schema version %d != supported %d",
			    (int)model->version(), TFLITE_SCHEMA_VERSION);
		return -1;
	}

	/* Операции детектора (список печатает ноутбук в разделе 'Экспорт'). */
	static tflite::MicroMutableOpResolver<5> micro_op_resolver;
	if (micro_op_resolver.AddAdd() != kTfLiteOk ||
	    micro_op_resolver.AddConv2D() != kTfLiteOk ||
	    micro_op_resolver.AddDepthwiseConv2D() != kTfLiteOk ||
	    micro_op_resolver.AddMaxPool2D() != kTfLiteOk ||
	    micro_op_resolver.AddResizeNearestNeighbor() != kTfLiteOk) {
		MicroPrintf("Op resolver: failed to register ops");
		return -5;
	}

	static tflite::MicroInterpreter static_interpreter(
		model, micro_op_resolver, tensor_arena, kTensorArenaSize);
	interpreter = &static_interpreter;

	if (interpreter->AllocateTensors() != kTfLiteOk) {
		MicroPrintf("AllocateTensors() failed (arena %d bytes)", kTensorArenaSize);
		return -2;
	}

	if (interpreter->inputs_size() != 1 || interpreter->outputs_size() != 2) {
		MicroPrintf("Bad model IO: %d inputs, %d outputs (expected 1/2)",
			    (int)interpreter->inputs_size(), (int)interpreter->outputs_size());
		return -3;
	}

	input = interpreter->input(0);
	out_cls = interpreter->output(DET_OUT_CLS_IDX);
	out_box = interpreter->output(DET_OUT_BOX_IDX);

	/* Проверки соответствия модели ожиданиям кода. */
	if (input->type != kTfLiteInt8 || input->bytes != (size_t)TFLM_INPUT_SIZE) {
		MicroPrintf("Bad input tensor: type %d, bytes %d", (int)input->type, (int)input->bytes);
		return -3;
	}
	if (out_cls->type != kTfLiteInt8 || out_cls->bytes != kClsBytes ||
	    out_box->type != kTfLiteInt8 || out_box->bytes != kBoxBytes) {
		MicroPrintf("Bad output tensors: cls %d B (exp %d), box %d B (exp %d). Проверьте DET_OUT_*_IDX и model.hpp",
			    (int)out_cls->bytes, (int)kClsBytes, (int)out_box->bytes, (int)kBoxBytes);
		return -4;
	}

	/* detector_config.h должен происходить из того же запуска ноутбука, что и model.hpp. */
	if (input->params.zero_point != DET_INPUT_ZERO_POINT ||
	    std::fabs(input->params.scale - DET_INPUT_SCALE) > 1e-6f) {
		MicroPrintf("WARNING: параметры входа модели (scale=%d/1e6, zp=%d) != detector_config.h - файлы из разных запусков?",
			    (int)(input->params.scale * 1e6f), (int)input->params.zero_point);
	}

	const float s = input->params.scale;
	const int zp = input->params.zero_point;
	for (int p = 0; p < 256; p++) {
		int q = (int)std::lround((float)p / 255.0f / s) + zp;
		input_lut[p] = (int8_t)(q < -128 ? -128 : (q > 127 ? 127 : q));
	}

	MicroPrintf("TFLM detector ready, arena used: %d of %d bytes",
		    (int)interpreter->arena_used_bytes(), kTensorArenaSize);

	ready = true;
	return 0;
}

extern "C" int tflm_wrapper_detect(const uint8_t *rgb, det_t *dets, int max_dets)
{
	if (!ready || rgb == nullptr || dets == nullptr || max_dets <= 0) {
		return -1;
	}

	/* uint8 [0..255] -> int8 по LUT. Порядок каналов R,G,B (как при обучении: вход модели = пиксель/255). */
	int8_t *dst = input->data.int8;
	for (int i = 0; i < TFLM_INPUT_SIZE; i++) {
		dst[i] = input_lut[rgb[i]];
	}

	const uint32_t t0 = k_cycle_get_32();
	if (interpreter->Invoke() != kTfLiteOk) {
		MicroPrintf("Invoke failed");
		return -2;
	}
	const uint32_t t1 = k_cycle_get_32();

	/* Сырые int8-выходы + параметры квантования -> декодирование анкеров/ltrb, порог, NMS (det_postprocess.c). */
	det_tensor_t cls = { out_cls->data.int8, out_cls->params.scale, out_cls->params.zero_point };
	det_tensor_t box = { out_box->data.int8, out_box->params.scale, out_box->params.zero_point };
	const int n = det_postprocess(&cls, &box, dets, max_dets);
	const uint32_t t2 = k_cycle_get_32();

	last_invoke_us = k_cyc_to_us_floor32(t1 - t0);
	last_post_us = k_cyc_to_us_floor32(t2 - t1);
	return n;
}






