#!/usr/bin/env python3
"""
ttf_to_custom_mpix_font.py — генератор шрифтов в формате struct
custom_mpix_font / struct custom_mpix_glyph (custom_mpix_draw.h) напрямую
из TTF/OTF, без промежуточного формата GFXfont.

Ключевые принципы (проверены на реальных данных, см. историю разработки):

1. БАЗОВАЯ ЛИНИЯ. PIL по умолчанию считает bbox от линии аскендера
   (anchor="la"), а custom_mpix_glyph::y_offset определён от БАЗОВОЙ ЛИНИИ.
   Поэтому и getbbox(), и draw.text() используют anchor="ls", и bbox.y0 уже
   равен нужному y_offset (отрицательный = выше базовой линии).

2. БИНАРИЗАЦИЯ. Формат битмапа — строго 1 бит/пиксель. Глиф рендерится в
   8-битную маску с alpha-покрытием FreeType и явно бинаризуется по порогу
   (--threshold, по умолчанию 128). Режим родного монохромного хинтинга
   FreeType (fontmode "1") на малых размерах (~15 px, FreeMono Bold) дал
   слипшиеся пятна на Ж/ж/ы и был отвергнут по результатам сравнения.

3. ОБРЕЗКА. После бинаризации вокруг глифа остаются пустые колонки/строки
   (бахрома сглаживания, не прошедшая порог). Битмап обрезается по тугим
   границам, x_offset/y_offset/width/height пересчитываются — рисунок
   остаётся пиксель-в-пиксель тем же, но шрифт меньше (отключается --no-trim).

4. ЗАЩИТА ОТ ПОТЕРИ ГЛИФОВ. Если непробельный символ после порога стал
   пустым — порог для него адаптивно снижается (с предупреждением), чтобы
   символ не исчез молча. Если символа нет в cmap самого TTF — генерация
   прерывается (иначе PIL тихо запёк бы квадратик .notdef).

Размер задаётся либо в пикселях (--size, допускается дробный, нужен
Pillow >= 10.1), либо в пунктах (--pt) через коэффициент --px-per-pt.
По умолчанию --px-per-pt = 22/12: именно так был получен выданный ранее
"12pt" шрифт (22 px), поэтому --pt 8 даёт тот же шрифт в пропорции 8:12.
(Оригинальный fontconvert Adafruit использует 141 dpi, т.е. 1 pt = 1.958 px;
для строгого соответствия Adafruit-размеру укажите --px-per-pt 1.9583.)

Использование:
    python3 ttf_to_custom_mpix_font.py FreeMonoBold.ttf --pt 8 \\
        --name custom_mpix_font_freemonobold8pt_ru \\
        --ranges "0x20-0x7E,0x0401,0x0410-0x042F,0x0430-0x044F,0x0451" \\
        -o custom_mpix_font_freemonobold8pt_ru.h
"""

import argparse
import math
import sys

from PIL import Image, ImageDraw, ImageFont

# sizeof(struct custom_mpix_glyph): uint16_t + 5 x 1 байт = 7 байт, с учётом
# выравнивания структуры по 2 байтам = 8. Проверено _Static_assert'ом на gcc
# x86-64 и i386 (-m32 -ffreestanding). На ARM/Xtensa/RISC-V не проверялось
# (нет тулчейнов), но при выравнивании uint16_t по 2 байтам значение то же —
# сверьте sizeof в своём Zephyr-проекте; на корректность шрифта это не влияет,
# число используется только в текстовом отчёте о размере flash.
GLYPH_STRUCT_SIZE = 8

FALLBACK_THRESHOLDS = (96, 64, 32, 1)


def parse_ranges(spec):
    """'0x20-0x7E,0x0401' -> отсортированный список кодовых точек без повторов."""
    codepoints = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo, hi = part.split("-")
            codepoints.update(range(int(lo, 0), int(hi, 0) + 1))
        else:
            codepoints.add(int(part, 0))
    return sorted(codepoints)


def check_coverage(ttf_path, codepoints):
    """Прерывает работу, если запрошенных символов нет в cmap TTF."""
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        print("WARNING: fontTools не установлен — проверка наличия символов в шрифте "
              "пропущена (pip install fonttools). Отсутствующий символ будет тихо "
              "отрисован как .notdef.", file=sys.stderr)
        return
    cmap = TTFont(ttf_path).getBestCmap()
    missing = [cp for cp in codepoints if cp not in cmap]
    if missing:
        shown = ", ".join(f"U+{cp:04X}" for cp in missing[:20])
        more = "" if len(missing) <= 20 else f" ... (+{len(missing) - 20})"
        raise SystemExit(f"ОШИБКА: в {ttf_path} нет {len(missing)} запрошенных символов: "
                         f"{shown}{more}")


def threshold_rows(img, width, height, threshold):
    px = img.load()
    return [[1 if px[x, y] >= threshold else 0 for x in range(width)] for y in range(height)]


