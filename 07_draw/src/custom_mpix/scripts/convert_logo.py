#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Подготовка логотипа для custom_mpix_combine, встраивается в сборку через
CMakeLists.txt (add_custom_command), пересобирается только при изменении
исходной картинки или самого скрипта (см. DEPENDS).

Конвертирует изображение (esp32.jpg или любое другое) в .inc-файл — тот же
формат, что генерирует стандартный Zephyr'овский file2hex.py для
index.html.gz.inc/esp32.jpg.inc: список байт "0x12, 0x34, ..." без внешних
фигурных скобок, готовый для #include внутри "static const uint8_t x[] = {...}".

Требует Pillow: pip install --break-system-packages Pillow

Использование (обычно вызывается из CMakeLists.txt, не руками):
    python3 convert_logo.py esp32.jpg esp32_logo.raw.inc --width 40 --height 30
"""
import argparse
import sys
from PIL import Image

BYTES_PER_LINE = 16


def main():
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("input", help="Исходное изображение (JPEG/PNG/...)")
	parser.add_argument("output", help="Куда записать .inc (текстовый C-массив байт)")
	parser.add_argument("--width", type=int, required=True, help="Ширина логотипа в пикселях")
	parser.add_argument("--height", type=int, required=True, help="Высота логотипа в пикселях")
	args = parser.parse_args()

	try:
		img = Image.open(args.input).convert("RGB")
	except Exception as exc:
		print(f"error: не удалось открыть {args.input}: {exc}", file=sys.stderr)
		return 1

	img = img.resize((args.width, args.height))
	raw = img.tobytes()  # Pillow уже отдаёт построчно R,G,B,R,G,B,... без правок.

	expected_len = args.width * args.height * 3
	if len(raw) != expected_len:
		print(f"error: ожидалось {expected_len} байт, получено {len(raw)}", file=sys.stderr)
		return 1

	with open(args.output, "w") as f:
		f.write(f"/* Автоматически сгенерировано convert_logo.py из {args.input}. Не редактировать руками. */\n")
		for i in range(0, len(raw), BYTES_PER_LINE):
			chunk = raw[i:i + BYTES_PER_LINE]
			line = ", ".join(f"0x{b:02X}" for b in chunk)
			f.write(line + ",\n")

	print(f"OK: {args.output} - {args.width}x{args.height}, {len(raw)} байт")
	return 0


if __name__ == "__main__":
	sys.exit(main())



# #!/usr/bin/env python3
# # SPDX-License-Identifier: Apache-2.0
# """
# Разовая офлайн-подготовка логотипа для custom_mpix_combine.

# Конвертирует esp32.jpg (или любой другой JPEG/PNG) в сырой RGB24-блок
# (3 байта на пиксель, порядок R,G,B, без заголовков) нужного размера —
# именно такой блок ожидает custom_mpix_combine_insert_rgb24() в качестве
# buf_logo_rgb24. Запускается один раз на компьютере, НЕ на плате — этот
# скрипт не имеет отношения к прошивке, он только готовит .raw файл,
# который потом встраивается через generate_inc_file_for_target(), как
# описано в инструкции к CMakeLists.txt.

# Требует Pillow: pip install --break-system-packages Pillow

# Использование:
#     python3 convert_logo.py esp32.jpg esp32_logo.raw --width 40 --height 30
# """
# import argparse
# from PIL import Image


# def main():
# 	parser = argparse.ArgumentParser(description=__doc__)
# 	parser.add_argument("input", help="Исходное изображение (JPEG/PNG/...)")
# 	parser.add_argument("output", help="Куда записать сырой RGB24-блок (.raw)")
# 	parser.add_argument("--width", type=int, required=True, help="Ширина логотипа в пикселях")
# 	parser.add_argument("--height", type=int, required=True, help="Высота логотипа в пикселях")
# 	args = parser.parse_args()

# 	img = Image.open(args.input).convert("RGB")
# 	img = img.resize((args.width, args.height))

# 	raw = img.tobytes()  # Pillow уже отдаёт построчно R,G,B,R,G,B,... — без правок.

# 	expected_len = args.width * args.height * 3
# 	assert len(raw) == expected_len, f"Ожидалось {expected_len} байт, получено {len(raw)}"

# 	with open(args.output, "wb") as f:
# 		f.write(raw)

# 	print(f"OK: {args.output} — {args.width}x{args.height}, {len(raw)} байт")
# 	print(f"Не забудьте задать в main.c:")
# 	print(f"  #define ESP32_LOGO_WIDTH  {args.width}")
# 	print(f"  #define ESP32_LOGO_HEIGHT {args.height}")


# if __name__ == "__main__":
# 	main()
