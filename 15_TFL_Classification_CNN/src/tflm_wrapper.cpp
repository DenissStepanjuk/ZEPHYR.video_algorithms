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

#include "model.hpp"

namespace {

const tflite::Model *model = nullptr;
tflite::MicroInterpreter *interpreter = nullptr;
TfLiteTensor *input = nullptr;
TfLiteTensor *output = nullptr;
bool ready = false;

/* Арена тензоров. Новая компактная модель требует единицы десятков КБ, поэтому арена лежит
 * во ВНУТРЕННЕЙ SRAM (быстрее PSRAM в разы). Начальный размер - из вывода ноутбука
 * ("Рекомендуемый kTensorArenaSize"); реальное значение смотрите в логе "arena used". */
constexpr int kTensorArenaSize = 26 * 1024;
// constexpr int kTensorArenaSize = 52 * 1024;


uint8_t tensor_arena[kTensorArenaSize] 
	__attribute__((aligned(16)));
    // Z_GENERIC_SECTION(.ext_ram.bss);


uint32_t last_invoke_us = 0;
uint32_t last_post_us = 0;

/* Наименования категорий (порядок = os.listdir(DATASET_PATH) при обучении). */
// const char *const kCategoryLabels[TFLM_NUM_CLASSES] = {"Background","Matrjona","Mouse"};
const char *const kCategoryLabels[TFLM_NUM_CLASSES] = {"Фон", "Матрёшка", "Мышка"};

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

	/* Операции новой модели (список печатает ноутбук в разделе 'Экспорт'). */
	static tflite::MicroMutableOpResolver<7> micro_op_resolver;
	micro_op_resolver.AddAveragePool2D();
	micro_op_resolver.AddConv2D();
	micro_op_resolver.AddDepthwiseConv2D();
	micro_op_resolver.AddFullyConnected();
	micro_op_resolver.AddMaxPool2D();
	micro_op_resolver.AddReshape();
	micro_op_resolver.AddSoftmax();

	static tflite::MicroInterpreter static_interpreter(
		model, micro_op_resolver, tensor_arena, kTensorArenaSize);
	interpreter = &static_interpreter;

	if (interpreter->AllocateTensors() != kTfLiteOk) {
		MicroPrintf("AllocateTensors() failed (arena %d bytes)", kTensorArenaSize);
		return -2;
	}

	input = interpreter->input(0);
	output = interpreter->output(0);

	/* Проверки соответствия модели ожиданиям кода. */
	if (input->type != kTfLiteInt8 || input->bytes != (size_t)TFLM_INPUT_SIZE) {
		MicroPrintf("Bad input tensor: type %d, bytes %d",
			    (int)input->type, (int)input->bytes);
		return -3;
	}
	if (output->type != kTfLiteInt8 || output->bytes != (size_t)TFLM_NUM_CLASSES) {
		MicroPrintf("Bad output tensor: type %d, bytes %d",
			    (int)output->type, (int)output->bytes);
		return -4;
	}

	/* Нормировка при обучении: x/255, значит ожидаем scale=1/255 и zero_point=-128,
	 * т.е. q = пиксель - 128. Если это не так - преобразование ниже надо менять. */
	if (input->params.zero_point != -128 ||
	    std::fabs(input->params.scale - (1.0f / 255.0f)) > 1e-4f) {
		MicroPrintf("WARNING: unexpected input quantization, zero_point=%d",
			    (int)input->params.zero_point);
	}

	MicroPrintf("TFLM ready, arena used: %d bytes",
		    (int)interpreter->arena_used_bytes());

	ready = true;
	return 0;
}

extern "C" int tflm_wrapper_classify(const uint8_t *rgb, struct tflm_result *res)
{
	if (!ready || rgb == nullptr || res == nullptr) {
		return -1;
	}

	/* uint8 [0..255] -> int8 [-128..127]. Порядок каналов R,G,B (как при обучении). */
	int8_t *dst = input->data.int8;
	for (int i = 0; i < TFLM_INPUT_SIZE; i++) {
		dst[i] = (int8_t)((int)rgb[i] - 128);
	}


	const uint32_t t0 = k_cycle_get_32();

	if (interpreter->Invoke() != kTfLiteOk) {
		MicroPrintf("Invoke failed");
		return -2;
	}

	const uint32_t t1 = k_cycle_get_32();

	const float scale = output->params.scale;
	const int zp = output->params.zero_point;
	int best = 0;

	for (int i = 0; i < TFLM_NUM_CLASSES; i++) {
		int8_t q = output->data.int8[i];
		float p = ((int)q - zp) * scale * 100.0f;

		if (p < 0.0f) {
			p = 0.0f;
		}
		if (p > 100.0f) {
			p = 100.0f;
		}
		res->raw[i] = q;
		res->percent[i] = (uint8_t)std::lround(p);
		if (q > output->data.int8[best]) {
			best = i;
		}
	}
	res->best = best;

	const uint32_t t2 = k_cycle_get_32();

	last_invoke_us = k_cyc_to_us_floor32(t1 - t0);
	last_post_us = k_cyc_to_us_floor32(t2 - t1);

	return 0;
}


