#include "det_postprocess.h"
#include "detector_config.h"

#include <math.h>

#define DET_DELTA_CLIP 2.5f

typedef struct {
	float score;
	int   label;
	int   idx;        /* SSD: индекс anchor; TTF: i*W+j */
} cand_t;

static inline float deq(const det_tensor_t *t, int i)
{
	return ((float)((int)t->data[i] - t->zero_point)) * t->scale;
}

static inline float clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* Вставка в массив, отсортированный по убыванию score; при равенстве — после уже имеющихся (стабильно). */
static void cand_insert(cand_t *c, int *n, int cap, float score, int label, int idx)
{
	int pos = *n;
	if (*n == cap) {
		if (!(score > c[cap - 1].score)) {
			return;
		}
		pos = cap - 1;           /* вытесняем последний */
	} else {
		(*n)++;
	}
	while (pos > 0 && c[pos - 1].score < score) {
		c[pos] = c[pos - 1];
		pos--;
	}
	c[pos].score = score; c[pos].label = label; c[pos].idx = idx;
}

static float box_iou(const det_t *a, const det_t *b)
{
	float iw = fminf(a->x2, b->x2) - fmaxf(a->x1, b->x1);
	float ih = fminf(a->y2, b->y2) - fmaxf(a->y1, b->y1);
	if (iw <= 0.0f || ih <= 0.0f) {
		return 0.0f;
	}
	float inter = iw * ih;
	float aa = fmaxf(a->x2 - a->x1, 0.0f) * fmaxf(a->y2 - a->y1, 0.0f);
	float ab = fmaxf(b->x2 - b->x1, 0.0f) * fmaxf(b->y2 - b->y1, 0.0f);
	float u = aa + ab - inter;
	return inter / (u > 1e-9f ? u : 1e-9f);
}

/* Жадный NMS внутри класса. dets[] уже отсортирован по убыванию score. Результат уплотняется в начале массива. */
static int nms_classwise(det_t *d, int n, float iou_thr, int max_out)
{
	uint8_t sup[DET_PRE_NMS_TOPK] = {0};
	int k = 0;
	for (int i = 0; i < n && k < max_out; i++) {
		if (sup[i]) {
			continue;
		}
		for (int j = i + 1; j < n; j++) {
			if (!sup[j] && d[j].label == d[i].label && box_iou(&d[i], &d[j]) > iou_thr) {
				sup[j] = 1;
			}
		}
		d[k++] = d[i];
	}
	return k;
}

#if DET_ARCH == DET_ARCH_SSD
/* ------------------------------------------ SSD ------------------------------------------ */
int det_postprocess(const det_tensor_t *cls, const det_tensor_t *box, det_t *out, int max_out)
{
	cand_t cand[DET_PRE_NMS_TOPK];
	int nc = 0;
	const int K = DET_NUM_CLASSES + 1;

	for (int a = 0; a < DET_NUM_ANCHORS; a++) {
		float l[DET_NUM_CLASSES + 1], m, s = 0.0f;
		for (int k = 0; k < K; k++) {
			l[k] = deq(cls, a * K + k);
		}
		m = l[0];
		for (int k = 1; k < K; k++) {
			if (l[k] > m) { m = l[k]; }
		}
		for (int k = 0; k < K; k++) {
			l[k] = expf(l[k] - m); s += l[k];
		}
		for (int c = 1; c < K; c++) {
			float p = l[c] / s;
			if (p >= DET_CONF_THR) {
				cand_insert(cand, &nc, DET_PRE_NMS_TOPK, p, c - 1, a);
			}
		}
	}

	static det_t tmp[DET_PRE_NMS_TOPK];
	for (int i = 0; i < nc; i++) {
		const int a = cand[i].idx;
		const float *an = DET_ANCHORS[a];
		float d0 = deq(box, a * 4 + 0), d1 = deq(box, a * 4 + 1), d2 = deq(box, a * 4 + 2), d3 = deq(box, a * 4 + 3);
		float cx = an[0] + d0 * DET_VAR[0] * an[2];
		float cy = an[1] + d1 * DET_VAR[1] * an[3];
		float sw = d2 * DET_VAR[2], sh = d3 * DET_VAR[3];
		sw = sw < -DET_DELTA_CLIP ? -DET_DELTA_CLIP : (sw > DET_DELTA_CLIP ? DET_DELTA_CLIP : sw);
		sh = sh < -DET_DELTA_CLIP ? -DET_DELTA_CLIP : (sh > DET_DELTA_CLIP ? DET_DELTA_CLIP : sh);
		float w = an[2] * expf(sw), h = an[3] * expf(sh);
		tmp[i].x1 = clamp01(cx - 0.5f * w); tmp[i].y1 = clamp01(cy - 0.5f * h);
		tmp[i].x2 = clamp01(cx + 0.5f * w); tmp[i].y2 = clamp01(cy + 0.5f * h);
		tmp[i].score = cand[i].score; tmp[i].label = cand[i].label;
	}

	int n = nms_classwise(tmp, nc, DET_NMS_IOU, max_out < DET_MAX_DET ? max_out : DET_MAX_DET);
	for (int i = 0; i < n; i++) {
		out[i] = tmp[i];
	}
	return n;
}

