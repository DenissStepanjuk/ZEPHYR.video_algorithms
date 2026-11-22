#!/usr/bin/env python3
"""
ttf_to_custom_mpix_font.py — генератор шрифтов в формате struct
custom_mpix_font/struct custom_mpix_glyph (mpix_draw) напрямую из TTF/OTF,
без промежуточного формата GFXfont.

В отличие от чернового алгоритма, который набрасывал ChatGPT (без учёта
особенностей этого проекта), здесь принципиально важны два момента:

1. БАЗОВАЯ ЛИНИЯ. PIL/FreeType по умолчанию возвращают bbox символа
   относительно "ascender line" (anchor="la"), а не относительно базовой
   линии. custom_mpix_glyph::y_offset у нас (как и yOffset в оригинальном
   Adafruit-GFX) определён как смещение от БАЗОВОЙ ЛИНИИ. Поэтому здесь
   явно используется anchor="ls" (left-baseline) и для getbbox(), и для
   draw.text() — тогда bbox.y0 уже прямо равен нужному y_offset (со
   знаком: отрицательный = выше базовой линии), без ручных пересчётов
   вида "-y0", которые в черновом алгоритме тихо давали неверный результат
   (проверено на конкретных числах: для 'A' в этом шрифте anchor="la" даёт
   y0=4, а -y0=-4 — тогда как правильное значение через anchor="ls" — y0=-12).

2. ПОРОГ 1bpp. Наш формат битмапа — строго 1 бит/пиксель (MSB-first),
   без сглаживания. Чтобы не зависеть от недокументированного поведения
   PIL при рисовании текста прямо в 1-битное изображение, здесь текст
   рендерится в 8-битную (`L`) маску с честным alpha-покрытием от
   FreeType, а затем ЯВНО бинаризуется по порогу (>=128 -> бит установлен).

Использование:
    python3 ttf_to_custom_mpix_font.py FreeMonoBold.ttf \\
        --size 22 \\
        --name custom_mpix_font_freemonobold12pt_ru \\
        --ranges "0x20-0x7E,0x0401,0x0410-0x042F,0x0430-0x044F,0x0451" \\
        -o custom_mpix_font_freemonobold12pt_ru.h
"""

import argparse
import sys

from PIL import Image, ImageDraw, ImageFont


def parse_ranges(spec: str):
    """'0x20-0x7E,0x0401,0x0410-0x042F' -> отсортированный список кодовых точек без повторов."""
    codepoints = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo, hi = part.split("-")
            lo = int(lo, 0)
            hi = int(hi, 0)
            codepoints.update(range(lo, hi + 1))
        else:
            codepoints.add(int(part, 0))
    return sorted(codepoints)


def render_glyph_bits(font, cp, threshold):
    """
    Возвращает (width, height, x_advance, x_offset, y_offset, rows_bits) для
    одной кодовой точки, либо (0, 0, x_advance, 0, 0, []) для пустого глифа
    (пробел / отсутствующий в шрифте символ).

    rows_bits — список списков 0/1 по строкам (row-major), уже
    бинаризованных по threshold.
    """
    ch = chr(cp)
    x_advance = round(font.getlength(ch))

    bbox = font.getbbox(ch, anchor="ls")
    if bbox is None:
        return 0, 0, x_advance, 0, 0, []

    x0, y0, x1, y1 = bbox
    width = x1 - x0
    height = y1 - y0
    if width <= 0 or height <= 0:
        return 0, 0, x_advance, 0, 0, []

    img = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(img)
    # anchor="ls" СОГЛАСОВАННО с getbbox(..., anchor="ls") выше — точка
    # (-x0, -y0) в изображении соответствует левому краю базовой линии.
    draw.text((-x0, -y0), ch, font=font, fill=255, anchor="ls")

    px = img.load()
    rows = []
    any_set = False
    for y in range(height):
        row = [1 if px[x, y] >= threshold else 0 for x in range(width)]
        if any(row):
            any_set = True
        rows.append(row)

    if not any_set:
        # Символ технически имеет ненулевой bbox, но после бинаризации
        # пуст (редкий крайний случай) — трактуем как пустой глиф.
        return 0, 0, x_advance, 0, 0, []

    return width, height, x_advance, x0, y0, rows


