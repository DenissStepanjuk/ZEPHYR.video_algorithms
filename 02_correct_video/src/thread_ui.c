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

// Палитра - должна совпадать 1:1 с index.html (background:#520707; color:#b8a830;).
#define COLOR_BG        0x520707
#define COLOR_TEXT      0xB8A830

struct slider_ctx {
    lv_obj_t *slider;
    lv_obj_t *label;
    const char *name;
    uint32_t accent_hex;        // NEW: нужен и для стилей, и для recolor-точки в тексте
    const char *ws_type;
    volatile float *value_out;
    atomic_t *dirty_flag;
};

// Статический пул контекстов слайдеров. Размер ограничен -
// create_slider_ui() проверяет переполнение через ARRAY_SIZE().
// Статический пул контекстов слайдеров.
static struct slider_ctx slider_ctxs[10];
static uint8_t slider_count;

// Актуальные значения слайдеров - определение (объявлены extern в
// ui_thread.h, см. подробный комментарий там).
volatile float slider_value_1;
volatile float slider_value_2;
volatile float slider_value_3;



/*
 * Собрать текст подписи в формате index.html: цветная точка (recolor-тег
 * LVGL) + "NAME: X.X", той же цветовой точкой, что и .dot в CSS у
 * соответствующего слайдера.
 */
static void format_slider_label(char *buf, size_t buf_size,
                                 const char *name, uint32_t accent_hex,
                                 float value)
{
    // "#RRGGBB ●# NAME: X.X" - lv_label с включённым recolor красит текст
    // между # #-парами в указанный цвет, здесь - только символ точки.
    snprintf(buf, buf_size, "#%06X \xE2\x97\x8F# %s: %.1f",
              (unsigned int)accent_hex, name, (double)value);
}
/*
 * Обработчик события слайдера.
 * Вызывается LVGL при событии LV_EVENT_VALUE_CHANGED, то есть каждый раз,
 * когда пользователь двигает "бегунок" пальцем по сенсорной панели.
 * Выполняется в контексте ui_thread, т.к. именно там крутится
 * lv_timer_handler(), который и генерирует это событие.
 */
static void slider_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    struct slider_ctx *ctx = lv_event_get_user_data(e);

    int32_t value = lv_slider_get_value(slider);
    float value_f = value / 10.0f;

    char buf[48];
    format_slider_label(buf, sizeof(buf), ctx->name, ctx->accent_hex, value_f);
    lv_label_set_text(ctx->label, buf);

    *ctx->value_out = value_f;

    LOG_INF("%s = %.1f", ctx->name, (double)value_f);

    ws_broadcast_slider(ctx->ws_type, value_f);
}

/*
 * Создаёт один "блок" слайдера: подпись сверху + слайдер снизу,
 * упакованные в отдельный контейнер-строку внутри parent.
 * Такой блок ведёт себя как единое целое и легко центрируется
 * flex-контейнером родителя и по X, и по Y.
 *
 * accent_color задаёт цвет индикатора/бегунка слайдера и текста подписи,
 * чтобы каждый слайдер визуально отличался.
 */
