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

#include "model.hpp"            /* g_model - модель, сгенерированная TFL-Segmentation.ipynb */
#include "seg_postprocess.h"    /* seg_postprocess(): int8-логиты -> битовая маска по целочисленным порогам */

/* Контрольные кадры (необязательно): файл генерирует ноутбук; если его нет в проекте, самопроверка отключается. */
#if defined(__has_include)
#  if __has_include("seg_selftest.h")
#    include "seg_selftest.h"
#    define TFLM_HAVE_SELFTEST 1
#  endif
#endif

/* Бит k байта маски = класс k, поэтому классов не больше 8. */
static_assert(SEG_NUM_CLASSES <= 8, "seg_config.h: маска хранит классы в битах uint8_t (максимум 8)");
static_assert(TFLM_INPUT_SIZE == SEG_INPUT_W * SEG_INPUT_H * 3, "seg_config.h: ожидается 3-канальный вход");

namespace {

const tflite::Model *model = nullptr;
tflite::MicroInterpreter *interpreter = nullptr;
TfLiteTensor *input = nullptr;
TfLiteTensor *out_seg = nullptr;   /* логиты int8 NHWC [1][SEG_OUT_H][SEG_OUT_W][SEG_NUM_CLASSES] */
bool ready = false;

/* Арена тензоров. Нижняя оценка пика активаций UNet с параметрами CFG по умолчанию - около 48 КБ: пик на последнем
 * уровне декодера (32x32), где одновременно живут апсемпл 32x32x16, проекция скипа 32x32x8 и их конкатенация
 * 32x32x24; к этому добавляются служебные структуры и фрагментация TFLM. Стартуем с 80 КБ (цель проекта ~64 КБ,
 * допустимый потолок 150 КБ); смотрите лог "arena used" и уменьшайте до ближайшего рабочего значения.
 * Арена лежит во ВНУТРЕННЕЙ SRAM (быстрее PSRAM). */
constexpr int kTensorArenaSize = 66 * 1024;


#if defined(CONFIG_ESP32_TRUE)
	uint8_t tensor_arena[kTensorArenaSize]
		__attribute__((aligned(16)))
		Z_GENERIC_SECTION(.ext_ram.bss);
#endif /* CONFIG_ESP32_TRUE */
#if defined(CONFIG_STM32_TRUE)
	uint8_t tensor_arena[kTensorArenaSize]
		__attribute__((aligned(16)));
#endif /* CONFIG_STM32_TRUE */

/* LUT квантования входа: uint8 пиксель -> int8. Строится по ФАКТИЧЕСКИМ параметрам входного тензора:
 * q = clamp(round(pix/255/scale) + zero_point). При scale=1/255, zp=-128 это ровно (pix - 128). */
int8_t input_lut[256];

uint32_t last_invoke_us = 0;
uint32_t last_post_us = 0;

/* Наименования классов (порядок = порядок fg_classes в ноутбуке: matrjona, mouse). */
const char *const kCategoryLabels[TFLM_NUM_CLASSES] = {"Матрёшка", "Мышка"};

constexpr size_t kOutBytes = (size_t)SEG_OUT_H * SEG_OUT_W * SEG_NUM_CLASSES;

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