def trim_rows(rows, x0, y0):
    """Обрезает пустые края; возвращает (rows, x0, y0)."""
    height = len(rows)
    width = len(rows[0])
    top = next(y for y in range(height) if any(rows[y]))
    bottom = next(y for y in range(height - 1, -1, -1) if any(rows[y]))
    left = next(x for x in range(width) if any(r[x] for r in rows))
    right = next(x for x in range(width - 1, -1, -1) if any(r[x] for r in rows))
    cropped = [r[left:right + 1] for r in rows[top:bottom + 1]]
    return cropped, x0 + left, y0 + top


def render_glyph(font, cp, threshold, trim, warnings):
    """
    -> (width, height, x_advance, x_offset, y_offset, rows)
    Для пустого глифа (пробел) rows == [] и width == height == 0.
    """
    ch = chr(cp)
    # Округление "половина вверх" (а не банковское round()) — предсказуемо.
    x_advance = int(math.floor(font.getlength(ch) + 0.5))

    bbox = font.getbbox(ch, anchor="ls")
    if bbox is None:
        return 0, 0, x_advance, 0, 0, []
    x0, y0, x1, y1 = bbox
    width, height = x1 - x0, y1 - y0
    if width <= 0 or height <= 0:
        return 0, 0, x_advance, 0, 0, []

    img = Image.new("L", (width, height), 0)
    # anchor="ls" согласованно с getbbox: точка (-x0, -y0) — левый край базовой линии.
    ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")

    rows = threshold_rows(img, width, height, threshold)
    if not any(any(r) for r in rows):
        if ch.isspace():
            return 0, 0, x_advance, 0, 0, []
        for th in FALLBACK_THRESHOLDS:
            rows = threshold_rows(img, width, height, th)
            if any(any(r) for r in rows):
                warnings.append(f"U+{cp:04X} '{ch}': пропал бы при пороге {threshold}, "
                                f"использован пониженный порог {th}")
                break
        else:
            warnings.append(f"U+{cp:04X} '{ch}': глиф пуст даже при пороге 1 — записан как пустой")
            return 0, 0, x_advance, 0, 0, []

    if trim:
        rows, x0, y0 = trim_rows(rows, x0, y0)
        width, height = len(rows[0]), len(rows)

    return width, height, x_advance, x0, y0, rows


def pack_bits_msb_first(rows):
    """Row-major, MSB первым, конец глифа выравнивается по байту."""
    out = bytearray()
    acc = 0
    n = 0
    for row in rows:
        for bit in row:
            acc = (acc << 1) | bit
            n += 1
            if n == 8:
                out.append(acc)
                acc = 0
                n = 0
    if n:
        out.append(acc << (8 - n))
    return bytes(out)