def pack_bits_msb_first(rows):
    """Row-major, MSB первым в каждом байте, конец глифа выравнивается по байту."""
    out = bytearray()
    bitbuf = 0
    bitcnt = 0
    for row in rows:
        for bit in row:
            bitbuf = (bitbuf << 1) | bit
            bitcnt += 1
            if bitcnt == 8:
                out.append(bitbuf)
                bitbuf = 0
                bitcnt = 0
    if bitcnt != 0:
        bitbuf <<= (8 - bitcnt)
        out.append(bitbuf)
    return bytes(out)


def generate(ttf_path, size, codepoints, threshold):
    font = ImageFont.truetype(ttf_path, size)
    ascent, descent = font.getmetrics()
    y_advance = ascent + descent

    bitmap = bytearray()
    glyphs = []  # (bitmap_offset, width, height, x_advance, x_offset, y_offset, codepoint)

    for cp in codepoints:
        width, height, x_advance, x0, y0, rows = render_glyph_bits(font, cp, threshold)
        offset = len(bitmap)
        if width > 0 and height > 0:
            packed = pack_bits_msb_first(rows)
            bitmap.extend(packed)

        for bound_name, val in (("width", width), ("height", height)):
            if val > 255:
                raise ValueError(f"Глиф U+{cp:04X}: {bound_name}={val} не влезает в uint8_t. "
                                  f"Уменьшите --size.")
        for bound_name, val in (("x_advance", x_advance),):
            if val > 255:
                raise ValueError(f"Глиф U+{cp:04X}: {bound_name}={val} не влезает в uint8_t. "
                                  f"Уменьшите --size.")
        for bound_name, val in (("x_offset", x0), ("y_offset", y0)):
            if not (-128 <= val <= 127):
                raise ValueError(f"Глиф U+{cp:04X}: {bound_name}={val} не влезает в int8_t. "
                                  f"Уменьшите --size.")
        if offset > 0xFFFF:
            raise ValueError("bitmap_offset превысил uint16_t — шрифт слишком большой "
                              "для этого формата, уменьшите набор символов или --size.")

        glyphs.append((offset, width, height, x_advance, x0, y0, cp))

    return bitmap, glyphs, y_advance


