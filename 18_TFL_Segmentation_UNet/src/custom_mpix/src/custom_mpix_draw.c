#include "custom_mpix_draw.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

/* -------------------------------------------------------------------- */
/* Внутренние вспомогательные функции                                    */
/* -------------------------------------------------------------------- */

static inline bool canvas_is_valid(const struct custom_mpix_canvas *canvas)
{
	return canvas != NULL && canvas->buf != NULL &&
	       canvas->width > 0 && canvas->height > 0;
}

static inline bool point_in_bounds(const struct custom_mpix_canvas *canvas, int32_t x, int32_t y)
{
	return x >= 0 && y >= 0 && x < (int32_t)canvas->width && y < (int32_t)canvas->height;
}

static inline uint8_t *pixel_ptr(const struct custom_mpix_canvas *canvas, int32_t x, int32_t y)
{
	return canvas->buf + (size_t)y * canvas->stride + (size_t)x * 3u;
}

static inline int32_t abs32(int32_t v)
{
	return v < 0 ? -v : v;
}

static inline int32_t min32(int32_t a, int32_t b)
{
	return a < b ? a : b;
}

static inline int32_t max32(int32_t a, int32_t b)
{
	return a > b ? a : b;
}

/**
 * @brief Безопасный верхний предел радиуса окружности (шаг 8, hardening).
 *
 * При radius > ~46340 выражение dy*dy в int32_t арифметике переполняется
 * (UB) ещё до приведения к uint64_t, а также сам диапазон цикла for
 * (-radius..radius) становится непрактично большим. Ни один реальный
 * embedded-дисплей не нуждается в окружности радиусом больше десятков
 * тысяч пикселей, поэтому вместо UB тихо клипуем radius к этому пределу —
 * визуально результат не отличается (окружность всё равно почти
 * целиком выходит за пределы любого реального канваса).
 */
#define CUSTOM_MPIX_DRAW_MAX_RADIUS 30000

static inline int32_t clamp_radius(int32_t radius)
{
	return (radius > CUSTOM_MPIX_DRAW_MAX_RADIUS) ? CUSTOM_MPIX_DRAW_MAX_RADIUS : radius;
}

static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
	if (v < lo) {
		return lo;
	}
	if (v > hi) {
		return hi;
	}
	return v;
}

/**
 * @brief Разумный предел координат/размеров (шаг 8, hardening).
 *
 * Ни один реальный embedded-канвас не превышает десятков тысяч пикселей
 * по стороне. Ограничение на 2 порядка выше (1e6) — достаточный запас,
 * чтобы не задеть ни один настоящий сценарий использования, но при этом
 * не дать x+w / y+h переполнить int32_t (UB) на патологических входных
 * данных (например, случайно переданных сверхбольших значениях).
 */
#define CUSTOM_MPIX_DRAW_COORD_LIMIT 1000000

static inline int clz64_portable(uint64_t n)
{
#if defined(__GNUC__) || defined(__clang__)
	/* На всех целевых тулчейнах (Zephyr SDK для ARM/Xtensa/RISC-V —
	 * все на базе GCC или LLVM) это как минимум быстрее ручного цикла,
	 * а на Cortex-M3+ компилируется в одну инструкцию CLZ. */
	return __builtin_clzll(n);
#else
	/* Портируемый fallback на случай экзотического тулчейна без
	 * __builtin_clzll — линейный поиск старшего бита. */
	int count = 0;

	for (uint64_t bit = (uint64_t)1 << 63; bit != 0; bit >>= 1) {
		if (n & bit) {
			break;
		}
		count++;
	}
	return count;
#endif
}

/**
 * @brief Целочисленный квадратный корень (floor(sqrt(n))), без float и без
 *        деления — классический побитовый алгоритм. Нужен для окружностей,
 *        чтобы не тянуть libm/FPU на MCU вроде STM32 без аппаратного FPU.
 *
 * Оптимизация (шаг 8): стартовый бит вычисляется через clz64_portable()
 * (на Cortex-M3+ компилируется в одну инструкцию CLZ) вместо линейного
 * "спуска" от бита 1<<62. Для типичных embedded-радиусов (единицы-сотни
 * пикселей) это устраняет порядка 25-30 холостых итераций на каждый
 * вызов, которые раньше тратились только на поиск стартовой позиции.
 */
