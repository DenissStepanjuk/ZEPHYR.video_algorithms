// Базовые примитивы ядра Zephyr (k_msleep, k_thread_create и т.д.):
#include <zephyr/kernel.h>
// Общее API устройств Zephyr:
#include <zephyr/device.h>
// API дисплеев Zephyr (display_blanking_off):
#include <zephyr/drivers/display.h>
// Макрос MIN(a, b), ARRAY_SIZE(a):
#include <zephyr/sys/util.h>
// Подсистема логирования Zephyr:
#include <zephyr/logging/log.h>

// Библиотека LVGL:
#include <lvgl.h>
#include <stdio.h>

#include "thread_ui.h"
#include "server.h" // extern checkbox_array[], lvgl_dirty_checkbox, ws_broadcast_checkbox()

// Регистрирует отдельный модуль логирования UI.
LOG_MODULE_REGISTER(UI, LOG_LEVEL_INF);

extern const struct device *const display_dev;

// Цветовая гамма приложения (1:1 с index.html)
#define COLOR_BG        0x520707
#define COLOR_TEXT      0xB8A830

// Структурное описание цвета палитры (совпадает 1:1 с массивом colors в HTML)
struct color_item {
    const char *name;
    uint32_t hex;
};

static const struct color_item palette[16] = {
    { "WHITE",     0xFFFFFF },
    { "BLACK",     0x000000 },
    { "RED",       0xE53935 },
    { "ORANGE",    0xFB8C00 },
    { "YELLOW",    0xFDD835 },
    { "GREEN",     0x43A047 },
    { "CYAN",      0x00ACC1 },
    { "BLUE",      0x1E88E5 },
    { "PURPLE",    0x8E24AA },
    { "PINK",      0xEC407A },
    { "BROWN",     0x795548 },
    { "GRAY",      0x9E9E9E },
    { "LIME",      0xCDDC39 },
    { "TEAL",      0x009688 },
    { "INDIGO",    0x3F51B5 },
    { "TURQUOISE", 0x40E0D0 }
};

// Ссылки на LVGL объекты для динамического обновления
static lv_obj_t *swatches[16];

/*
 * Визуальное обновление состояния чекбоксов палитры в UI LVGL на основе checkbox_array[16]
 */
static void update_checkbox_ui(void)
{
    for (uint8_t i = 0; i < ARRAY_SIZE(palette); i++) {
        if (checkbox_array[i]) {
            // Выделение свотча золотистой контурной рамкой (если выбран)
            lv_obj_set_style_outline_color(swatches[i], lv_color_hex(COLOR_TEXT), 0);
            lv_obj_set_style_outline_width(swatches[i], 2, 0);
            lv_obj_set_style_outline_pad(swatches[i], 2, 0);
            lv_obj_set_style_outline_opa(swatches[i], LV_OPA_COVER, 0);
        } else {
            // Снятие выделения
            lv_obj_set_style_outline_width(swatches[i], 0, 0);
        }
    }
}

/*
 * Обработчик нажатия на свотч палитры (Touch Panel)
 */
static void swatch_event_cb(lv_event_t *e)
{
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    if (idx >= ARRAY_SIZE(palette)) {
        return;
    }

    // Инвертируем состояние выбранного чекбокса
    checkbox_array[idx] = !checkbox_array[idx];
    update_checkbox_ui();

    LOG_INF("LVGL Touch: Checkbox[%d] -> %s", (int)idx, checkbox_array[idx] ? "true" : "false");

    // Отправка нового состояния в WebSocket
    ws_broadcast_checkbox((int)idx, checkbox_array[idx]);
}

/*
 * Построение интерфейса: Заголовок + Зона видео + Панель палитры 4x4
 */
static void create_ui(void)
{
    lv_obj_t *screen = lv_screen_active();

    // Фон экрана - тёмно-бордовый
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen,
                            LV_FLEX_ALIGN_START,
                            LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen, 8, 0);
    lv_obj_set_style_pad_top(screen, 6, 0);

    // 1. Заголовок "ZEPHYR STREAM"
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "ZEPHYR STREAM");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_letter_space(title, 2, 0);

    // 2. Отступ под зону прямого вывода видеокадра
    lv_obj_t *top_spacer = lv_obj_create(screen);
    lv_obj_remove_style_all(top_spacer);
    lv_obj_set_size(top_spacer, lv_pct(100), lv_pct(26));

    // 3. Карточка палитры (стиль .panel из CSS)
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_width(panel, lv_pct(90));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_20, 0);            // background: rgba(0,0,0,.18)
    lv_obj_set_style_border_color(panel, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_30, 0);         // border: 1px solid rgba(184,168,48,.25)
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_style_pad_row(panel, 6, 0);

    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
                            LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER);

    // Заголовок карточки "Color palette"
    lv_obj_t *panel_title = lv_label_create(panel);
    lv_label_set_text(panel_title, "COLOR PALETTE");
    lv_obj_set_style_text_font(panel_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(panel_title, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_letter_space(panel_title, 1, 0);

    // Контейнер-сетка 4x4 для свотчей
    lv_obj_t *grid = lv_obj_create(panel);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 136, 136); // Компактная квадратная сетка
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_SPACE_EVENLY);
    lv_obj_set_style_pad_column(grid, 4, 0);
    lv_obj_set_style_pad_row(grid, 4, 0);

    // Создание 16 кнопок-свотчей
    for (uint32_t i = 0; i < 16; i++) {
        lv_obj_t *swatch = lv_obj_create(grid);
        swatches[i] = swatch;

        lv_obj_set_size(swatch, 28, 28);
        lv_obj_set_style_radius(swatch, 5, 0);
        lv_obj_set_style_bg_color(swatch, lv_color_hex(palette[i].hex), 0);
        lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(swatch, lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_opa(swatch, LV_OPA_40, 0);
        lv_obj_set_style_border_width(swatch, 1, 0);
        lv_obj_clear_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(swatch, LV_OBJ_FLAG_CLICKABLE);

        // Передаем индекс цвета через user_data
        lv_obj_add_event_cb(swatch, swatch_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }

    // Установка начального состояния всех свотчей (все выбраны по умолчанию)
    update_checkbox_ui();
}

/* ------------------------- UI-поток (ui_thread) ------------------------- */

K_THREAD_STACK_DEFINE(ui_thread_stack, UI_THREAD_STACK_SIZE);
struct k_thread ui_thread_data;

void ui_thread_entry(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    int ret;

    LOG_INF("LVGL Color Palette, display: %s", display_dev->name);

    if (!device_is_ready(display_dev)) {
        LOG_ERR("Device %s not found. Aborting sample.", display_dev->name);
        return;
    }

    create_ui();

    ret = display_blanking_off(display_dev);
    if (ret < 0 && ret != -ENOSYS) {
        LOG_ERR("Failed to turn blanking off (error %d)", ret);
        return;
    }

    while (1) {
        // Проверяем флаг обновления, пришедший по WebSocket от веб-клиента
        if (atomic_cas(&lvgl_dirty_checkbox, 1, 0)) {
            update_checkbox_ui();
            LOG_INF("LVGL UI synced from WS: checkbox_array updated");
        }

        uint32_t sleep_ms = lv_timer_handler();
        k_msleep(MIN(sleep_ms, INT32_MAX));
    }
}