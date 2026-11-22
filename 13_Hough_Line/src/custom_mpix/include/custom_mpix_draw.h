/**
 * @file custom_mpix_draw.h
 * @brief Модуль прямой отрисовки геометрических фигур и текста на буфере
 *        изображения в формате MPIX_FMT_RGB24 ('R','G','B','3').
 *
 * В отличие от штатных операций libmpix (mpix/operation.h), которые строятся
 * по построчной (line-streaming) модели конвейера, данный модуль работает
 * напрямую с уже полностью собранным в памяти RGB24-буфером — то есть
 * применяется ПОСЛЕ mpix_image_to_buf(), когда весь кадр целиком доступен
 * по указателю (как buf_out в примере пайплайна камеры).
 *
 * Модуль не является операцией конвейера (MPIX_OP_*) и не регистрируется
 * через MPIX_REGISTER_OP. Он ничего не аллоцирует: struct custom_mpix_canvas — это
 * лёгкая обёртка (view) поверх буфера, который выделен и живёт на стороне
 * приложения.
 *
 * Соглашения:
 *  - Формат пикселя: 3 байта на пиксель, порядок байт R, G, B, без padding
 *    между строками (stride = width * 3).
 *  - Все функции отрисовки, чьи параметры (координаты, радиус, длина)
 *    частично или полностью выходят за границы канваса, молча обрезают
 *    видимую часть фигуры. Это НЕ считается ошибкой и не отражается
 *    в коде возврата.
 *  - Отрицательные коды возврата (-EINVAL, -ENOMEM и т.п. из errno.h)
 *    сигнализируют только о программных ошибках вызывающей стороны:
 *    NULL-указатели, неинициализированный канвас, нулевые размеры канваса
 *    и т.п. Геометрический выход фигуры за пределы буфера — не ошибка.
 *  - Толщина линий/контуров НЕ фиксирована и передаётся явным параметром
 *    в каждую функцию, где это применимо.
 */

#ifndef CUSTOM_MPIX_DRAW_H_
#define CUSTOM_MPIX_DRAW_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Канвас — обёртка (view) поверх уже существующего RGB24-буфера.
 *
 * Не владеет памятью buf, только хранит указатель и геометрию.
 * Инициализируется через custom_mpix_canvas_from_buf().
 */
struct custom_mpix_canvas {
	uint8_t *buf;    /**< Указатель на буфер изображения (R,G,B, R,G,B, ...). */
	uint16_t width;  /**< Ширина изображения в пикселях. */
	uint16_t height; /**< Высота изображения в пикселях. */
	size_t stride;   /**< Байт на строку. Всегда width * 3 для RGB24. */
};