static uint32_t isqrt64(uint64_t n)
{
	if (n == 0) {
		return 0;
	}

	int msb = 63 - clz64_portable(n);
	int shift = msb & ~1; /* округляем вниз до чётной степени (степени 4). */
	uint64_t bit = (uint64_t)1 << shift;
	uint64_t res = 0;

	while (bit != 0) {
		if (n >= res + bit) {
			n -= res + bit;
			res = (res >> 1) + bit;
		} else {
			res >>= 1;
		}
		bit >>= 2;
	}

	return (uint32_t)res;
}

/**
 * @brief Закрасить один пиксель-ряд длиной count пикселей, начиная с dst,
 *        одним цветом. Вместо count скалярных записей по 3 байта — пишем
 *        первый пиксель, а дальше УДВАИВАЕМ уже записанный префикс через
 *        memcpy (1->2->4->8... пикселей за вызов) до нужной длины.
 *
 * Итог: O(log count) вызовов memcpy вместо O(count) скалярных записей.
 * memcpy на любом embedded-тулчейне как минимум развёрнут в пословные
 * копирования (а часто использует более широкие инструкции/DMA-подобные
 * пути), что на MCU без аппаратного блиттера/2D-DMA заметно быстрее
 * покомпонентной записи r/g/b по отдельности в цикле.
 */
static void fill_row_bytes(uint8_t *dst, int32_t count, struct custom_mpix_rgb24 color)
{
	if (count <= 0) {
		return;
	}

	dst[0] = color.r;
	dst[1] = color.g;
	dst[2] = color.b;

	size_t have = 3;                 /* байт уже гарантированно записано */
	size_t need = (size_t)count * 3; /* сколько байт нужно всего */

	while (have < need) {
		size_t chunk = have;

		if (chunk > need - have) {
			chunk = need - have;
		}
		memcpy(dst + have, dst, chunk);
		have += chunk;
	}
}

/**
 * @brief Закрасить прямоугольную область [x, x+w) x [y, y+h), обрезая её
 *        по границам канваса. Используется как общий "движок" заливки для
 *        hline/vline/rect_fill/circle*(через per-row вызовы).
 *
 * Оптимизация (шаг 8): первая видимая строка заполняется через
 * fill_row_bytes() (удвоение + memcpy), а все последующие строки —
 * прямым memcpy уже готовой первой строки. Итого O(H) вызовов memcpy
 * вместо O(W*H) скалярных попиксельных записей.
 *
 * Вызывающая сторона отвечает за проверку canvas/thickness на валидность —
 * эта функция только клипует геометрию и всегда успешна.
 */
static void fill_rect_clipped(struct custom_mpix_canvas *canvas, int32_t x, int32_t y,
			       int32_t w, int32_t h, struct custom_mpix_rgb24 color)
{
	/* Защита от переполнения int32_t в x+w/y+h на патологических входных
	 * данных — см. CUSTOM_MPIX_DRAW_COORD_LIMIT выше. На любые реальные вызовы
	 * (координаты и размеры в пределах канваса) это не влияет. */
	x = clampi(x, -CUSTOM_MPIX_DRAW_COORD_LIMIT, CUSTOM_MPIX_DRAW_COORD_LIMIT);
	y = clampi(y, -CUSTOM_MPIX_DRAW_COORD_LIMIT, CUSTOM_MPIX_DRAW_COORD_LIMIT);
	w = clampi(w, 0, CUSTOM_MPIX_DRAW_COORD_LIMIT);
	h = clampi(h, 0, CUSTOM_MPIX_DRAW_COORD_LIMIT);

	int32_t x0 = max32(x, 0);
	int32_t y0 = max32(y, 0);
	int32_t x1 = min32(x + w, (int32_t)canvas->width);
	int32_t y1 = min32(y + h, (int32_t)canvas->height);

	if (x1 <= x0 || y1 <= y0) {
		/* Прямоугольник полностью вне канваса — рисовать нечего. */
		return;
	}

	uint8_t *first_row = pixel_ptr(canvas, x0, y0);
	size_t row_bytes = (size_t)(x1 - x0) * 3u;

	fill_row_bytes(first_row, x1 - x0, color);

	for (int32_t row = y0 + 1; row < y1; row++) {
		memcpy(pixel_ptr(canvas, x0, row), first_row, row_bytes);
	}
}