def render_header(name, ttf_path, size_desc, bitmap, glyphs, y_advance, first_cp, last_cp):
    guard = "".join(c if c.isalnum() else "_" for c in name).upper() + "_H_"
    bitmap_name = f"{name}_bitmap"
    glyphs_name = f"{name}_glyphs"
    L = []
    L.append("/**")
    L.append(f" * @file {name}.h")
    L.append(f" * @brief Автоматически сгенерировано ttf_to_custom_mpix_font.py из")
    L.append(f" *        {ttf_path} (GNU FreeFont), {size_desc}.")
    L.append(f" *        Формат совместим со struct custom_mpix_font (custom_mpix_draw.h).")
    L.append(f" *        Диапазон: U+{first_cp:04X}-U+{last_cp:04X}.")
    L.append(" */")
    L.append("")
    L.append(f"#ifndef {guard}")
    L.append(f"#define {guard}")
    L.append("")
    L.append('#include "custom_mpix_draw.h"')
    L.append("")
    L.append("#if !defined(CUSTOM_MPIX_FONT_ABI_VERSION) || CUSTOM_MPIX_FONT_ABI_VERSION < 2")
    L.append('#error "Этот файл шрифта требует custom_mpix_draw.h с ABI версии >= 2 '
             '(first_char/last_char как uint32_t, поддержка Unicode). '
             'Обновите custom_mpix_draw.h и custom_mpix_draw.c в проекте до актуальной версии."')
    L.append("#endif")
    L.append("")
    L.append(f"static const uint8_t {bitmap_name}[] = {{")
    for i in range(0, len(bitmap), 12):
        L.append("\t" + " ".join(f"0x{b:02X}," for b in bitmap[i:i + 12]))
    L.append("};")
    L.append("")
    L.append(f"static const struct custom_mpix_glyph {glyphs_name}[] = {{")
    for offset, width, height, x_advance, x0, y0, cp in glyphs:
        ch = chr(cp)
        ch = ch if ch.isprintable() else "?"
        L.append(f"\t{{ {offset}, {width}, {height}, {x_advance}, {x0}, {y0} }}, "
                 f"/* U+{cp:04X} '{ch}' */")
    L.append("};")
    L.append("")
    L.append(f"const struct custom_mpix_font {name} = {{")
    L.append(f"\t.bitmap = {bitmap_name},")
    L.append(f"\t.glyphs = {glyphs_name},")
    L.append(f"\t.first_char = 0x{first_cp:04X},")
    L.append(f"\t.last_char = 0x{last_cp:04X},")
    L.append(f"\t.y_advance = {y_advance},")
    L.append("};")
    L.append("")
    L.append(f"#endif /* {guard} */")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ttf", help="Путь к TTF/OTF файлу шрифта")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--size", type=float, help="Размер в пикселях (допускается дробный, Pillow >= 10.1)")
    g.add_argument("--pt", type=float, help="Размер в пунктах (см. --px-per-pt)")
    ap.add_argument("--px-per-pt", type=float, default=22.0 / 12.0,
                    help="Пикселей на пункт для --pt (по умолчанию 22/12: 12pt == 22px, "
                         "как у выданного ранее 12pt шрифта)")
    ap.add_argument("--name", required=True, help="Имя переменной struct custom_mpix_font")
    ap.add_argument("--ranges", required=True,
                    help="Диапазоны кодовых точек, напр. '0x20-0x7E,0x0401,0x0410-0x044F'")
    ap.add_argument("--threshold", type=int, default=128,
                    help="Порог бинаризации альфа-канала 0-255 (по умолчанию 128)")
    ap.add_argument("--no-trim", action="store_true", help="Не обрезать пустые края глифов")
    ap.add_argument("-o", "--output", required=True, help="Путь к выходному .h файлу")
    args = ap.parse_args()

    if args.pt is not None:
        px = args.pt * args.px_per_pt
        size_desc = f"{args.pt:g}pt = {px:.3f}px (px-per-pt={args.px_per_pt:.4f})"
    else:
        px = args.size
        size_desc = f"size={px:g}px"

    codepoints = parse_ranges(args.ranges)
    if not codepoints:
        raise SystemExit("Пустой список кодовых точек")

    check_coverage(args.ttf, codepoints)

    first_cp, last_cp = codepoints[0], codepoints[-1]
    requested = set(codepoints)

    font = ImageFont.truetype(args.ttf, px)
    ascent, descent = font.getmetrics()
    y_advance = ascent + descent
    if y_advance > 255:
        raise SystemExit(f"y_advance={y_advance} не влезает в uint8_t, уменьшите размер")

    bitmap = bytearray()
    glyphs = []
    warnings = []
    for cp in range(first_cp, last_cp + 1):
        if cp in requested:
            width, height, x_advance, x0, y0, rows = render_glyph(
                font, cp, args.threshold, not args.no_trim, warnings)
        else:
            # Незапрошенный код внутри сплошного диапазона — пустой глиф с нулевым
            # advance: custom_mpix_draw_char ничего не рисует, курсор не двигается.
            width = height = x_advance = x0 = y0 = 0
            rows = []

        for label, val, lo, hi in (("width", width, 0, 255), ("height", height, 0, 255),
                                   ("x_advance", x_advance, 0, 255),
                                   ("x_offset", x0, -128, 127), ("y_offset", y0, -128, 127)):
            if not (lo <= val <= hi):
                raise SystemExit(f"U+{cp:04X}: {label}={val} вне диапазона [{lo},{hi}], "
                                 f"уменьшите размер")

        offset = len(bitmap)
        if offset > 0xFFFF:
            raise SystemExit("bitmap_offset превысил uint16_t — шрифт слишком большой")
        if width and height:
            bitmap.extend(pack_bits_msb_first(rows))
        glyphs.append((offset, width, height, x_advance, x0, y0, cp))

    if len(bitmap) > 0xFFFF:
        raise SystemExit("Суммарный размер битмапа превысил uint16_t (bitmap_offset)")

    with open(args.output, "w", encoding="utf-8") as f:
        f.write(render_header(args.name, args.ttf, size_desc, bitmap, glyphs,
                              y_advance, first_cp, last_cp))

    slots = last_cp - first_cp + 1
    gap = slots - len(requested)
    meta = slots * GLYPH_STRUCT_SIZE
    print(f"OK: {args.ttf} ({size_desc}) -> {args.output}")
    print(f"    диапазон U+{first_cp:04X}-U+{last_cp:04X}: {slots} слотов = "
          f"{len(requested)} реальных + {gap} пустых-заполнителей")
    print(f"    y_advance={y_advance}; битмап={len(bitmap)} Б; метаданные="
          f"{meta} Б (sizeof(glyph)={GLYPH_STRUCT_SIZE}), из них на заполнители {gap * GLYPH_STRUCT_SIZE} Б; "
          f"итого ~{len(bitmap) + meta} Б flash")
    for w in warnings:
        print(f"    WARNING: {w}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