/** @brief Цвет пикселя в формате RGB24 (порядок байт: R, G, B). */
struct custom_mpix_rgb24 {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

/** @brief Конструктор цвета из отдельных компонент. */
#define CUSTOM_MPIX_RGB24(_r, _g, _b) ((struct custom_mpix_rgb24){ .r = (_r), .g = (_g), .b = (_b) })

/* Набор часто используемых цветов-констант. */
#define CUSTOM_MPIX_COLOR_BLACK   CUSTOM_MPIX_RGB24(0x00, 0x00, 0x00)
#define CUSTOM_MPIX_COLOR_WHITE   CUSTOM_MPIX_RGB24(0xFF, 0xFF, 0xFF)
#define CUSTOM_MPIX_COLOR_RED     CUSTOM_MPIX_RGB24(0x00, 0xFF, 0x00)
#define CUSTOM_MPIX_COLOR_GREEN   CUSTOM_MPIX_RGB24(0x00, 0x00, 0xFF)
#define CUSTOM_MPIX_COLOR_BLUE    CUSTOM_MPIX_RGB24(0xFF, 0x00, 0x00)
#define CUSTOM_MPIX_COLOR_YELLOW  CUSTOM_MPIX_RGB24(0x00, 0xFF, 0xFF)
#define CUSTOM_MPIX_COLOR_CYAN    CUSTOM_MPIX_RGB24(0xFF, 0xFF, 0x00)
#define CUSTOM_MPIX_COLOR_MAGENTA CUSTOM_MPIX_RGB24(0xFF, 0x00, 0xFF)

/**
 * @brief Описание одного глифа шрифта.
 *
 * Поля намеренно совмещены по смыслу и порядку с полями GFXglyph из
 * формата шрифтов Adafruit-GFX (тот же формат, что используют
 * fontconvert-генерируемые заголовки вида FreeMonoBold12pt7b.h). Это
 * позволяет подключать готовые открытые шрифты напрямую или тонкой
 * обёрткой, без переписывания растровых данных.
 */
struct custom_mpix_glyph {
	uint16_t bitmap_offset; /**< Байтовое смещение начала битмапа глифа внутри custom_mpix_font::bitmap. */
	uint8_t width;          /**< Ширина битмапа глифа в пикселях. */
	uint8_t height;         /**< Высота битмапа глифа в пикселях. */
	uint8_t x_advance;      /**< На сколько пикселей сдвинуть курсор после отрисовки символа. */
	int8_t x_offset;        /**< Смещение по X от курсора до левого края битмапа глифа. */
	int8_t y_offset;        /**< Смещение по Y от базовой линии до верхнего края битмапа глифа. */
};

/**
 * @brief Растровый моноширинный/пропорциональный шрифт, 1 бит на пиксель.
 *
 * bitmap — сплошной массив бит (MSB первым в байте), содержащий битмапы
 * всех глифов подряд, на которые ссылаются custom_mpix_glyph::bitmap_offset.
 *
 * ВАЖНО (расширение формата под Unicode/кириллицу): first_char/last_char —
 * это КОДОВЫЕ ТОЧКИ Unicode (не байты), тип uint32_t. Оригинальный формат
 * Adafruit-GFX ограничен uint8_t (только Latin-1/ASCII, до 255) — этого
 * достаточно для custom_mpix_font_freemonobold12pt (0x20-0x7E), но
 * принципиально недостаточно для кириллицы (У+0400 и выше). Строки,
 * передаваемые в custom_mpix_draw_text/custom_mpix_text_measure, должны
 * быть в кодировке UTF-8 — многобайтовые последовательности декодируются
 * в кодовые точки автоматически. Для чисто ASCII-строк (как раньше)
 * поведение не меняется: каждый байт < 0x80 — это и есть кодовая точка.
 */
struct custom_mpix_font {
	const uint8_t *bitmap;           /**< Общий битмап всех глифов, 1 бит/пиксель, MSB-first. */
	const struct custom_mpix_glyph *glyphs; /**< Массив глифов, индекс = (кодовая точка - first_char). */
	uint32_t first_char;              /**< Кодовая точка Unicode первого символа, описанного в glyphs[0]. */
	uint32_t last_char;                /**< Кодовая точка Unicode последнего символа, описанного в glyphs[]. */
	uint8_t y_advance;                /**< Межстрочный интервал в пикселях для многострочного текста. */
};

/* -------------------------------------------------------------------- */
/* Инициализация канваса                                                 */
/* -------------------------------------------------------------------- */

/**
 * @brief Инициализировать канвас поверх существующего RGB24-буфера.
 *
 * @param canvas Указатель на структуру канваса для заполнения.
 * @param buf    Указатель на буфер изображения (не копируется, должен жить
 *               не меньше, чем канвас используется).
 * @param width  Ширина изображения в пикселях.
 * @param height Высота изображения в пикселях.
 *
 * @retval 0        Успех.
 * @retval -EINVAL  canvas == NULL, buf == NULL, width == 0 или height == 0.
 */
int custom_mpix_canvas_from_buf(struct custom_mpix_canvas *canvas, uint8_t *buf, uint16_t width, uint16_t height);

/* -------------------------------------------------------------------- */
/* Пиксельный уровень                                                    */
/* -------------------------------------------------------------------- */

/**
 * @brief Установить цвет одного пикселя.
 *
 * Если (x, y) вне границ канваса — вызов молча ничего не делает,
 * это не считается ошибкой.
 *
 * @retval 0        Успех (в т.ч. если пиксель был за пределами канваса).
 * @retval -EINVAL  canvas == NULL или canvas->buf == NULL.
 */
int custom_mpix_draw_set_pixel(struct custom_mpix_canvas *canvas, int32_t x, int32_t y, struct custom_mpix_rgb24 color);

/**
 * @brief Прочитать цвет одного пикселя.
 *
 * В отличие от set_pixel, чтение вне границ канваса — программная ошибка
 * вызывающей стороны (нет "видимой части" для чтения), поэтому возвращает
 * -ERANGE, а не молчаливый успех.
 *
 * @retval 0        Успех, *color заполнен.
 * @retval -EINVAL  canvas == NULL, canvas->buf == NULL или color == NULL.
 * @retval -ERANGE  (x, y) вне границ канваса.
 */
int custom_mpix_draw_get_pixel(const struct custom_mpix_canvas *canvas, int32_t x, int32_t y, struct custom_mpix_rgb24 *color);

/* -------------------------------------------------------------------- */
/* Линии                                                                 */
/* -------------------------------------------------------------------- */

/**
 * @brief Нарисовать горизонтальную линию.
 *
 * @param x, y      Координаты начальной (левой) точки линии.
 * @param length    Длина линии в пикселях (вправо от (x, y)).
 * @param thickness Толщина линии в пикселях (>= 1), линия "растёт" вниз от y.
 * @param color     Цвет линии.
 *
 * Часть линии за пределами канваса молча отсекается.
 *
 * @retval 0        Успех.
 * @retval -EINVAL  canvas == NULL/не инициализирован или thickness == 0.
 */
int custom_mpix_draw_hline(struct custom_mpix_canvas *canvas, int32_t x, int32_t y,
		     int32_t length, uint16_t thickness, struct custom_mpix_rgb24 color);

/**
 * @brief Нарисовать вертикальную линию.
 *
 * @param x, y      Координаты начальной (верхней) точки линии.
 * @param length    Длина линии в пикселях (вниз от (x, y)).
 * @param thickness Толщина линии в пикселях (>= 1), линия "растёт" вправо от x.
 * @param color     Цвет линии.
 *
 * @retval 0        Успех.
 * @retval -EINVAL  canvas == NULL/не инициализирован или thickness == 0.
 */
int custom_mpix_draw_vline(struct custom_mpix_canvas *canvas, int32_t x, int32_t y,
		     int32_t length, uint16_t thickness, struct custom_mpix_rgb24 color);

/**
 * @brief Нарисовать произвольную линию между двумя точками (алгоритм Брезенхэма).
 *
 * @param x0, y0    Начальная точка.
 * @param x1, y1    Конечная точка.
 * @param thickness Толщина линии в пикселях (>= 1).
 * @param color     Цвет линии.
 *
 * @retval 0        Успех.
 * @retval -EINVAL  canvas == NULL/не инициализирован или thickness == 0.
 */
int custom_mpix_draw_line(struct custom_mpix_canvas *canvas, int32_t x0, int32_t y0,
		    int32_t x1, int32_t y1, uint16_t thickness, struct custom_mpix_rgb24 color);

/* -------------------------------------------------------------------- */
/* Прямоугольники                                                        */
/* -------------------------------------------------------------------- */

/**
 * @brief Нарисовать контур прямоугольника.
 *
 * @param x, y      Координаты левого верхнего угла.
 * @param w, h      Ширина и высота прямоугольника.
 * @param thickness Толщина контура в пикселях (>= 1), растёт внутрь фигуры.
 * @param color     Цвет контура.
 *
 * @retval 0        Успех.
 * @retval -EINVAL  canvas == NULL/не инициализирован, w == 0, h == 0 или thickness == 0.
 */
int custom_mpix_draw_rect(struct custom_mpix_canvas *canvas, int32_t x, int32_t y,
		    int32_t w, int32_t h, uint16_t thickness, struct custom_mpix_rgb24 color);

/**
 * @brief Нарисовать закрашенный прямоугольник.
 *
 * @param x, y  Координаты левого верхнего угла.
 * @param w, h  Ширина и высота прямоугольника.
 * @param color Цвет заливки.
 *
 * @retval 0        Успех.
 * @retval -EINVAL  canvas == NULL/не инициализирован, w == 0 или h == 0.
 */
int custom_mpix_draw_rect_fill(struct custom_mpix_canvas *canvas, int32_t x, int32_t y,
			 int32_t w, int32_t h, struct custom_mpix_rgb24 color);

/* -------------------------------------------------------------------- */
/* Окружности                                                            */
/* -------------------------------------------------------------------- */

/**
 * @brief Нарисовать контур окружности (алгоритм Брезенхэма/midpoint circle).
 *
 * @param cx, cy    Координаты центра.
 * @param radius    Радиус в пикселях.
 * @param thickness Толщина контура в пикселях (>= 1), растёт внутрь окружности.
 * @param color     Цвет контура.
 *
 * @retval 0        Успех.
 * @retval -EINVAL  canvas == NULL/не инициализирован, radius == 0 или thickness == 0.
 */
int custom_mpix_draw_circle(struct custom_mpix_canvas *canvas, int32_t cx, int32_t cy,
		      int32_t radius, uint16_t thickness, struct custom_mpix_rgb24 color);

/**
 * @brief Нарисовать закрашенную окружность.
 *
 * @param cx, cy Координаты центра.
 * @param radius Радиус в пикселях.
 * @param color  Цвет заливки.
 *
 * @retval 0        Успех.
 * @retval -EINVAL  canvas == NULL/не инициализирован или radius == 0.
 */
int custom_mpix_draw_circle_fill(struct custom_mpix_canvas *canvas, int32_t cx, int32_t cy,
			   int32_t radius, struct custom_mpix_rgb24 color);

/* -------------------------------------------------------------------- */
/* Текст                                                                 */
/* -------------------------------------------------------------------- */

/**
 * @brief Отрисовать один символ шрифтом font, закрашивая только пиксели
 *        глифа цветом color (фон не трогается — символ "монолитный").
 *
 * @param x, y      Координаты базовой точки курсора (левый край, базовая линия).
 * @param codepoint Кодовая точка Unicode отображаемого символа (для ASCII
 *                  совпадает со значением char, для кириллицы — код
 *                  U+0400 и выше; см. struct custom_mpix_font).
 * @param font      Шрифт (см. struct custom_mpix_font).
 * @param color     Цвет символа.
 *
 * Если codepoint вне диапазона [font->first_char, font->last_char] — вызов
 * ничего не рисует и возвращает 0 (не ошибка, символ просто пропускается,
 * как пробел).
 *
 * @retval 0        Успех.
 * @retval -EINVAL  canvas == NULL/не инициализирован или font == NULL.
 */
int custom_mpix_draw_char(struct custom_mpix_canvas *canvas, int32_t x, int32_t y,
		    uint32_t codepoint, const struct custom_mpix_font *font, struct custom_mpix_rgb24 color);

/**
 * @brief Отрисовать строку текста шрифтом font.
 *
 * Курсор продвигается по x_advance каждого символа. Символ '\n' переносит
 * курсор на новую строку (x возвращается к исходному x, y увеличивается
 * на font->y_advance).
 *
 * @param x, y  Координаты начальной точки текста (левый верхний угол первой строки).
 * @param str   Строка в кодировке UTF-8 с завершающим нулевым байтом. Для
 *              шрифтов с диапазоном внутри ASCII (например,
 *              custom_mpix_font_freemonobold12pt) поведение не отличается
 *              от простой байтовой строки — многобайтовые UTF-8
 *              последовательности актуальны только для символов с кодом
 *              0x80 и выше (кириллица и т.п.).
 * @param font  Шрифт.
 * @param color Цвет текста.
 *
 * Некорректная/оборванная UTF-8 последовательность в конце строки не
 * читает память за пределами завершающего нулевого байта строки — декодер
 * останавливается, как только видит байт, не являющийся валидным
 * продолжением многобайтовой последовательности (в т.ч. сам нулевой байт).
 *
 * @retval 0        Успех.
 * @retval -EINVAL  canvas == NULL/не инициализирован, str == NULL или font == NULL.
 */
int custom_mpix_draw_text(struct custom_mpix_canvas *canvas, int32_t x, int32_t y,
		    const char *str, const struct custom_mpix_font *font, struct custom_mpix_rgb24 color);

/**
 * @brief Посчитать ширину и высоту прямоугольной области, которую займёт
 *        строка text при отрисовке шрифтом font (без фактического рисования).
 *
 * Полезно для выравнивания/центрирования текста перед вызовом custom_mpix_draw_text().
 * Высота считается как (число строк) * font->y_advance — приближённо, без
 * учёта точных верхних/нижних выступов конкретных глифов в первой/последней строке.
 *
 * @param font    Шрифт.
 * @param str     Строка в кодировке UTF-8 с завершающим нулевым байтом.
 * @param out_w   [out] Итоговая ширина в пикселях (по самой длинной строке).
 * @param out_h   [out] Итоговая высота в пикселях (с учётом переносов строк).
 *
 * @retval 0        Успех.
 * @retval -EINVAL  font == NULL, str == NULL, out_w == NULL или out_h == NULL.
 */
int custom_mpix_text_measure(const struct custom_mpix_font *font, const char *str,
		       uint16_t *out_w, uint16_t *out_h);

#ifdef __cplusplus
}
#endif

#endif /* CUSTOM_MPIX_DRAW_H_ */