/* -------------------------------------------------------------------- */
/* Инициализация канваса                                                 */
/* -------------------------------------------------------------------- */

int custom_mpix_canvas_from_buf(struct custom_mpix_canvas *canvas, uint8_t *buf, uint16_t width, uint16_t height)
{
	if (canvas == NULL || buf == NULL || width == 0 || height == 0) {
		return -EINVAL;
	}

	canvas->buf = buf;
	canvas->width = width;
	canvas->height = height;
	canvas->stride = (size_t)width * 3u;

	return 0;
}

/* -------------------------------------------------------------------- */
/* Пиксельный уровень                                                    */
/* -------------------------------------------------------------------- */

int custom_mpix_draw_set_pixel(struct custom_mpix_canvas *canvas, int32_t x, int32_t y, struct custom_mpix_rgb24 color)
{
	if (!canvas_is_valid(canvas)) {
		return -EINVAL;
	}

	if (!point_in_bounds(canvas, x, y)) {
		/* Выход за границы канваса — не ошибка, просто нечего рисовать. */
		return 0;
	}

	uint8_t *p = pixel_ptr(canvas, x, y);

	p[0] = color.r;
	p[1] = color.g;
	p[2] = color.b;

	return 0;
}

int custom_mpix_draw_get_pixel(const struct custom_mpix_canvas *canvas, int32_t x, int32_t y, struct custom_mpix_rgb24 *color)
{
	if (!canvas_is_valid(canvas) || color == NULL) {
		return -EINVAL;
	}

	if (!point_in_bounds(canvas, x, y)) {
		return -ERANGE;
	}

	const uint8_t *p = pixel_ptr(canvas, x, y);

	color->r = p[0];
	color->g = p[1];
	color->b = p[2];

	return 0;
}

/* -------------------------------------------------------------------- */
/* Линии                                                                 */
/* -------------------------------------------------------------------- */

int custom_mpix_draw_hline(struct custom_mpix_canvas *canvas, int32_t x, int32_t y,
		     int32_t length, uint16_t thickness, struct custom_mpix_rgb24 color)
{
	if (!canvas_is_valid(canvas) || thickness == 0) {
		return -EINVAL;
	}

	if (length <= 0) {
		return 0;
	}

	fill_rect_clipped(canvas, x, y, length, (int32_t)thickness, color);

	return 0;
}

int custom_mpix_draw_vline(struct custom_mpix_canvas *canvas, int32_t x, int32_t y,
		     int32_t length, uint16_t thickness, struct custom_mpix_rgb24 color)
{
	if (!canvas_is_valid(canvas) || thickness == 0) {
		return -EINVAL;
	}

	if (length <= 0) {
		return 0;
	}

	fill_rect_clipped(canvas, x, y, (int32_t)thickness, length, color);

	return 0;
}

int custom_mpix_draw_line(struct custom_mpix_canvas *canvas, int32_t x0, int32_t y0,
		    int32_t x1, int32_t y1, uint16_t thickness, struct custom_mpix_rgb24 color)
{
	if (!canvas_is_valid(canvas) || thickness == 0) {
		return -EINVAL;
	}

	/* Классический целочисленный алгоритм Брезенхэма (без float, важно для MCU). */
	int32_t dx = abs32(x1 - x0);
	int32_t sx = (x0 < x1) ? 1 : -1;
	int32_t dy = -abs32(y1 - y0);
	int32_t sy = (y0 < y1) ? 1 : -1;
	int32_t err = dx + dy;
	int32_t half = (int32_t)thickness / 2;

	while (1) {
		if (thickness == 1) {
			/* Быстрый путь для самого частого случая — линия в 1px. */
			(void)custom_mpix_draw_set_pixel(canvas, x0, y0, color);
		} else {
			/* Толщина эмулируется закрашенным квадратом thickness x thickness
			 * в каждой точке пути — просто и достаточно для встраиваемых
			 * систем, без вычисления перпендикулярных смещений. */
			fill_rect_clipped(canvas, x0 - half, y0 - half,
					   (int32_t)thickness, (int32_t)thickness, color);
		}

		if (x0 == x1 && y0 == y1) {
			break;
		}

		int32_t e2 = 2 * err;

		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}

	return 0;
}