	/* Операции UNet: свёртки backbone/декодера, пулинг, апсемплинг, конкатенация со скипом.
	 * AddAdd нужен только для варианта merge="add" в CFG["unet"] (по умолчанию "concat"), оставлен про запас.
	 * Точный список печатает ноутбук в разделе "Экспорт"; если он отличается - поправьте и размер шаблона. */
	static tflite::MicroMutableOpResolver<6> micro_op_resolver;
	if (micro_op_resolver.AddConv2D() != kTfLiteOk ||
	    micro_op_resolver.AddDepthwiseConv2D() != kTfLiteOk ||
	    micro_op_resolver.AddMaxPool2D() != kTfLiteOk ||
	    micro_op_resolver.AddResizeNearestNeighbor() != kTfLiteOk ||
	    micro_op_resolver.AddConcatenation() != kTfLiteOk ||
	    micro_op_resolver.AddAdd() != kTfLiteOk) {
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

	if (interpreter->inputs_size() != 1 || interpreter->outputs_size() != 1) {
		MicroPrintf("Bad model IO: %d inputs, %d outputs (expected 1/1)",
			    (int)interpreter->inputs_size(), (int)interpreter->outputs_size());
		return -3;
	}

	input = interpreter->input(0);
	out_seg = interpreter->output(0);

	/* Проверки соответствия модели ожиданиям кода. */
	if (input->type != kTfLiteInt8 || input->bytes != (size_t)TFLM_INPUT_SIZE) {
		MicroPrintf("Bad input tensor: type %d, bytes %d", (int)input->type, (int)input->bytes);
		return -3;
	}
	if (out_seg->type != kTfLiteInt8 || out_seg->bytes != kOutBytes ||
	    out_seg->dims->size != 4 || out_seg->dims->data[1] != SEG_OUT_H ||
	    out_seg->dims->data[2] != SEG_OUT_W || out_seg->dims->data[3] != SEG_NUM_CLASSES) {
		MicroPrintf("Bad output tensor: type %d, %d B (exp %d). Проверьте, что model.hpp и seg_config.h из одного запуска ноутбука",
			    (int)out_seg->type, (int)out_seg->bytes, (int)kOutBytes);
		return -4;
	}

	/* Целочисленные пороги SEG_Q_THR посчитаны ноутбуком именно для этих scale/zero_point выхода. Если model.hpp и
	 * seg_config.h из разных запусков, маска будет неверной без каких-либо других признаков - поэтому это ошибка. */
	if (out_seg->params.zero_point != SEG_OUT_ZERO_POINT ||
	    std::fabs(out_seg->params.scale - SEG_OUT_SCALE) > 1e-5f * SEG_OUT_SCALE) {
		MicroPrintf("Output quant mismatch: model scale=%d/1e9 zp=%d, seg_config.h scale=%d/1e9 zp=%d - файлы из разных запусков?",
			    (int)(out_seg->params.scale * 1e9f), (int)out_seg->params.zero_point,
			    (int)(SEG_OUT_SCALE * 1e9f), (int)SEG_OUT_ZERO_POINT);
		return -6;
	}

	/* Параметры входа не критичны (LUT строится по фактическим), но расхождение говорит о файлах из разных запусков. */
	if (input->params.zero_point != SEG_INPUT_ZERO_POINT ||
	    std::fabs(input->params.scale - SEG_INPUT_SCALE) > 1e-6f) {
		MicroPrintf("WARNING: параметры входа модели (scale=%d/1e6, zp=%d) != seg_config.h - файлы из разных запусков?",
			    (int)(input->params.scale * 1e6f), (int)input->params.zero_point);
	}

	const float s = input->params.scale;
	const int zp = input->params.zero_point;
	for (int p = 0; p < 256; p++) {
		int q = (int)std::lround((float)p / 255.0f / s) + zp;
		input_lut[p] = (int8_t)(q < -128 ? -128 : (q > 127 ? 127 : q));
	}

	MicroPrintf("TFLM segmentation ready, arena used: %d of %d bytes",
		    (int)interpreter->arena_used_bytes(), kTensorArenaSize);

	ready = true;
	return 0;
}

extern "C" int tflm_wrapper_segment(const uint8_t *rgb, uint8_t *mask, int *class_px)
{
	if (!ready || rgb == nullptr || mask == nullptr) {
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

	/* Сырые int8-логиты -> битовая маска: одно целочисленное сравнение на пиксель и класс (без float). */
	const int n = seg_postprocess(out_seg->data.int8, mask, class_px);
	const uint32_t t2 = k_cycle_get_32();

	last_invoke_us = k_cyc_to_us_floor32(t1 - t0);
	last_post_us = k_cyc_to_us_floor32(t2 - t1);
	return n;
}

extern "C" int tflm_wrapper_selftest(void)
{
#ifdef TFLM_HAVE_SELFTEST
	if (!ready) {
		return -1;
	}

	static uint8_t st_mask[TFLM_MASK_SIZE];
	int total_diff = 0;

	for (int i = 0; i < SEG_SELFTEST_N; i++) {
		/* Вход в seg_selftest.h уже квантован ноутбуком (int8), LUT не нужен. */
		for (int j = 0; j < TFLM_INPUT_SIZE; j++) {
			input->data.int8[j] = SEG_SELFTEST_IN[i][j];
		}
		if (interpreter->Invoke() != kTfLiteOk) {
			MicroPrintf("selftest #%d: Invoke failed", i);
			return -2;
		}
		seg_postprocess(out_seg->data.int8, st_mask, nullptr);

		int diff = 0;
		for (int p = 0; p < TFLM_MASK_SIZE; p++) {
			diff += (st_mask[p] != SEG_SELFTEST_MASK[i][p]) ? 1 : 0;
		}
		MicroPrintf("selftest #%d: %d of %d px differ from notebook", i, diff, TFLM_MASK_SIZE);
		total_diff += diff;
	}
	return total_diff;
#else
	return TFLM_SELFTEST_UNAVAILABLE;
#endif
}