static void create_slider_ui(lv_obj_t *parent,
                              const char *name,
                              const char *ws_type,
                              int32_t min, int32_t max, int32_t initial,
                              uint32_t accent_hex,        // было lv_color_t accent_color
                              volatile float *value_out,
                              atomic_t *dirty_flag)
{
    if (slider_count >= ARRAY_SIZE(slider_ctxs)) {
        LOG_ERR("Slider limit (%d) exceeded, \"%s\" skipped",
                (int)ARRAY_SIZE(slider_ctxs), name);
        return;
    }

    lv_color_t accent_color = lv_color_hex(accent_hex);

    struct slider_ctx *ctx = &slider_ctxs[slider_count++];
    ctx->name = name;
    ctx->ws_type = ws_type;
    ctx->accent_hex = accent_hex;
    ctx->value_out = value_out;
    ctx->dirty_flag = dirty_flag;

    // Контейнер-строка под пару "подпись + слайдер" - без своего фона,
    // просто прозрачная строка внутри панели (как .slider-row в CSS).
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(90), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row,
                           LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(row, 4, 0);

    ctx->label = lv_label_create(row);
    lv_label_set_recolor(ctx->label, true);   // включаем разбор #RRGGBB text# тегов
    lv_obj_set_style_text_font(ctx->label, &lv_font_montserrat_14, 0);
    // Основной цвет подписи - золотистый, как .label в CSS (color:#b8a830);
    // точка перед текстом красится отдельно через recolor-тег.
    lv_obj_set_style_text_color(ctx->label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_align(ctx->label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(ctx->label, 1, 0);

    char buf[48];
    format_slider_label(buf, sizeof(buf), name, accent_hex, initial / 10.0f);
    lv_label_set_text(ctx->label, buf);

    *ctx->value_out = initial / 10.0f;

    lv_obj_t *slider = lv_slider_create(row);
    ctx->slider = slider;

    lv_obj_set_size(slider, lv_pct(100), 16);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, initial, LV_ANIM_OFF);

    // Трек - приглушённый золотисто-полупрозрачный, как .slider-row
    // input[type=range]::-webkit-slider-runnable-track в CSS
    // (background: rgba(184,168,48,.3)).
    lv_obj_set_style_bg_color(slider, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_30, LV_PART_MAIN);

    // Заполненная часть (индикатор) и бегунок - акцентный цвет слайдера.
    lv_obj_set_style_bg_color(slider, accent_color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, accent_color, LV_PART_KNOB);
    // Тёмная окантовка бегунка в тон фону страницы - как .thumb border
    // в CSS (border: 1px solid #520707), а не белая, как в дефолтной теме.
    lv_obj_set_style_border_color(slider, lv_color_hex(COLOR_BG), LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_style_outline_color(slider, accent_color,
                                    LV_PART_KNOB | LV_STATE_FOCUSED);

    lv_obj_add_event_cb(slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, ctx);
}


/*
 * Строит весь UI: заголовок + панель-карточку с тремя
 * центрированными слайдерами.
 */
static void create_ui(void)
{
    lv_obj_t *screen = lv_screen_active();

    // Фон экрана - тот же тёмно-бордовый, что и body в index.html.
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen,
                           LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen, 12, 0);
    lv_obj_set_style_pad_top(screen, 8, 0);

    // Заголовок - аналог <h1>Zephyr Stream</h1>: главный элемент, вынесен
    // в самый верх экрана. Раз он рисуется через LVGL flex-layout (а не
    // через прямой display_write, как видео), он безопасно "потеснит"
    // top_spacer/panel вниз на свою высоту, но не заденет физические
    // пиксели видео (те пишутся по фиксированным абсолютным координатам
    // x=160,y=100 в app_display_frame(), в обход LVGL целиком).
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "ZEPHYR STREAM");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_letter_space(title, 2, 0);

    // Пустой отступ - зарезервирован под область видео, LVGL сюда ничего
    // не рисует (remove_style_all -> нет фона), display_write() пишет
    // напрямую в этот же физический прямоугольник экрана.
    lv_obj_t *top_spacer = lv_obj_create(screen);
    lv_obj_remove_style_all(top_spacer);
    lv_obj_set_size(top_spacer, lv_pct(100), lv_pct(28));

    // Панель-карточка со слайдерами - полупрозрачная тёмная плашка с
    // золотистой рамкой, точь-в-точь как .panel в CSS:
    //   background: rgba(0,0,0,.18);
    //   border: 1px solid rgba(184,168,48,.25);
    //   border-radius: 10px;
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_width(panel, lv_pct(90));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_20, 0);            // ~.18 rgba alpha
    lv_obj_set_style_border_color(panel, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_30, 0);         // ~.25 rgba alpha
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);               // теней в CSS-варианте нет
    lv_obj_set_style_pad_all(panel, 14, 0);
    lv_obj_set_style_pad_row(panel, 18, 0);

    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
                           LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);

    // Цвета акцентов те же hex, что и в CSS: --accent у каждого .slider-row.
    create_slider_ui(panel, "RED BALANCE", "red_balance",
                      0, 20, (int32_t)(red_balance * 10.0f + 0.5f),
                      0xE85D5D, &red_balance, &lvgl_dirty_red);
    create_slider_ui(panel, "BLUE BALANCE", "blue_balance",
                      0, 20, (int32_t)(blue_balance * 10.0f + 0.5f),
                      0x3FA37E, &blue_balance, &lvgl_dirty_blue);
    create_slider_ui(panel, "GAMMA LEVEL", "gamma_level",
                      0, 20, (int32_t)(gamma_level * 10.0f + 0.5f),
                      0x4A90D9, &gamma_level, &lvgl_dirty_gamma);
}
/* ------------------------- UI-поток (ui_thread) ------------------------- */

// Область стека потока выделяется статически на этапе компиляции.
// main.c обращается к этому же стеку через K_THREAD_STACK_DECLARE()
// в ui_thread.h.
K_THREAD_STACK_DEFINE(ui_thread_stack, UI_THREAD_STACK_SIZE);

// Структура данных потока (используется ядром Zephyr для планирования).
// Не static - main.c передаёт указатель на неё в k_thread_create().
struct k_thread ui_thread_data;

/*
 * Точка входа UI-потока.
 * Здесь выполняется всё, что связано с дисплеем: инициализация устройства,
 * построение интерфейса (create_ui), включение вывода на экран и главный
 * цикл LVGL (обработка touch-событий слайдеров через slider_event_cb,
 * вывод значений в терминал через LOG_INF и перерисовка экрана).
 * Ни один другой поток LVGL-объекты не трогает, поэтому дополнительная
 * блокировка (мьютекс) здесь не нужна.
 */
void ui_thread_entry(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    int ret;

    LOG_INF("LVGL slider sample, display: %s", display_dev->name);

    if (!device_is_ready(display_dev)) {
        LOG_ERR("Device %s not found. Aborting sample.", display_dev->name);
        return;
    }

    // Строим UI один раз при старте.
    create_ui();

    // Включаем реальный вывод изображения на дисплей.
    ret = display_blanking_off(display_dev);
    if (ret < 0 && ret != -ENOSYS) {
        LOG_ERR("Failed to turn blanking off (error %d)", ret);
        return;
    }

    // Главный цикл LVGL: lv_timer_handler() сам обрабатывает touch-события
    // (пришедшие через LVGL indev от touch_dev) и сам перерисовывает экран.
    // Возвращает, через сколько мс его нужно вызвать снова.
    while (1) {


        // Подхватить значения, пришедшие с веб-страницы (см. lvgl_dirty_*).
        for (int i = 0; i < slider_count; i++) {
            struct slider_ctx *ctx = &slider_ctxs[i];

            if (atomic_cas(ctx->dirty_flag, 1, 0)) {
                int32_t raw = (int32_t)(*ctx->value_out * 10.0f + 0.5f);

                lv_slider_set_value(ctx->slider, raw, LV_ANIM_OFF);

                char buf[48];
                format_slider_label(buf, sizeof(buf), ctx->name, ctx->accent_hex, *ctx->value_out);
                lv_label_set_text(ctx->label, buf);
            }
        }

        uint32_t sleep_ms = lv_timer_handler();

        k_msleep(MIN(sleep_ms, INT32_MAX));
    }
}