/* -------------------------------------------------------------------- */
/* Прямоугольники                                                        */
/* -------------------------------------------------------------------- */

int custom_mpix_draw_rect(struct custom_mpix_canvas *canvas, int32_t x, int32_t y,
		    int32_t w, int32_t h, uint16_t thickness, struct custom_mpix_rgb24 color)
{
	if (!canvas_is_valid(canvas) || w <= 0 || h <= 0 || thickness == 0) {
		return -EINVAL;
	}

	int32_t t = (int32_t)thickness;

	/* Контур рисуется как 4 закрашенные полосы, растущие ВНУТРЬ фигуры,
	 * так что внешняя граница полос точно совпадает с (x, y)-(x+w, y+h).
	 * Если толщина больше половины ширины/высоты — полосы просто
	 * перекрываются, вырождаясь в полностью закрашенный прямоугольник.
	 * Это не ошибка, а естественный предельный случай. */

	/* Верхняя полоса. */
	fill_rect_clipped(canvas, x, y, w, t, color);
	/* Нижняя полоса. */
	fill_rect_clipped(canvas, x, y + h - t, w, t, color);
	/* Левая полоса. */
	fill_rect_clipped(canvas, x, y, t, h, color);
	/* Правая полоса. */
	fill_rect_clipped(canvas, x + w - t, y, t, h, color);

	return 0;
}

int custom_mpix_draw_rect_fill(struct custom_mpix_canvas *canvas, int32_t x, int32_t y,
			 int32_t w, int32_t h, struct custom_mpix_rgb24 color)
{
	if (!canvas_is_valid(canvas) || w <= 0 || h <= 0) {
		return -EINVAL;
	}

	fill_rect_clipped(canvas, x, y, w, h, color);

	return 0;
}

/* -------------------------------------------------------------------- */
/* Окружности                                                            */
/* -------------------------------------------------------------------- */

int custom_mpix_draw_circle_fill(struct custom_mpix_canvas *canvas, int32_t cx, int32_t cy,
			   int32_t radius, struct custom_mpix_rgb24 color)
{
	if (!canvas_is_valid(canvas) || radius <= 0) {
		return -EINVAL;
	}

	radius = clamp_radius(radius);

	/* Для каждой строки dy в пределах окружности считаем ширину хорды
	 * через целочисленный корень и закрашиваем её одним вызовом —
	 * fill_rect_clipped сам обрежет строку по границам канваса. Строки,
	 * целиком выходящие за верх/низ канваса, тоже безопасно отсекутся. */
	for (int32_t dy = -radius; dy <= radius; dy++) {
		/* dy приводим к int64_t ДО умножения — иначе dy*dy считается
		 * в int32_t и переполняется (UB) при большом radius, ещё до
		 * приведения результата к uint64_t. */
		uint64_t under_sqrt = (uint64_t)radius * (uint64_t)radius -
				      (uint64_t)((int64_t)dy * (int64_t)dy);
		int32_t dx = (int32_t)isqrt64(under_sqrt);

		fill_rect_clipped(canvas, cx - dx, cy + dy, 2 * dx + 1, 1, color);
	}

	return 0;
}

