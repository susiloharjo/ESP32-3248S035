#include <Arduino.h>
#include "LGFX_ESP32_3248S035C.hpp"  // header working dari project sebelumnya
#include <lvgl.h>

LGFX tft;  // dari header LGFX custom kamu

// LVGL buffer & driver
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1;
static lv_disp_drv_t disp_drv;

// Touch input driver
static lv_indev_drv_t indev_drv;

// Flush callback buat LVGL
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  tft.pushImage(area->x1, area->y1, w, h, (lgfx::rgb565_t*)&color_p->full);
  lv_disp_flush_ready(disp);
}

// Touch read callback (contoh pakai GT911/FT6x06)
void my_touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    // Contoh pakai GT911/FT6x06
    if (tft.getTouch(&data->point.x, &data->point.y)) {
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}


// Button event handler
static void btn_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *btn = lv_event_get_target(e);

  if (code == LV_EVENT_CLICKED) {
    lv_obj_t *label = (lv_obj_t *)lv_event_get_user_data(e);
    lv_label_set_text(label, "Button Clicked!");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Init LCD with LVGL...");

  // init TFT
  tft.init();
  tft.setRotation(1);
  tft.setColorDepth(16);
  tft.fillScreen(TFT_BLACK);

  // init LVGL
  lv_init();

  buf1 = (lv_color_t*)heap_caps_malloc(sizeof(lv_color_t) * tft.width() * 40, MALLOC_CAP_DMA);
  if (!buf1) {
    Serial.println("LVGL buffer alloc FAILED!");
    return;
  }

  lv_disp_draw_buf_init(&draw_buf, buf1, NULL, tft.width() * 40);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = tft.width();
  disp_drv.ver_res = tft.height();
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  // --- Input device (touch) ---
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touch_read;
  lv_indev_t *touch_indev = lv_indev_drv_register(&indev_drv);

  // --- test UI ---
  lv_obj_t *label = lv_label_create(lv_scr_act());
  lv_label_set_text(label, "Hello LVGL on CYD 3.5");
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);

  lv_obj_t *btn = lv_btn_create(lv_scr_act());
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
  lv_obj_t *btn_label = lv_label_create(btn);
  lv_label_set_text(btn_label, "Click me");
  lv_obj_center(btn_label);

  lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, label);

  Serial.println("Setup LVGL done");
}

void loop() {
  lv_timer_handler();  // jalankan LVGL task
  delay(5);
}