#elif DET_ARCH == DET_ARCH_TTF
/* ------------------------------------------ TTF ------------------------------------------ */
int det_postprocess(const det_tensor_t *cls, const det_tensor_t *box, det_t *out, int max_out)
{
	cand_t cand[DET_PRE_NMS_TOPK];
	int nc = 0;
	const int H = DET_GRID_H, W = DET_GRID_W, C = DET_NUM_CLASSES;
	/*  порог по логиту: sigmoid(v) >= thr  <=>  v >= ln(thr/(1-thr)) */
	const float thr_logit = logf(DET_CONF_THR / (1.0f - DET_CONF_THR));

	for (int i = 0; i < H; i++) {
		for (int j = 0; j < W; j++) {
			for (int c = 0; c < C; c++) {
				float v = deq(cls, (i * W + j) * C + c);
				if (v < thr_logit) {
					continue;
				}
				int is_peak = 1;
				for (int di = -1; di <= 1 && is_peak; di++) {
					for (int dj = -1; dj <= 1; dj++) {
						int ni = i + di, nj = j + dj;
						if ((di == 0 && dj == 0) || ni < 0 || nj < 0 || ni >= H || nj >= W) {
							continue;
						}
						if (deq(cls, (ni * W + nj) * C + c) > v) { is_peak = 0; break; }
					}
				}
				if (is_peak) {
					cand_insert(cand, &nc, DET_PRE_NMS_TOPK, 1.0f / (1.0f + expf(-v)), c, i * W + j);
				}
			}
		}
	}

	static det_t tmp[DET_PRE_NMS_TOPK];
	for (int k = 0; k < nc; k++) {
		const int i = cand[k].idx / W, j = cand[k].idx % W;
		float l = fmaxf(deq(box, cand[k].idx * 4 + 0), 0.0f), t = fmaxf(deq(box, cand[k].idx * 4 + 1), 0.0f);
		float r = fmaxf(deq(box, cand[k].idx * 4 + 2), 0.0f), b = fmaxf(deq(box, cand[k].idx * 4 + 3), 0.0f);
		float cx = ((float)j + 0.5f) / (float)W, cy = ((float)i + 0.5f) / (float)H;
		tmp[k].x1 = clamp01(cx - l); tmp[k].y1 = clamp01(cy - t);
		tmp[k].x2 = clamp01(cx + r); tmp[k].y2 = clamp01(cy + b);
		tmp[k].score = cand[k].score; tmp[k].label = cand[k].label;
	}

	int n = nms_classwise(tmp, nc, DET_NMS_IOU, max_out < DET_MAX_DET ? max_out : DET_MAX_DET);
	for (int k = 0; k < n; k++) {
		out[k] = tmp[k];
	}
	return n;
}
#else
#error "DET_ARCH must be DET_ARCH_SSD or DET_ARCH_TTF (see detector_config.h)"
#endif