int custom_mpix_draw_circle(struct custom_mpix_canvas *canvas, int32_t cx, int32_t cy,
		      int32_t radius, uint16_t thickness, struct custom_mpix_rgb24 color)
{
	if (!canvas_is_valid(canvas) || radius <= 0 || thickness == 0) {
		return -EINVAL;
	}

	radius = clamp_radius(radius);

	int32_t inner_r = radius - (int32_t)thickness;

	if (inner_r <= 0) {
		/* Толщина контура >= радиуса — контур вырождается в сплошной
		 * круг, ровно как аналогичный предельный случай у custom_mpix_draw_rect. */
		return custom_mpix_draw_circle_fill(canvas, cx, cy, radius, color);
	}

	for (int32_t dy = -radius; dy <= radius; dy++) {
		uint64_t outer_sq = (uint64_t)radius * (uint64_t)radius -
				     (uint64_t)((int64_t)dy * (int64_t)dy);
		int32_t outer_dx = (int32_t)isqrt64(outer_sq);

		if (abs32(dy) > inner_r) {
			/* Эта строка выше/ниже внутренней окружности — здесь
			 * кольцо сплошное на всю ширину внешней хорды ("шапка"). */
			fill_rect_clipped(canvas, cx - outer_dx, cy + dy, 2 * outer_dx + 1, 1, color);
			continue;
		}

		uint64_t inner_sq = (uint64_t)inner_r * (uint64_t)inner_r -
				     (uint64_t)((int64_t)dy * (int64_t)dy);
		int32_t inner_dx = (int32_t)isqrt64(inner_sq);
		int32_t seg_w = outer_dx - inner_dx;

		if (seg_w <= 0) {
			continue;
		}

		/* Левый и правый сегмент кольца в этой строке. */
		fill_rect_clipped(canvas, cx - outer_dx, cy + dy, seg_w, 1, color);
		fill_rect_clipped(canvas, cx + inner_dx + 1, cy + dy, seg_w, 1, color);
	}

	return 0;
}

/* -------------------------------------------------------------------- */
/* Текст                                                                 */
/* -------------------------------------------------------------------- */

/**
 * @brief Найти глиф для кодовой точки Unicode code в шрифте font.
 *
 * @return Указатель на глиф, либо NULL, если code вне диапазона
 *         [font->first_char, font->last_char].
 */
static const struct custom_mpix_glyph *find_glyph(const struct custom_mpix_font *font, uint32_t code)
{
	if (code < font->first_char || code > font->last_char) {
		return NULL;
	}

	return &font->glyphs[code - font->first_char];
}

/**
 * @brief Декодировать одну кодовую точку UTF-8 из строки *pp и продвинуть
 *        *pp за прочитанные байты.
 *
 * Безопасно относительно завершающего нулевого байта строки: '\0' не
 * является валидным продолжающим байтом UTF-8 (0x80-0xBF), поэтому цикл
 * чтения продолжающих байтов всегда останавливается на нём или раньше,
 * НИКОГДА не читая память за пределы найденного '\0'. Оборванная
 * (некорректная) последовательность в конце строки просто даёт частично
 * декодированную кодовую точку и останавливается — не приводит к чтению
 * за пределы буфера.
 *
 * @return Декодированная кодовая точка Unicode.
 */
static uint32_t utf8_decode(const char **pp)
{
	const unsigned char *p = (const unsigned char *)*pp;
	uint32_t cp;
	int extra;

	if (p[0] < 0x80u) {
		cp = p[0];
		extra = 0;
	} else if ((p[0] & 0xE0u) == 0xC0u) {
		cp = p[0] & 0x1Fu;
		extra = 1;
	} else if ((p[0] & 0xF0u) == 0xE0u) {
		cp = p[0] & 0x0Fu;
		extra = 2;
	} else if ((p[0] & 0xF8u) == 0xF0u) {
		cp = p[0] & 0x07u;
		extra = 3;
	} else {
		/* Невалидный стартовый байт (одинокий продолжающий байт или
		 * зарезервированное значение) — трактуем как есть, без
		 * продолжения, чтобы не потерять синхронизацию навсегда. */
		cp = p[0];
		extra = 0;
	}

	p++;

	for (int i = 0; i < extra; i++) {
		if ((p[0] & 0xC0u) != 0x80u) {
			/* Не продолжающий байт (в т.ч. '\0') — обрываем
			 * декодирование ЗДЕСЬ, не потребляя этот байт: он
			 * будет заново интерпретирован как начало следующего
			 * символа на следующем вызове (или как '\0' в конце
			 * строки, что корректно завершит цикл в вызывающем
			 * коде). */
			break;
		}
		cp = (cp << 6) | (p[0] & 0x3Fu);
		p++;
	}

	*pp = (const char *)p;
	return cp;
}