def render_header(name, ttf_path, size, bitmap, glyphs, y_advance, first_cp, last_cp):
    lines = []
    lines.append("/**")
    lines.append(f" * @file {name}.h")
    lines.append(f" * @brief Автоматически сгенерировано ttf_to_custom_mpix_font.py из")
    lines.append(f" *        {ttf_path} (GNU FreeFont), pixel size={size}.")
    lines.append(f" *        Формат совместим со struct custom_mpix_font (custom_mpix_draw.h).")
    lines.append(f" *        Диапазон: U+{first_cp:04X}-U+{last_cp:04X}.")
    lines.append(" */")
    lines.append("")
    guard = "".join(c if c.isalnum() else "_" for c in name).upper() + "_H_"
    lines.append(f"#ifndef {guard}")
    lines.append(f"#define {guard}")
    lines.append("")
    lines.append('#include "custom_mpix_draw.h"')
    lines.append("")
    lines.append("#if !defined(CUSTOM_MPIX_FONT_ABI_VERSION) || CUSTOM_MPIX_FONT_ABI_VERSION < 2")
    lines.append('#error "Этот файл шрифта требует custom_mpix_draw.h с ABI версии >= 2 '
                  '(first_char/last_char как uint32_t, поддержка Unicode). '
                  'Обновите custom_mpix_draw.h и custom_mpix_draw.c в проекте до актуальной версии."')
    lines.append("#endif")
    lines.append("")

    bitmap_name = f"{name}_bitmap"
    glyphs_name = f"{name}_glyphs"

    lines.append(f"static const uint8_t {bitmap_name}[] = {{")
    for i in range(0, len(bitmap), 12):
        chunk = bitmap[i:i + 12]
        lines.append("\t" + " ".join(f"0x{b:02X}," for b in chunk))
    lines.append("};")
    lines.append("")

    lines.append(f"static const struct custom_mpix_glyph {glyphs_name}[] = {{")
    for offset, width, height, x_advance, x0, y0, cp in glyphs:
        try:
            comment_ch = chr(cp)
            comment_ch = comment_ch if comment_ch.isprintable() else "?"
        except ValueError:
            comment_ch = "?"
        lines.append(
            f"\t{{ {offset}, {width}, {height}, {x_advance}, {x0}, {y0} }}, "
            f"/* U+{cp:04X} '{comment_ch}' */"
        )
    lines.append("};")
    lines.append("")

    lines.append(f"const struct custom_mpix_font {name} = {{")
    lines.append(f"\t.bitmap = {bitmap_name},")
    lines.append(f"\t.glyphs = {glyphs_name},")
    lines.append(f"\t.first_char = 0x{first_cp:04X},")
    lines.append(f"\t.last_char = 0x{last_cp:04X},")
    lines.append(f"\t.y_advance = {y_advance},")
    lines.append("};")
    lines.append("")
    lines.append(f"#endif /* {guard} */")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ttf", help="Путь к TTF/OTF файлу шрифта")
    ap.add_argument("--size", type=int, required=True, help="Размер шрифта в пикселях (PIL truetype size)")
    ap.add_argument("--name", required=True, help="Имя переменной struct custom_mpix_font")
    ap.add_argument("--ranges", required=True,
                     help="Диапазоны кодовых точек через запятую, напр. '0x20-0x7E,0x0401,0x0410-0x044F'")
    ap.add_argument("--threshold", type=int, default=128,
                     help="Порог бинаризации альфа-канала 0-255 (по умолчанию 128)")
    ap.add_argument("-o", "--output", required=True, help="Путь к выходному .h файлу")
    args = ap.parse_args()

    codepoints = parse_ranges(args.ranges)
    if not codepoints:
        print("Пустой список кодовых точек", file=sys.stderr)
        return 1

    # Диапазон first_char..last_char в нашем формате обязан быть сплошным
    # (индексация массива глифов — прямое смещение), поэтому заполняем
    # промежутки между запрошенными кодовыми точками "пустыми" глифами.
    first_cp = codepoints[0]
    last_cp = codepoints[-1]
    full_range = list(range(first_cp, last_cp + 1))
    requested = set(codepoints)

    font = ImageFont.truetype(args.ttf, args.size)
    bitmap = bytearray()
    glyphs = []
    ascent, descent = font.getmetrics()
    y_advance = ascent + descent

    for cp in full_range:
        if cp in requested:
            width, height, x_advance, x0, y0, rows = render_glyph_bits(font, cp, args.threshold)
        else:
            # Не запрошенный код внутри диапазона (например, промежуточные
            # нелатинские/нерусские кодовые точки между блоками) — пустой
            # глиф с нулевым advance, ведёт себя как полностью пропускаемый
            # символ (custom_mpix_draw_char тихо ничего не рисует).
            width, height, x_advance, x0, y0, rows = 0, 0, 0, 0, 0, []

        offset = len(bitmap)
        if width > 0 and height > 0:
            bitmap.extend(pack_bits_msb_first(rows))

        for bound_name, val, limit in (("width", width, 255), ("height", height, 255),
                                        ("x_advance", x_advance, 255)):
            if val > limit:
                raise ValueError(f"U+{cp:04X}: {bound_name}={val} > {limit}, уменьшите --size")
        for bound_name, val in (("x_offset", x0), ("y_offset", y0)):
            if not (-128 <= val <= 127):
                raise ValueError(f"U+{cp:04X}: {bound_name}={val} вне диапазона int8_t")

        glyphs.append((offset, width, height, x_advance, x0, y0, cp))

    if len(bitmap) > 0xFFFF:
        raise ValueError("Суммарный размер битмапа превысил uint16_t (bitmap_offset)")

    header = render_header(args.name, args.ttf, args.size, bitmap, glyphs,
                            y_advance, first_cp, last_cp)

    with open(args.output, "w", encoding="utf-8") as f:
        f.write(header)

    n_real = len(requested)
    n_gap = len(full_range) - n_real
    print(f"OK: {args.ttf} (size={args.size}) -> {args.output}")
    print(f"    диапазон U+{first_cp:04X}-U+{last_cp:04X} ({len(full_range)} слотов: "
          f"{n_real} реальных глифов, {n_gap} пустых-заполнителей)")
    print(f"    y_advance={y_advance}, bitmap={len(bitmap)} байт, "
          f"glyphs={len(glyphs) * 6} байт метаданных")
    return 0


if __name__ == "__main__":
    sys.exit(main())
