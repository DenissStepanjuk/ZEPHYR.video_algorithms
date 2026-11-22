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

// Поток выделенный под пользовательский интерфейс на базе LVGL.
#include "thread_ui.h"

// Логика WebSocket-сервера.
#include "server.h"

// Регистрирует отдельный модуль логирования UI.
LOG_MODULE_REGISTER(UI, LOG_LEVEL_INF);

// display_dev определён (без static) в main.c, там же, где находятся
// связанные с ним проверки devicetree (#error, если нет zephyr,display).
extern const struct device *const display_dev;
// Внешнее состояние для обновления индикатора
extern volatile bool state;

// Цветовая гамма приложения (1:1 с index.html)
#define COLOR_BG        0x520707
#define COLOR_TEXT      0xB8A830

// Ссылка на LVGL-кнопку SET и светодиодный индикатор
static lv_obj_t *btn_set_lvgl;
static lv_obj_t *led_indicator;

/*
 * Кратковременная подсветка кнопки золотистой контурной рамкой
 * (аналог CSS .sent / box-shadow из index.html при локальном тапе).
 */
static void set_btn_flash(bool on)
{
    if (!btn_set_lvgl) {
        return;
    }

    if (on) {
        lv_obj_set_style_outline_color(btn_set_lvgl, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_outline_width(btn_set_lvgl, 2, 0);
        lv_obj_set_style_outline_pad(btn_set_lvgl, 2, 0);
        lv_obj_set_style_outline_opa(btn_set_lvgl, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_outline_width(btn_set_lvgl, 0, 0);
    }
}

/*
 * Таймер LVGL, снимающий подсветку через ~180мс (как setTimeout в HTML).
 */
static void flash_timer_cb(lv_timer_t *timer)
{
    set_btn_flash(false);
    lv_timer_del(timer);
}

/*
 * Обработчик нажатия на кнопку SET (Touch Panel).
 * Устанавливает btn_set в true и коротко подсвечивает кнопку.
 */
static void btn_set_event_cb(lv_event_t *e)
{
    btn_set = true;

    set_btn_flash(true);
    lv_timer_create(flash_timer_cb, 180, NULL);

    LOG_INF("LVGL Touch: SET pressed -> btn_set = true");
}

/*
 * Обновление цвета и стилей светодиодного индикатора в зависимости от state
 */
static void update_led_state(bool active)
{
    if (!led_indicator) {
        return;
    }

    if (active) {
        // Зелёный (state = true)
        lv_led_set_color(led_indicator, lv_color_hex(0x22c55e));
        lv_obj_set_style_border_color(led_indicator, lv_color_hex(0x14532d), 0);
        lv_obj_set_style_border_width(led_indicator, 2, 0);
        lv_obj_set_style_outline_color(led_indicator, lv_color_hex(0x22c55e), 0);
        lv_obj_set_style_outline_width(led_indicator, 6, 0);
        lv_obj_set_style_outline_pad(led_indicator, 4, 0);
        lv_obj_set_style_outline_opa(led_indicator, 80, 0);
        lv_obj_set_style_shadow_color(led_indicator, lv_color_hex(0x22c55e), 0);
        lv_obj_set_style_shadow_width(led_indicator, 20, 0);
        lv_obj_set_style_shadow_opa(led_indicator, 200, 0);
    } else {
        // Красный (state = false) - изначально и при движении
        lv_led_set_color(led_indicator, lv_color_hex(0xef4444));
        lv_obj_set_style_border_color(led_indicator, lv_color_hex(0x7f1d1d), 0);
        lv_obj_set_style_border_width(led_indicator, 2, 0);
        lv_obj_set_style_outline_color(led_indicator, lv_color_hex(0xef4444), 0);
        lv_obj_set_style_outline_width(led_indicator, 6, 0);
        lv_obj_set_style_outline_pad(led_indicator, 4, 0);
        lv_obj_set_style_outline_opa(led_indicator, 80, 0);
        lv_obj_set_style_shadow_color(led_indicator, lv_color_hex(0xef4444), 0);
        lv_obj_set_style_shadow_width(led_indicator, 20, 0);
        lv_obj_set_style_shadow_opa(led_indicator, 200, 0);
    }
}

/*
 * Построение интерфейса: Заголовок + Зона видео + Панель с кнопкой SET и LED индикатором
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

    // 3. Карточка панели (стиль .panel из CSS)
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_width(panel, lv_pct(90));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_20, 0);            // background: rgba(0,0,0,.18)
    lv_obj_set_style_border_color(panel, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_30, 0);        // border: 1px solid rgba(184,168,48,.25)
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_style_pad_row(panel, 6, 0);

    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // Заголовок карточки "REFERENCE FRAME" (соответствует .panel-title в HTML)
    lv_obj_t *panel_title = lv_label_create(panel);
    lv_label_set_text(panel_title, "REFERENCE FRAME");
    lv_obj_set_style_text_font(panel_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(panel_title, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_letter_space(panel_title, 1, 0);

    // Кнопка SET (аналог #BTN_SET в HTML: золотистый фон, бордовый текст)
    lv_obj_t *btn = lv_button_create(panel);
    btn_set_lvgl = btn;

    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_pad_all(btn, 10, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, btn_set_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "SET");
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(btn_label, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_text_letter_space(btn_label, 1, 0);
    lv_obj_center(btn_label);

    // Цветовой LED индикатор под кнопкой (аналог html аналога)
    lv_obj_t *led_1 = lv_led_create(panel);
    led_indicator = led_1;
    lv_obj_set_size(led_1, 50, 50);
    
    // Изначально устанавливаем индикатор в красный цвет (state = false)
    update_led_state(false);
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

    LOG_INF("LVGL SET button, display: %s", display_dev->name);

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
        // Проверяем флаг обновления, пришедший из главного цикла обработки кадров
        if (atomic_cas(&lvgl_dirty_state, 1, 0)) {
            // Изменяем индикатор на дисплее.
            update_led_state(state);
            // Вызываем ws_broadcast_state() чтобы отобразить изменение индикатора на вебстраничке.
            ws_broadcast_state();
            LOG_INF("LVGL UI synced state: state = %d", state);
        }

        uint32_t sleep_ms = lv_timer_handler();
        k_msleep(MIN(sleep_ms, INT32_MAX));
    }
}