int custom_mpix_draw_char(struct custom_mpix_canvas *canvas, int32_t x, int32_t y,
		    uint32_t codepoint, const struct custom_mpix_font *font, struct custom_mpix_rgb24 color)
{
	if (!canvas_is_valid(canvas) || font == NULL) {
		return -EINVAL;
	}

	const struct custom_mpix_glyph *glyph = find_glyph(font, codepoint);

	if (glyph == NULL || glyph->width == 0 || glyph->height == 0) {
		/* Символ вне диапазона шрифта или "пустой" глиф (например,
		 * пробел без собственного битмапа) — рисовать нечего, это не
		 * ошибка, символ просто пропускается. */
		return 0;
	}

	/* Формат битмапа — 1 бит/пиксель, MSB первым, глифы упакованы подряд
	 * начиная с байта glyph->bitmap_offset. Каждые 8 прочитанных пикселей
	 * подгружаем следующий байт. Тот же алгоритм, что в оригинальном
	 * Adafruit-GFX/fb_gfx drawChar — это и обеспечивает бинарную
	 * совместимость с готовыми шрифтами этого формата. */
	uint16_t bo = glyph->bitmap_offset;
	uint16_t bit = 0;
	uint8_t bits = 0;

	for (uint8_t yy = 0; yy < glyph->height; yy++) {
		for (uint8_t xx = 0; xx < glyph->width; xx++) {
			if ((bit & 7u) == 0) {
				bits = font->bitmap[bo++];
			}
			bit++;

			if (bits & 0x80u) {
				(void)custom_mpix_draw_set_pixel(canvas,
					x + glyph->x_offset + xx,
					y + glyph->y_offset + yy,
					color);
			}

			bits <<= 1;
		}
	}

	return 0;
}

int custom_mpix_draw_text(struct custom_mpix_canvas *canvas, int32_t x, int32_t y,
		    const char *str, const struct custom_mpix_font *font, struct custom_mpix_rgb24 color)
{
	if (!canvas_is_valid(canvas) || str == NULL || font == NULL) {
		return -EINVAL;
	}

	int32_t cx = x;
	int32_t cy = y;
	const char *p = str;

	while (*p != '\0') {
		uint32_t code = utf8_decode(&p);

		if (code == '\n') {
			cx = x;
			cy += font->y_advance;
			continue;
		}

		const struct custom_mpix_glyph *glyph = find_glyph(font, code);

		if (glyph == NULL) {
			/* Символ вне диапазона шрифта — пропускаем, курсор не
			 * двигаем. */
			continue;
		}

		(void)custom_mpix_draw_char(canvas, cx, cy, code, font, color);
		cx += glyph->x_advance;
	}

	return 0;
}

int custom_mpix_text_measure(const struct custom_mpix_font *font, const char *str,
		       uint16_t *out_w, uint16_t *out_h)
{
	if (font == NULL || str == NULL || out_w == NULL || out_h == NULL) {
		return -EINVAL;
	}

	uint16_t max_w = 0;
	uint16_t cur_w = 0;
	uint16_t lines = 1;
	const char *p = str;

	while (*p != '\0') {
		uint32_t code = utf8_decode(&p);

		if (code == '\n') {
			lines++;
			if (cur_w > max_w) {
				max_w = cur_w;
			}
			cur_w = 0;
			continue;
		}

		const struct custom_mpix_glyph *glyph = find_glyph(font, code);

		if (glyph == NULL) {
			continue;
		}

		cur_w += glyph->x_advance;
	}

	if (cur_w > max_w) {
		max_w = cur_w;
	}

	*out_w = max_w;
	*out_h = (uint16_t)(lines * font->y_advance);

	return 0;
}