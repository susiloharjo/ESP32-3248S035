// ESP32-3248S035C - Demo hub (Gemini Chatbot + Digital Andon System)
//
// One firmware, two demos, picked from a menu shown right after touch
// calibration. Both demos are otherwise unmodified ports of
// examples/gemini-chatbot/src/main.cpp and
// examples/andon-system/firmware/src/main.cpp - each app's code lives in
// its own C++ namespace (chatapp / andon) so the two, which independently
// grew near-identical globals (chat_* vs g_andon, screen state, etc.) don't
// collide when compiled into one translation unit. The GT911 driver, touch
// calibration, and lvgl/TFT_eSPI plumbing are shared (both apps' copies
// were already byte-for-byte the same logic) rather than compiled twice.
//
// See the size math in the chat transcript: even a naive sum of both
// standalone builds (chat ~1.17MB flash / 97KB RAM, andon ~0.8MB flash /
// 72KB RAM) fits comfortably under huge_app.csv's ~3MB app partition and
// the chip's 320KB RAM - this file uses noticeably less than that sum
// since the shared driver/framework code is now only compiled once.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h> // NVS storage for the on-device WiFi manager
#include "esp_system.h" // esp_reset_reason() - TEMP/DEBUG, see setup()

#include <lvgl.h>
#include <TFT_eSPI.h>

#include "secrets.h" // Gemini API key + legacy WiFi fallback - gitignored


//------------------------------------------------------------------------------
// GT911 touch controller - bit-banged I2C driver
//------------------------------------------------------------------------------
// I2C commands
#define GT_CMD_WR           0XBA
#define GT_CMD_RD           0XBB

#define GT911_MAX_WIDTH     320   // touch panel's native (portrait) width
#define GT911_MAX_HEIGHT    480   // touch panel's native (portrait) height

// GT911 registers
#define GT_CTRL_REG         0X8040
#define GT_CFGS_REG         0X8047
#define GT_CHECK_REG        0X80FF
#define GT_PID_REG          0X8140
#define GT_GSTID_REG        0X814E
#define GT911_READ_XY_REG   0x814E
#define CT_MAX_TOUCH        5     // max simultaneous touch points GT911 supports

int IIC_SCL = 32;
int IIC_SDA = 33;
int IIC_RST = 25;

#define IIC_SCL_0  digitalWrite(IIC_SCL, LOW)
#define IIC_SCL_1  digitalWrite(IIC_SCL, HIGH)
#define IIC_SDA_0  digitalWrite(IIC_SDA, LOW)
#define IIC_SDA_1  digitalWrite(IIC_SDA, HIGH)
#define IIC_RST_0  digitalWrite(IIC_RST, LOW)
#define IIC_RST_1  digitalWrite(IIC_RST, HIGH)
#define READ_SDA   digitalRead(IIC_SDA)

typedef struct {
  uint8_t Touch;
  uint8_t TouchpointFlag;
  uint8_t TouchCount;
  uint8_t Touchkeytrackid[CT_MAX_TOUCH];
  uint16_t X[CT_MAX_TOUCH];
  uint16_t Y[CT_MAX_TOUCH];
  uint16_t S[CT_MAX_TOUCH];
} GT911_Dev;
GT911_Dev Dev_Now, Dev_Backup;
bool touched = 0; // no touch interrupt wired up - polled flag instead

void delay_us(unsigned int xus) {
  for (; xus > 1; xus--);
}

void SDA_IN(void)  { pinMode(IIC_SDA, INPUT); }
void SDA_OUT(void) { pinMode(IIC_SDA, OUTPUT); }

void IIC_Init(void) {
  pinMode(IIC_SDA, OUTPUT);
  pinMode(IIC_SCL, OUTPUT);
  pinMode(IIC_RST, OUTPUT);
  IIC_SCL_1;
  IIC_SDA_1;
}

void IIC_Start(void) {
  SDA_OUT();
  IIC_SDA_1;
  IIC_SCL_1;
  delay_us(4);
  IIC_SDA_0; // START: while CLK is high, DATA goes high->low
  delay_us(4);
  IIC_SCL_0;
}

void IIC_Stop(void) {
  SDA_OUT();
  IIC_SCL_0;
  IIC_SDA_0;
  delay_us(4);
  IIC_SCL_1;
  IIC_SDA_1; // STOP: while CLK is high, DATA goes low->high
  delay_us(4);
}

// Returns 0 on ACK, 1 on timeout/NACK.
uint8_t IIC_Wait_Ack(void) {
  uint8_t ucErrTime = 0;
  SDA_IN();
  IIC_SDA_1; delay_us(1);
  IIC_SCL_1; delay_us(1);
  while (READ_SDA) {
    ucErrTime++;
    if (ucErrTime > 250) {
      IIC_Stop();
      return 1;
    }
  }
  IIC_SCL_0;
  return 0;
}

void IIC_Ack(void) {
  IIC_SCL_0;
  SDA_OUT();
  IIC_SDA_0;
  delay_us(2);
  IIC_SCL_1;
  delay_us(2);
  IIC_SCL_0;
}

void IIC_NAck(void) {
  IIC_SCL_0;
  SDA_OUT();
  IIC_SDA_1;
  delay_us(2);
  IIC_SCL_1;
  delay_us(2);
  IIC_SCL_0;
}

void IIC_Send_Byte(uint8_t txd) {
  uint8_t t;
  SDA_OUT();
  IIC_SCL_0;
  for (t = 0; t < 8; t++) {
    if ((txd & 0x80) >> 7) IIC_SDA_1;
    else IIC_SDA_0;
    txd <<= 1;
    delay_us(2);
    IIC_SCL_1;
    delay_us(2);
    IIC_SCL_0;
    delay_us(2);
  }
}

// ack=1 sends ACK after the read byte, ack=0 sends NACK (last byte of a read).
uint8_t IIC_Read_Byte(unsigned char ack) {
  unsigned char i, receive = 0;
  SDA_IN();
  for (i = 0; i < 8; i++) {
    IIC_SCL_0;
    delay_us(2);
    IIC_SCL_1;
    receive <<= 1;
    if (READ_SDA) receive++;
    delay_us(1);
  }
  if (!ack) IIC_NAck();
  else IIC_Ack();
  return receive;
}

// Bit-banged I2C timing (delay_us() is a plain busy-wait, not RTOS-aware) can
// get corrupted if a higher-priority task preempts mid-transaction. Wrapping
// each full transaction in a critical section protects it; transactions here
// are short, so this stays cheap.
static portMUX_TYPE gt911_i2c_mux = portMUX_INITIALIZER_UNLOCKED;

uint8_t GT911_WR_Reg(uint16_t reg, uint8_t *buf, uint8_t len) {
  portENTER_CRITICAL(&gt911_i2c_mux);
  uint8_t ret = 0;
  IIC_Start();
  IIC_Send_Byte(GT_CMD_WR);
  IIC_Wait_Ack();
  IIC_Send_Byte(reg >> 8);
  IIC_Wait_Ack();
  IIC_Send_Byte(reg & 0XFF);
  IIC_Wait_Ack();
  for (uint8_t i = 0; i < len; i++) {
    IIC_Send_Byte(buf[i]);
    ret = IIC_Wait_Ack();
    if (ret) break;
  }
  IIC_Stop();
  portEXIT_CRITICAL(&gt911_i2c_mux);
  return ret;
}

void GT911_RD_Reg(uint16_t reg, uint8_t *buf, uint8_t len) {
  portENTER_CRITICAL(&gt911_i2c_mux);
  IIC_Start();
  IIC_Send_Byte(GT_CMD_WR);
  IIC_Wait_Ack();
  IIC_Send_Byte(reg >> 8);
  IIC_Wait_Ack();
  IIC_Send_Byte(reg & 0XFF);
  IIC_Wait_Ack();
  IIC_Start();
  IIC_Send_Byte(GT_CMD_RD);
  IIC_Wait_Ack();
  for (uint8_t i = 0; i < len; i++) {
    buf[i] = IIC_Read_Byte(i == (len - 1) ? 0 : 1);
  }
  IIC_Stop();
  portEXIT_CRITICAL(&gt911_i2c_mux);
}

// mode: 0 = don't persist to GT911 flash, 1 = persist.
uint8_t GT911_Send_Cfg(uint8_t mode) {
  uint8_t buf[2];
  buf[0] = 0;
  buf[1] = mode;
  GT911_WR_Reg(GT_CHECK_REG, buf, 2);
  return 0;
}

void GT911_Scan(void) {
  uint8_t buf[41];
  uint8_t Clearbuf = 0;
  uint8_t i;

  Dev_Now.Touch = 0;
  GT911_RD_Reg(GT911_READ_XY_REG, buf, 1);

  if ((buf[0] & 0x80) == 0x00) {
    touched = 0;
    GT911_WR_Reg(GT911_READ_XY_REG, (uint8_t *)&Clearbuf, 1);
    return;
  }

  touched = 1;
  Dev_Now.TouchpointFlag = buf[0];
  Dev_Now.TouchCount = buf[0] & 0x0f;
  if (Dev_Now.TouchCount > 5) {
    touched = 0;
    GT911_WR_Reg(GT911_READ_XY_REG, (uint8_t *)&Clearbuf, 1);
    return;
  }
  GT911_RD_Reg(GT911_READ_XY_REG + 1, &buf[1], Dev_Now.TouchCount * 8);
  GT911_WR_Reg(GT911_READ_XY_REG, (uint8_t *)&Clearbuf, 1);

  for (i = 0; i < CT_MAX_TOUCH; i++) {
    uint8_t o = i * 8;
    Dev_Now.Touchkeytrackid[i] = buf[1 + o];
    Dev_Now.X[i] = ((uint16_t)buf[3 + o] << 8) + buf[2 + o];
    Dev_Now.Y[i] = ((uint16_t)buf[5 + o] << 8) + buf[4 + o];
    Dev_Now.S[i] = ((uint16_t)buf[7 + o] << 8) + buf[6 + o];
  }

  for (i = 0; i < Dev_Now.TouchCount; i++) {
    if (Dev_Now.Y[i] < 0 || Dev_Now.Y[i] > 480 ||
        Dev_Now.X[i] < 0 || Dev_Now.X[i] > 320) {
      touched = 0;
    } else {
      Dev_Backup.X[i] = Dev_Now.X[i];
      Dev_Backup.Y[i] = Dev_Now.Y[i];
      Dev_Backup.TouchCount = Dev_Now.TouchCount;
    }
  }
  if (Dev_Now.TouchCount == 0) touched = 0;
}

const uint8_t GT9111_CFG_TBL[] = {
  0X60, 0X40, 0X01, 0XE0, 0X01, 0X05, 0X35, 0X00, 0X02, 0X08,
  0X1E, 0X08, 0X50, 0X3C, 0X0F, 0X05, 0X00, 0X00, 0XFF, 0X67,
  0X50, 0X00, 0X00, 0X18, 0X1A, 0X1E, 0X14, 0X89, 0X28, 0X0A,
  0X30, 0X2E, 0XBB, 0X0A, 0X03, 0X00, 0X00, 0X02, 0X33, 0X1D,
  0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X32, 0X00, 0X00,
  0X2A, 0X1C, 0X5A, 0X94, 0XC5, 0X02, 0X07, 0X00, 0X00, 0X00,
  0XB5, 0X1F, 0X00, 0X90, 0X28, 0X00, 0X77, 0X32, 0X00, 0X62,
  0X3F, 0X00, 0X52, 0X50, 0X00, 0X52, 0X00, 0X00, 0X00, 0X00,
  0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00,
  0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X0F,
  0X0F, 0X03, 0X06, 0X10, 0X42, 0XF8, 0X0F, 0X14, 0X00, 0X00,
  0X00, 0X00, 0X1A, 0X18, 0X16, 0X14, 0X12, 0X10, 0X0E, 0X0C,
  0X0A, 0X08, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00,
  0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00,
  0X00, 0X00, 0X29, 0X28, 0X24, 0X22, 0X20, 0X1F, 0X1E, 0X1D,
  0X0E, 0X0C, 0X0A, 0X08, 0X06, 0X05, 0X04, 0X02, 0X00, 0XFF,
  0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00,
  0X00, 0XFF, 0XFF, 0XFF, 0XFF, 0XFF, 0XFF, 0XFF, 0XFF, 0XFF,
  0XFF, 0XFF, 0XFF, 0XFF,
};

uint8_t GT9111_Send_Cfg(uint8_t mode) {
  uint8_t buf[2];
  uint8_t i = 0;
  buf[0] = 0;
  buf[1] = mode;
  for (i = 0; i < sizeof(GT9111_CFG_TBL); i++) buf[0] += GT9111_CFG_TBL[i];
  buf[0] = (~buf[0]) + 1;
  GT911_WR_Reg(GT_CFGS_REG, (uint8_t *)GT9111_CFG_TBL, sizeof(GT9111_CFG_TBL));
  GT911_WR_Reg(GT_CHECK_REG, buf, 2);
  return 0;
}

// Resets the GT911 and, if it reports a config version older than 0x60,
// pushes the known-good config table above.
void gt911_init() {
  uint8_t buf[4];

  pinMode(IIC_SDA, OUTPUT);
  pinMode(IIC_SCL, OUTPUT);
  pinMode(IIC_RST, OUTPUT);

  delay(50);
  digitalWrite(IIC_RST, LOW);
  delay(10);
  digitalWrite(IIC_RST, HIGH);
  delay(50);

  GT911_RD_Reg(GT_PID_REG, buf, 4);
  Serial.printf("TouchPad_ID: %d,%d,%d\r\n", buf[0], buf[1], buf[2]);
  buf[0] = 0x02;

  GT911_WR_Reg(GT_CTRL_REG, buf, 1);
  GT911_RD_Reg(GT_CFGS_REG, buf, 1);
  Serial.printf("Config version: 0x%X\r\n", buf[0]);
  if (buf[0] < 0X60) {
    GT9111_Send_Cfg(1);
  }

  delay(10);
  buf[0] = 0x00;
  GT911_WR_Reg(GT_CTRL_REG, buf, 1);
}

//------------------------------------------------------------------------------
// Touch -> screen mapping and on-device calibration
//------------------------------------------------------------------------------
// GT911 reports raw touch coordinates in the panel's native PORTRAIT
// orientation, but the display runs in LANDSCAPE (tft.setRotation(1),
// 480x320 - see setup()), so raw X/Y must be swapped and rescaled to match
// what's drawn on screen: screenX = f(rawY), screenY = f(rawX) (90 deg
// rotation, confirmed on-device).
//
// The scale/offset for that mapping is derived at boot by
// runTouchCalibration(), which shows two on-screen targets and captures the
// actual raw reading at each tap - see examples/gemini-chatbot/src/main.cpp
// for the debugging history behind this approach.
struct TouchCalib {
  float scaleX = 1, offsetX = 0; // screenX = rawY * scaleX + offsetX
  float scaleY = 1, offsetY = 0; // screenY = rawX * scaleY + offsetY
};
TouchCalib g_touchCalib;

bool readRawTouch(int32_t &rawX, int32_t &rawY) {
  GT911_Scan();
  if (!touched) return false;
  rawX = Dev_Now.X[0];
  rawY = Dev_Now.Y[0];
  return true;
}

void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
  int32_t rawX, rawY;
  if (!readRawTouch(rawX, rawY)) {
    data->state = LV_INDEV_STATE_REL;
    return;
  }

  int32_t screenX = (int32_t)(rawY * g_touchCalib.scaleX + g_touchCalib.offsetX);
  int32_t screenY = (int32_t)(rawX * g_touchCalib.scaleY + g_touchCalib.offsetY);
  if (screenX < 0) screenX = 0;
  if (screenX > 479) screenX = 479;
  if (screenY < 0) screenY = 0;
  if (screenY > 319) screenY = 319;

  data->point.x = screenX;
  data->point.y = screenY;
  data->state = LV_INDEV_STATE_PR;
}

// Waits for any already-in-progress touch to release, then waits for and
// returns the raw coordinates of the next fresh tap. Does NOT call
// lv_timer_handler() while waiting - that would also poll touch via the
// indev in the background, racing with these direct reads (GT911_Scan()
// clears the "data ready" flag on every read, so two independent pollers
// can each clear it out from under the other and miss the tap).
void waitForRawTap(int32_t &rawX, int32_t &rawY) {
  int32_t x, y;
  while (readRawTouch(x, y)) { delay(5); }
  while (!readRawTouch(rawX, rawY)) { delay(5); }
  delay(30); // let the reading settle
  readRawTouch(rawX, rawY);
}

// Shows two on-screen targets, captures the real raw touch at each, and
// derives g_touchCalib from those two points. Run once at boot before
// building the real UI.
void runTouchCalibration() {
  lv_obj_clean(lv_scr_act());
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0f172a), 0);

  lv_obj_t *label = lv_label_create(lv_scr_act());
  lv_label_set_text(label, "Tap the red dot");
  lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 10);

  lv_obj_t *target = lv_obj_create(lv_scr_act());
  lv_obj_set_size(target, 20, 20);
  lv_obj_set_style_radius(target, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(target, lv_color_hex(0xff0000), 0);
  lv_obj_set_style_border_width(target, 0, 0);
  lv_obj_clear_flag(target, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(target, LV_OBJ_FLAG_CLICKABLE);

  // Pulled well clear of the true edges - there's a dead strip near the top
  // (and likely the right) of the touch-sensitive area in this orientation
  // (tft.setRotation(1)). These points are tuned specifically against that
  // dead zone; if the display orientation ever changes, re-derive them
  // rather than guessing.
  const int32_t P1X = 70,  P1Y = 110;
  const int32_t P2X = 380, P2Y = 260;
  // If both taps land within this many raw units of each other, treat it as
  // a mis-tap and retry rather than divide-by-near-zero into an inf/-inf
  // scale that clamps every touch to one screen corner.
  const int32_t MIN_RAW_SEPARATION = 20;

  int32_t r1x, r1y, r2x, r2y;
  for (;;) {
    lv_label_set_text(label, "Tap the red dot (1/2)");
    lv_obj_set_style_bg_color(target, lv_color_hex(0xff0000), 0);
    lv_obj_set_pos(target, P1X - 10, P1Y - 10);
    lv_obj_clear_flag(target, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);
    waitForRawTap(r1x, r1y);

    // Force the user to see the dot move before tapping again - also gives
    // a still-touching finger time to lift.
    lv_label_set_text(label, "Good! Now tap the dot (2/2)");
    lv_obj_set_style_bg_color(target, lv_color_hex(0x22c55e), 0);
    lv_obj_set_pos(target, P2X - 10, P2Y - 10);
    lv_refr_now(NULL);
    delay(600);
    waitForRawTap(r2x, r2y);

    if (abs(r2x - r1x) >= MIN_RAW_SEPARATION && abs(r2y - r1y) >= MIN_RAW_SEPARATION) {
      break;
    }
    Serial.printf("Calibration taps too close (r1=%d,%d r2=%d,%d) - retrying\r\n",
                  r1x, r1y, r2x, r2y);
    lv_label_set_text(label, "Too close together - try again");
    lv_obj_set_style_bg_color(target, lv_color_hex(0xff0000), 0);
    lv_refr_now(NULL);
    delay(1200);
  }

  g_touchCalib.scaleX = (float)(P2X - P1X) / (float)(r2y - r1y);
  g_touchCalib.offsetX = P1X - r1y * g_touchCalib.scaleX;
  g_touchCalib.scaleY = (float)(P2Y - P1Y) / (float)(r2x - r1x);
  g_touchCalib.offsetY = P1Y - r1x * g_touchCalib.scaleY;

  Serial.printf("Touch calibrated: scaleX=%.4f offsetX=%.2f scaleY=%.4f offsetY=%.2f\r\n",
                g_touchCalib.scaleX, g_touchCalib.offsetX,
                g_touchCalib.scaleY, g_touchCalib.offsetY);

  lv_label_set_text(label, "Calibrated!");
  lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);
  lv_refr_now(NULL);
  delay(400);

  // Make sure the finger has actually lifted before handing control to the
  // real UI. waitForRawTap() returns as soon as it captures point 2's
  // coordinates - it does NOT wait for release - so a finger still resting
  // on the glass at this instant would be seen by the real screen the
  // moment it's built. P2 (380,260) sits right where a full-width primary
  // action button ends up on several screens, so a lingering touch here
  // was firing that button's click the instant calibration finished
  // (reported as "abis kalibrasi langsung masuk ke NEED ASSISTANCE").
  int32_t releaseX, releaseY;
  while (readRawTouch(releaseX, releaseY)) { delay(5); }
}

//------------------------------------------------------------------------------
// LVGL / display plumbing
//------------------------------------------------------------------------------
static const uint16_t screenWidth  = 480;
static const uint16_t screenHeight = 320;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * 10];

TFT_eSPI tft = TFT_eSPI();

#if LV_USE_LOG != 0
void my_print(const char *buf) {
  Serial.printf(buf);
  Serial.flush();
}
#endif

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}


//------------------------------------------------------------------------------
// Demo mode selection
//------------------------------------------------------------------------------
enum AppMode { MODE_NONE, MODE_CHAT, MODE_ANDON };
static AppMode g_mode = MODE_NONE;

// Backlight idle-dimming was attempted and reverted: TFT_BL and IIC_SCL
// (touch I2C clock) are both wired to GPIO32 on this board (see
// include/User_Setup.h). Forcing the pin low between touch polls to darken
// it changed nothing visible on real hardware - confirmed via serial log
// (the idle/wake transitions fired correctly) while the backlight stayed
// lit throughout. Most likely explanation: this board's backlight isn't
// actually GPIO-controlled at all (hardwired on, as on many cheap CYD
// boards) and GPIO32's TFT_BL definition doesn't correspond to a real
// connection - not something fixable in software. Left unimplemented
// rather than keep code that pokes the shared touch pin for no benefit.

//------------------------------------------------------------------------------
// Gemini AI chat demo (ported unmodified from examples/gemini-chatbot)
//------------------------------------------------------------------------------
namespace chatapp {

// NTP Configuration
const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET = 7 * 3600; // GMT+7 (Indonesia)
const int DAYLIGHT_OFFSET = 0;

lv_obj_t * chat_input_ta = nullptr;
lv_obj_t * chat_response_label = nullptr;
lv_obj_t * chat_kb = nullptr;
lv_obj_t * chat_response_box = nullptr;
lv_obj_t * chat_send_btn = nullptr;
lv_obj_t * chat_back_btn = nullptr;
lv_obj_t * chat_wifi_gear_btn = nullptr;
lv_obj_t * chat_header_bar = nullptr;
lv_obj_t * chat_time_label = nullptr;
lv_obj_t * chat_wifi_label = nullptr;
lv_timer_t * g_chatHeaderTimer = nullptr;

// Refreshes the header's clock + WiFi status. Called on a timer and once
// right after the header is built.
void updateChatHeader(lv_timer_t *timer) {
  if (!chat_time_label || !chat_wifi_label) return;

  time_t now = time(nullptr);
  if (now > 1000000000) { // NTP-synced (after year 2001)
    struct tm *ti = localtime(&now);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", ti->tm_hour, ti->tm_min);
    lv_label_set_text(chat_time_label, buf);
  } else {
    lv_label_set_text(chat_time_label, "--:--");
  }

  bool online = (WiFi.status() == WL_CONNECTED);
  lv_label_set_text(chat_wifi_label, online ? LV_SYMBOL_WIFI " Online" : LV_SYMBOL_CLOSE " Offline");
  lv_obj_set_style_text_color(chat_wifi_label, online ? lv_color_hex(0x22c55e) : lv_color_hex(0xef4444), 0);
}

// Two layouts: "normal" (header + reply box + input row + Send button) and
// "typing" (header + reply box hidden, input box moved to the very top and
// widened to fill the space Send normally occupies - Send is hidden since
// the keyboard's own Enter/checkmark key submits - and the keyboard fills
// essentially the rest of the screen).
void setChatTypingMode(bool typing) {
  if (typing) {
    lv_obj_add_flag(chat_header_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(chat_response_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(chat_send_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(chat_back_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(chat_wifi_gear_btn, LV_OBJ_FLAG_HIDDEN);
    // Input box is DISPLAY ONLY while typing (T9 writes into it directly -
    // see t9KeypadEventCb - you never need to tap it here), so unlike the
    // back/gear buttons it's fine for it to sit up in the confirmed touch
    // dead zone (y<110 - see runTouchCalibration()'s P1Y): big and visible
    // at the top, full width.
    lv_obj_set_size(chat_input_ta, 460, 100);
    lv_obj_align(chat_input_ta, LV_ALIGN_TOP_MID, 0, 8);
    // Back/gear DO need to be tappable, so they still can't go in the dead
    // zone - but instead of a horizontal row (which cost the keypad
    // height), they're a tall vertical strip beside the keypad (split into
    // two stacked buttons - see createChatScreen()), costing width instead.
    // y=113 is the same dead-zone boundary as before; together they span
    // the full safe area down to y=315, all of which the keypad used to
    // have to share with a separate row above it.
    lv_obj_set_size(chat_kb, 380, 202);
    lv_obj_align(chat_kb, LV_ALIGN_TOP_LEFT, 10, 113);
    lv_obj_clear_flag(chat_kb, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(chat_header_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(chat_response_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(chat_send_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(chat_back_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(chat_wifi_gear_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(chat_input_ta, 340, 40);
    lv_obj_align(chat_input_ta, LV_ALIGN_TOP_LEFT, 10, 270);
    lv_obj_align(chat_send_btn, LV_ALIGN_TOP_RIGHT, -10, 270);
    lv_obj_add_flag(chat_kb, LV_OBJ_FLAG_HIDDEN);
  }
}

// Back-to-front-page button (typing mode only, to the left of the input
// box): cancels typing without sending, just like defocusing used to before
// that got tied to Send-only (see the note on chatTextareaEventCb).
void backToChatBtnEventCb(lv_event_t *e) {
  setChatTypingMode(false);
}

// fwd decls - defined later (WiFi manager section / this function itself),
// used by the gear button below before their real definitions are in scope.
void createChatScreen();
bool runWifiSetupFlow();

// Gear button next to Back in the T9 keypad's side strip: jump straight
// into the WiFi setup screens (scan/select/type password) from the chat
// screen at any time, not just when there's no working connection at boot.
// Set here, actually acted on from loop() - see the comment there for why.
bool g_wifiSetupRequested = false;

void changeWifiFromChatBtnEventCb(lv_event_t *e) {
  g_wifiSetupRequested = true;
}

// Set once a conversation is underway; sending it back as
// previous_interaction_id tells Gemini's Interactions API to continue from
// that point using history it already has server-side, so the ESP32 never
// has to store/resend the growing conversation itself (crucial on 320KB of
// RAM - a client-side "contents" history array would eventually blow that).
String g_previousInteractionId = "";

// Ask Gemini `prompt` (continuing the running conversation, if any) and
// return its reply text (or an error string).
String queryGemini(const String &prompt) {
  if (WiFi.status() != WL_CONNECTED) {
    return "No WiFi connection";
  }

  WiFiClientSecure client;
  client.setInsecure(); // skip TLS cert validation - acceptable for a hobby project
  client.setTimeout(20000); // ms - TLS handshake alone can take a few sec on ESP32

  HTTPClient http;
  const String url = "https://generativelanguage.googleapis.com/v1beta/interactions";
  http.begin(client, url);
  http.setTimeout(20000);        // ms - default is too short for Gemini's response time
  http.setConnectTimeout(20000); // ms
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-goog-api-key", GEMINI_API_KEY);
  http.addHeader("Api-Revision", "2026-05-20"); // per Gemini's Interactions API quickstart

  JsonDocument reqDoc;
  reqDoc["model"] = "gemini-flash-latest";
  reqDoc["input"] = prompt;
  if (g_previousInteractionId.length() > 0) {
    reqDoc["previous_interaction_id"] = g_previousInteractionId;
  }
  String reqBody;
  serializeJson(reqDoc, reqBody);

  String result = "Error contacting Gemini";
  int httpCode = http.POST(reqBody);

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();

    // Only keep the fields we actually need - keeps parsing cheap on 320KB
    // RAM. "steps" is an array of {type, content:[{type,text}]} entries
    // (user_input, model_output, ...); index 0 is ArduinoJson's filter
    // template applied to every array element, not "only element 0".
    JsonDocument filter;
    filter["id"] = true;
    filter["steps"][0]["type"] = true;
    filter["steps"][0]["content"][0]["type"] = true;
    filter["steps"][0]["content"][0]["text"] = true;

    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    if (!err) {
      const char *newId = doc["id"];
      if (newId) g_previousInteractionId = String(newId);

      for (JsonObject step : doc["steps"].as<JsonArray>()) {
        const char *stepType = step["type"];
        if (!stepType || strcmp(stepType, "model_output") != 0) continue;
        for (JsonObject part : step["content"].as<JsonArray>()) {
          const char *partType = part["type"];
          if (partType && strcmp(partType, "text") == 0) {
            const char *text = part["text"];
            if (text) result = String(text);
          }
        }
      }
    } else {
      result = "Parse error";
      Serial.println(err.c_str());
    }
  } else {
    result = "HTTP error: " + String(httpCode);
    Serial.println(http.getString());
  }

  http.end();
  return result;
}

void sendChatMessage(lv_event_t *e) {
  if (!chat_input_ta || !chat_response_label) return;

  const char *prompt = lv_textarea_get_text(chat_input_ta);
  if (strlen(prompt) == 0) return;

  String promptCopy = prompt; // queryGemini() clears the textarea below
  setChatTypingMode(false); // reveal the reply box + hide keyboard again
  lv_label_set_text(chat_response_label, "Thinking...");
  lv_refr_now(NULL); // paint "Thinking..." before the blocking HTTP call

  String reply = queryGemini(promptCopy);

  lv_label_set_text(chat_response_label, reply.c_str());
  lv_textarea_set_text(chat_input_ta, "");
}

// Show/hide the keyboard as the textarea gains/loses focus, and treat the
// keyboard's Enter key the same as tapping Send.
// The normal-mode button next to the input box: a keyboard icon that opens
// typing mode (tapping the input box itself does the same via FOCUSED
// below, this is just a more obviously-tappable/discoverable entry point).
// Actually sending a message happens via the T9 keypad's own checkmark key.
void openKeyboardBtnEventCb(lv_event_t *e) {
  lv_obj_add_state(chat_input_ta, LV_STATE_FOCUSED); // so the cursor shows
  setChatTypingMode(true);
}

void chatTextareaEventCb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED) {
    setChatTypingMode(true);
  }
  // Deliberately NOT reacting to LV_EVENT_DEFOCUSED here: tapping any T9
  // key on the keypad shifts LVGL's focus away from this textarea too,
  // which fired this same event and exited typing mode on every keypress
  // instead of only on Send. Typing mode now only ends via sendChatMessage()
  // (Send button or the T9 keypad's Send/checkmark key).
}

// ---------------------------------------------------------------------------
// T9-style multi-tap keypad (old feature-phone style): a 4x3 grid instead of
// a QWERTY layout, so each button is huge (~153x65px vs ~40-90px on the
// QWERTY attempts before this). Built on a plain lv_btnmatrix rather than
// lv_keyboard - lv_keyboard always inserts a button's literal label text
// into the bound textarea on click, which can't express "tap twice quickly
// to cycle to the next letter", so button presses are handled entirely by
// hand in t9KeypadEventCb() below instead.
// ---------------------------------------------------------------------------

// Grid order: 1 2 3 / 4 5 6 / 7 8 9 / <BACKSPACE> 0 <SEND>. Each entry is
// every character that digit's key cycles through on repeated taps within
// T9_CYCLE_TIMEOUT_MS (last char in each string is the digit itself, so you
// can still reach a literal digit by tapping through the whole cycle).
// NULL entries (backspace/send) are handled as special cases, not cycled.
static const char * T9_CYCLES[12] = {
  ".,!?1", "abc2", "def3",
  "ghi4",  "jkl5", "mno6",
  "pqrs7", "tuv8", "wxyz9",
  NULL,    " 0",   NULL,
};
static const int T9_BACKSPACE_IDX = 9;
static const int T9_SEND_IDX = 11;
static const uint32_t T9_CYCLE_TIMEOUT_MS = 600;

// Each button shows its digit and letters on two lines, e.g. "2\nabc" -
// safe to embed a literal newline inside a label like this because
// lv_btnmatrix only treats an array element that IS "\n" (via strcmp) as a
// row break, not one that merely contains one (confirmed in lv_btnmatrix.c).
static const char * chat_t9_map[] = {
  "1\n.,!?", "2\nabc", "3\ndef", "\n",
  "4\nghi",  "5\njkl", "6\nmno", "\n",
  "7\npqrs", "8\ntuv", "9\nwxyz", "\n",
  LV_SYMBOL_BACKSPACE, "0\n_", LV_SYMBOL_OK, "",
};

int g_t9LastBtnIdx = -1;
int g_t9CyclePos = 0;
uint32_t g_t9LastPressMs = 0;

void t9KeypadEventCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  lv_obj_t *btnm = lv_event_get_target(e);
  uint16_t idx = lv_btnmatrix_get_selected_btn(btnm);
  if (idx == LV_BTNMATRIX_BTN_NONE) return;

  if (idx == T9_BACKSPACE_IDX) {
    lv_textarea_del_char(chat_input_ta);
    g_t9LastBtnIdx = -1;
    return;
  }
  if (idx == T9_SEND_IDX) {
    g_t9LastBtnIdx = -1;
    sendChatMessage(NULL);
    return;
  }

  const char *cycle = T9_CYCLES[idx];
  if (!cycle) return;
  int cycleLen = (int)strlen(cycle);

  uint32_t now = millis();
  bool cyclingSameKey = (idx == g_t9LastBtnIdx) && (now - g_t9LastPressMs < T9_CYCLE_TIMEOUT_MS);

  if (cyclingSameKey) {
    lv_textarea_del_char(chat_input_ta); // replace the last candidate letter
    g_t9CyclePos = (g_t9CyclePos + 1) % cycleLen;
  } else {
    g_t9CyclePos = 0; // different key, or timed out - start a new letter
  }

  lv_textarea_add_char(chat_input_ta, cycle[g_t9CyclePos]);
  g_t9LastBtnIdx = idx;
  g_t9LastPressMs = now;
}

// Builds the entire (only) screen: reply area on top, input box + Send
// button below it, and an lv_keyboard that pops up when the input box is
// focused. Called once from setup().
void createChatScreen() {
  lv_obj_clean(lv_scr_act());
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0f172a), 0);

  // Header bar: clock, title, WiFi status (replaces a battery indicator -
  // this board has no battery). Hidden while typing to reclaim space for
  // the input box + keyboard.
  chat_header_bar = lv_obj_create(lv_scr_act());
  lv_obj_set_size(chat_header_bar, 480, 28);
  lv_obj_set_pos(chat_header_bar, 0, 0);
  lv_obj_set_style_bg_color(chat_header_bar, lv_color_hex(0x0f172a), 0);
  lv_obj_set_style_border_width(chat_header_bar, 0, 0);
  lv_obj_set_style_radius(chat_header_bar, 0, 0);
  lv_obj_clear_flag(chat_header_bar, LV_OBJ_FLAG_SCROLLABLE);

  chat_time_label = lv_label_create(chat_header_bar);
  lv_label_set_text(chat_time_label, "--:--");
  lv_obj_set_style_text_color(chat_time_label, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_font(chat_time_label, &lv_font_montserrat_14, 0);
  lv_obj_align(chat_time_label, LV_ALIGN_LEFT_MID, 8, 0);

  lv_obj_t *title_label = lv_label_create(chat_header_bar);
  lv_label_set_text(title_label, "AI Assistant");
  lv_obj_set_style_text_color(title_label, lv_color_hex(0x22c55e), 0);
  lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, 0);
  lv_obj_align(title_label, LV_ALIGN_CENTER, 0, 0);

  chat_wifi_label = lv_label_create(chat_header_bar);
  lv_label_set_text(chat_wifi_label, LV_SYMBOL_CLOSE " Offline");
  lv_obj_set_style_text_font(chat_wifi_label, &lv_font_montserrat_14, 0);
  lv_obj_align(chat_wifi_label, LV_ALIGN_RIGHT_MID, -8, 0);

  updateChatHeader(NULL);                                     // paint real state immediately
  if (g_chatHeaderTimer) lv_timer_del(g_chatHeaderTimer);      // don't stack a duplicate on repeat visits
  g_chatHeaderTimer = lv_timer_create(updateChatHeader, 2000, NULL);

  // Reply area - fills essentially the rest of the screen (input row is
  // pinned to the bottom below it). Hidden while typing (see
  // setChatTypingMode) so the input box + keyboard can take over instead.
  chat_response_box = lv_obj_create(lv_scr_act());
  lv_obj_set_size(chat_response_box, 460, 227);
  lv_obj_align(chat_response_box, LV_ALIGN_TOP_MID, 0, 33);
  lv_obj_set_style_bg_color(chat_response_box, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(chat_response_box, 1, 0);
  lv_obj_set_style_border_color(chat_response_box, lv_color_hex(0x334155), 0);
  lv_obj_set_style_radius(chat_response_box, 8, 0);

  chat_response_label = lv_label_create(chat_response_box);
  lv_label_set_long_mode(chat_response_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(chat_response_label, 430);
  lv_label_set_text(chat_response_label, "Ask me anything!");
  lv_obj_set_style_text_color(chat_response_label, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_font(chat_response_label, &lv_font_montserrat_14, 0);
  lv_obj_align(chat_response_label, LV_ALIGN_TOP_LEFT, 5, 5);

  // Back-to-front button (typing mode only - hidden here, shown by
  // setChatTypingMode). Sits to the left of the input box.
  // Right-side strip split into two stacked buttons: gear on top to jump
  // into WiFi setup at any time, Back below to cancel typing (same total
  // footprint as the old single button - still clear of the dead zone).
  chat_wifi_gear_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(chat_wifi_gear_btn, 70, 97);
  lv_obj_align(chat_wifi_gear_btn, LV_ALIGN_TOP_RIGHT, -10, 113);
  lv_obj_add_event_cb(chat_wifi_gear_btn, changeWifiFromChatBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_set_style_radius(chat_wifi_gear_btn, 16, 0);
  lv_obj_set_style_bg_color(chat_wifi_gear_btn, lv_color_hex(0x334155), 0);
  lv_obj_t *gear_label = lv_label_create(chat_wifi_gear_btn);
  lv_label_set_text(gear_label, LV_SYMBOL_SETTINGS "\nWiFi");
  lv_obj_set_style_text_align(gear_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(gear_label);
  lv_obj_add_flag(chat_wifi_gear_btn, LV_OBJ_FLAG_HIDDEN);

  chat_back_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(chat_back_btn, 70, 97);
  lv_obj_align(chat_back_btn, LV_ALIGN_TOP_RIGHT, -10, 218); // 113 + 97 + 8 gap
  lv_obj_add_event_cb(chat_back_btn, backToChatBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_set_style_radius(chat_back_btn, 16, 0);
  lv_obj_set_style_bg_color(chat_back_btn, lv_color_hex(0x334155), 0);
  lv_obj_t *back_label = lv_label_create(chat_back_btn);
  lv_label_set_text(back_label, LV_SYMBOL_LEFT "\nBack"); // two lines - fits a tall narrow button
  lv_obj_set_style_text_align(back_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(back_label);
  lv_obj_add_flag(chat_back_btn, LV_OBJ_FLAG_HIDDEN);

  // Input row: textarea + Send button
  chat_input_ta = lv_textarea_create(lv_scr_act());
  lv_obj_set_size(chat_input_ta, 340, 40);
  lv_obj_align(chat_input_ta, LV_ALIGN_TOP_LEFT, 10, 270);
  lv_textarea_set_one_line(chat_input_ta, true);
  lv_textarea_set_placeholder_text(chat_input_ta, "Type a message...");
  lv_obj_add_event_cb(chat_input_ta, chatTextareaEventCb, LV_EVENT_ALL, NULL);
  lv_obj_set_style_radius(chat_input_ta, 16, 0);
  lv_obj_set_style_border_width(chat_input_ta, 2, 0);
  lv_obj_set_style_border_color(chat_input_ta, lv_color_hex(0x22c55e), 0);
  lv_obj_set_style_bg_color(chat_input_ta, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_text_color(chat_input_ta, lv_color_hex(0xffffff), 0);

  chat_send_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(chat_send_btn, 100, 40);
  lv_obj_align(chat_send_btn, LV_ALIGN_TOP_RIGHT, -10, 270);
  lv_obj_add_event_cb(chat_send_btn, openKeyboardBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_set_style_radius(chat_send_btn, 16, 0);
  lv_obj_set_style_bg_color(chat_send_btn, lv_color_hex(0x22c55e), 0); // match the green accent theme
  lv_obj_t *send_label = lv_label_create(chat_send_btn);
  lv_label_set_text(send_label, LV_SYMBOL_KEYBOARD);
  lv_obj_center(send_label);

  // T9 keypad, hidden until the textarea is focused. setChatTypingMode()
  // resizes/repositions it (and the input row) each time it's shown/hidden.
  // Styled as a rounded bordered "card" to match the input box. A plain
  // lv_btnmatrix, not lv_keyboard - see t9KeypadEventCb() for why.
  chat_kb = lv_btnmatrix_create(lv_scr_act());
  lv_btnmatrix_set_map(chat_kb, chat_t9_map);
  lv_obj_add_event_cb(chat_kb, t9KeypadEventCb, LV_EVENT_VALUE_CHANGED, NULL);
  // Outer "card" background - transparent/borderless so only the individual
  // key outlines below show, matching the reference (each key has its own
  // outline on a plain dark background, not one big bordered box).
  lv_obj_set_style_bg_opa(chat_kb, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chat_kb, 0, 0);
  lv_obj_set_style_pad_all(chat_kb, 4, 0);
  lv_obj_set_style_pad_row(chat_kb, 6, 0);
  lv_obj_set_style_pad_column(chat_kb, 6, 0);
  // Per-key styling (LV_PART_ITEMS = the individual buttons, not the
  // container) - dark fill, thin green outline, rounded corners.
  lv_obj_set_style_radius(chat_kb, 10, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(chat_kb, lv_color_hex(0x1e293b), LV_PART_ITEMS);
  lv_obj_set_style_border_width(chat_kb, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(chat_kb, lv_color_hex(0x22c55e), LV_PART_ITEMS);
  lv_obj_set_style_text_color(chat_kb, lv_color_hex(0xffffff), LV_PART_ITEMS);
  lv_obj_add_flag(chat_kb, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// On-device WiFi manager: scan for networks, pick one by tapping, type its
// password on the same T9 keypad the chat screen uses, and save the result
// to NVS (via Preferences) so it's remembered across reboots/reflashes -
// no more hardcoding SSID/password in secrets.h and reflashing to change
// networks. Runs once at boot, blocking (same lv_timer_handler()+delay()
// polling pattern as runTouchCalibration()), before the chat screen is
// built. Layout reuses the exact coordinates already proven safe on this
// panel's touch dead zone (see setChatTypingMode()'s comments).
// ---------------------------------------------------------------------------

Preferences g_wifiPrefs;

bool loadSavedWifi(String &ssid, String &pass) {
  g_wifiPrefs.begin("wifi", true); // read-only
  ssid = g_wifiPrefs.getString("ssid", "");
  pass = g_wifiPrefs.getString("pass", "");
  g_wifiPrefs.end();
  return ssid.length() > 0;
}

void saveWifi(const String &ssid, const String &pass) {
  g_wifiPrefs.begin("wifi", false);
  g_wifiPrefs.putString("ssid", ssid);
  g_wifiPrefs.putString("pass", pass);
  g_wifiPrefs.end();
}

// Blocking connect attempt (up to ~10s), updating statusLabel (if given)
// as it goes. Returns true on success.
bool tryConnectWifi(const String &ssid, const String &pass, lv_obj_t *statusLabel) {
  Serial.printf("Connecting to WiFi: %s\r\n", ssid.c_str());
  WiFi.disconnect(true);
  delay(1000); // full radio power-cycle from disconnect(true) needs this long to settle
  WiFi.mode(WIFI_STA);
  delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
    if (statusLabel) {
      String dots = "";
      for (int i = 0; i < (attempts % 4); i++) dots += ".";
      lv_label_set_text_fmt(statusLabel, "Connecting%s", dots.c_str());
      lv_refr_now(NULL);
    }
  }
  bool ok = (WiFi.status() == WL_CONNECTED);
  Serial.println(ok ? "WiFi connected!" : "WiFi connect failed.");
  return ok;
}

// Set by the list/password views' button handlers; polled by the blocking
// runWifiSetupFlow() loop in setup() to know when to stop.
bool g_wifiSetupDone = false;
bool g_wifiSetupConnected = false;

void gt911_int_(); // fwd decl - defined near the bottom of the file

// --- Single combined setup screen -------------------------------------------
// Network list and password entry are two VIEWS on the same screen, toggled
// by show/hide - not separate screens rebuilt via lv_obj_clean(). Originally
// these were two lv_obj_clean()-rebuilt screens; after the WiFi scan, touch
// stopped registering at all (zero raw reads, not just missed clicks) and
// neither re-running the GT911 init nor other fixes recovered it, so this
// cuts the number of full screen tear-downs/rebuilds during the flow down
// to just one (right when the flow starts), which also means backing out of
// password entry to pick a different network doesn't need a re-scan.
// ---------------------------------------------------------------------------

// Network picker: a wide label (column 1) shows the currently-selected
// SSID; Up/Select/Down (column 2) step through the scan results and
// confirm one. This replaced a per-item clickable list (one button per
// network in a shared container) that rendered fine but wouldn't reliably
// register clicks - Up/Select/Down are the same kind of standalone lv_btn
// directly on the screen as Skip/Cancel/gear/T9 keys, which have all
// worked reliably, so this sidesteps whatever was specific to the list.
// Column 1 is a real multi-row list now: WIFI_LIST_VISIBLE_ROWS row widgets
// created once, refreshed in place by updateWifiListRows() as Up/Down move
// the highlight (a sliding window over the scan results - no scrolling, no
// per-row click handlers; rows are display-only, Select confirms the
// highlighted one). Rows are plain lv_obj + label children of the box,
// which is safe: the earlier unresponsive-list problem turned out to be the
// nested lv_timer_handler() call (see loop()), not child widgets.
#define WIFI_LIST_VISIBLE_ROWS 7
lv_obj_t * wifi_current_box = nullptr;   // bordered container - hide/show target
lv_obj_t * wifi_list_rows[WIFI_LIST_VISIBLE_ROWS] = {nullptr};   // row background objects
lv_obj_t * wifi_list_row_labels[WIFI_LIST_VISIBLE_ROWS] = {nullptr}; // their text labels
lv_obj_t * wifi_up_btn = nullptr;
lv_obj_t * wifi_select_btn = nullptr;
lv_obj_t * wifi_down_btn = nullptr;
lv_obj_t * wifi_list_cancel_btn = nullptr;
lv_obj_t * wifi_list_skip_btn = nullptr;
int g_wifiListIdx = 0;
int g_wifiListCount = 0;

lv_obj_t * wifi_pw_title = nullptr;
lv_obj_t * wifi_pw_ta = nullptr;
lv_obj_t * wifi_pw_status_label = nullptr;
lv_obj_t * wifi_pw_back_btn = nullptr;
lv_obj_t * wifi_pw_kb = nullptr;
String g_wifiSetupSsid = "";

int g_wifiT9LastBtnIdx = -1;
int g_wifiT9CyclePos = 0;
uint32_t g_wifiT9LastPressMs = 0;

void updateWifiListRows() {
  if (g_wifiListCount <= 0) {
    for (int i = 0; i < WIFI_LIST_VISIBLE_ROWS; i++) {
      lv_obj_add_flag(wifi_list_rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(wifi_list_rows[0], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(wifi_list_row_labels[0], "No networks found");
    return;
  }

  // Sliding window over the results, keeping the highlighted entry visible
  // (roughly centered once the list is longer than the window).
  int start = g_wifiListIdx - WIFI_LIST_VISIBLE_ROWS / 2;
  if (start > g_wifiListCount - WIFI_LIST_VISIBLE_ROWS) start = g_wifiListCount - WIFI_LIST_VISIBLE_ROWS;
  if (start < 0) start = 0;

  for (int i = 0; i < WIFI_LIST_VISIBLE_ROWS; i++) {
    int netIdx = start + i;
    if (netIdx >= g_wifiListCount) {
      lv_obj_add_flag(wifi_list_rows[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(wifi_list_rows[i], LV_OBJ_FLAG_HIDDEN);
    bool isOpen = (WiFi.encryptionType(netIdx) == WIFI_AUTH_OPEN);
    lv_label_set_text_fmt(wifi_list_row_labels[i], "%s%s",
                           isOpen ? "" : LV_SYMBOL_CLOSE " ",
                           WiFi.SSID(netIdx).c_str());
    bool highlighted = (netIdx == g_wifiListIdx);
    lv_obj_set_style_bg_color(wifi_list_rows[i],
                               highlighted ? lv_color_hex(0x22c55e) : lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_text_color(wifi_list_row_labels[i],
                                 highlighted ? lv_color_hex(0x0f172a) : lv_color_hex(0xffffff), 0);
  }
}

void showWifiListView() {
  lv_obj_clear_flag(wifi_current_box, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_up_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_select_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_down_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_list_cancel_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_list_skip_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_pw_title, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_pw_ta, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_pw_status_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_pw_back_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_pw_kb, LV_OBJ_FLAG_HIDDEN);
}

void showWifiPasswordView(const String &ssid) {
  g_wifiSetupSsid = ssid;
  g_wifiT9LastBtnIdx = -1;
  lv_label_set_text_fmt(wifi_pw_title, "Password for: %s", ssid.c_str());
  lv_textarea_set_text(wifi_pw_ta, "");
  lv_label_set_text(wifi_pw_status_label, "");

  lv_obj_add_flag(wifi_current_box, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_up_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_select_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_down_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_list_cancel_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_list_skip_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_pw_title, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_pw_ta, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_pw_status_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_pw_back_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_pw_kb, LV_OBJ_FLAG_HIDDEN);
}

void wifiBackToListBtnEventCb(lv_event_t *e) {
  showWifiListView();
}

void wifiPwKeypadEventCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  lv_obj_t *btnm = lv_event_get_target(e);
  uint16_t idx = lv_btnmatrix_get_selected_btn(btnm);
  if (idx == LV_BTNMATRIX_BTN_NONE) return;

  if (idx == T9_BACKSPACE_IDX) {
    lv_textarea_del_char(wifi_pw_ta);
    g_wifiT9LastBtnIdx = -1;
    return;
  }
  if (idx == T9_SEND_IDX) {
    g_wifiT9LastBtnIdx = -1;
    String pass = lv_textarea_get_text(wifi_pw_ta);
    lv_label_set_text(wifi_pw_status_label, "Connecting...");
    lv_refr_now(NULL);
    if (tryConnectWifi(g_wifiSetupSsid, pass, wifi_pw_status_label)) {
      saveWifi(g_wifiSetupSsid, pass);
      g_wifiSetupConnected = true;
      g_wifiSetupDone = true;
    } else {
      lv_label_set_text(wifi_pw_status_label, "Failed - check password, try again");
    }
    return;
  }

  const char *cycle = T9_CYCLES[idx];
  if (!cycle) return;
  int cycleLen = (int)strlen(cycle);

  uint32_t now = millis();
  bool cyclingSameKey = (idx == g_wifiT9LastBtnIdx) && (now - g_wifiT9LastPressMs < T9_CYCLE_TIMEOUT_MS);

  if (cyclingSameKey) {
    lv_textarea_del_char(wifi_pw_ta);
    g_wifiT9CyclePos = (g_wifiT9CyclePos + 1) % cycleLen;
  } else {
    g_wifiT9CyclePos = 0;
  }

  lv_textarea_add_char(wifi_pw_ta, cycle[g_wifiT9CyclePos]);
  g_wifiT9LastBtnIdx = idx;
  g_wifiT9LastPressMs = now;
}

void wifiListSkipBtnEventCb(lv_event_t *e) {
  g_wifiSetupConnected = false;
  g_wifiSetupDone = true;
}

// Distinct from Skip: this is for when the setup screen was opened from the
// chat screen's gear button while ALREADY connected (not just at boot with
// nothing working yet) - "cancel" should restore whatever was previously
// connected rather than force offline mode.
void wifiListCancelBtnEventCb(lv_event_t *e) {
  String ssid, pass;
  if (loadSavedWifi(ssid, pass)) {
    tryConnectWifi(ssid, pass, nullptr);
  }
  g_wifiSetupConnected = (WiFi.status() == WL_CONNECTED);
  g_wifiSetupDone = true;
}

void wifiUpBtnEventCb(lv_event_t *e) {
  if (g_wifiListCount <= 0) return;
  g_wifiListIdx = (g_wifiListIdx - 1 + g_wifiListCount) % g_wifiListCount;
  updateWifiListRows();
}

void wifiDownBtnEventCb(lv_event_t *e) {
  if (g_wifiListCount <= 0) return;
  g_wifiListIdx = (g_wifiListIdx + 1) % g_wifiListCount;
  updateWifiListRows();
}

void wifiSelectBtnEventCb(lv_event_t *e) {
  if (g_wifiListCount <= 0) return;
  String ssid = WiFi.SSID(g_wifiListIdx);
  bool isOpen = (WiFi.encryptionType(g_wifiListIdx) == WIFI_AUTH_OPEN);
  if (isOpen) {
    if (tryConnectWifi(ssid, "", nullptr)) {
      saveWifi(ssid, "");
      g_wifiSetupConnected = true;
      g_wifiSetupDone = true;
      return;
    }
    // fall through to password view if an "open" network unexpectedly
    // still needs one (captive portals etc. aren't handled - offer manual entry)
  }
  showWifiPasswordView(ssid);
}

void createWifiSetupScreen() {
  lv_obj_clean(lv_scr_act());
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0f172a), 0);

  lv_obj_t *title = lv_label_create(lv_scr_act());
  lv_label_set_text(title, "Select a WiFi network");
  lv_obj_set_style_text_color(title, lv_color_hex(0x22c55e), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
  lv_refr_now(NULL); // paint the title before the blocking scan below

  // Two columns, all standalone lv_btn/lv_obj widgets directly on the
  // screen (same class of widget as gear/T9 keys, all of which have worked
  // reliably) instead of many small buttons inside one shared container
  // (which rendered fine but wouldn't reliably register clicks):
  //   col 1 (wide, x=10..300): current SSID display, non-interactive
  //   col 2 (x=310..470): Up / Select / Down / Back / Skip, stacked

  // Only fully reset the radio before scanning if we're not already
  // connected. A mid-chat "change WiFi" tap (the gear button) starts out
  // already connected, and unconditionally forcing disconnect(true) + a 1s
  // settle here was one of the suspects for touch going dead afterwards -
  // scanning while connected is a normal supported ESP32 WiFi operation, no
  // need to tear the radio down for it.
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(true);
    delay(1000); // full radio power-cycle from disconnect(true) needs this long to settle
    WiFi.mode(WIFI_STA);
    delay(100);
  }

  Serial.println("Scanning WiFi networks...");
  int n = WiFi.scanNetworks();
  Serial.printf("scanNetworks() returned %d\r\n", n);
  g_wifiListCount = n > 0 ? n : 0;
  g_wifiListIdx = 0;

  // --- Column 1: the network list (7 visible rows, sliding window,
  // highlighted row = current selection; see updateWifiListRows()).
  // Full height from just under the title down to the screen bottom - the
  // rows are display-only (Up/Down/Select do all the interaction), so
  // unlike the buttons this box is allowed to extend up into the touch
  // dead zone (y<110) without any downside. ---
  wifi_current_box = lv_obj_create(lv_scr_act());
  lv_obj_set_size(wifi_current_box, 290, 280);
  lv_obj_align(wifi_current_box, LV_ALIGN_TOP_LEFT, 10, 35);
  lv_obj_set_style_bg_color(wifi_current_box, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(wifi_current_box, 2, 0);
  lv_obj_set_style_border_color(wifi_current_box, lv_color_hex(0x22c55e), 0); // green outline - same theme as input box/T9 keys
  lv_obj_set_style_radius(wifi_current_box, 8, 0);
  lv_obj_set_style_pad_all(wifi_current_box, 4, 0);
  lv_obj_clear_flag(wifi_current_box, LV_OBJ_FLAG_SCROLLABLE);

  // 7 rows x 36px + 6x 3px gaps = 270px inside the 280px box (with padding)
  for (int i = 0; i < WIFI_LIST_VISIBLE_ROWS; i++) {
    wifi_list_rows[i] = lv_obj_create(wifi_current_box);
    lv_obj_set_size(wifi_list_rows[i], 274, 36);
    lv_obj_set_pos(wifi_list_rows[i], 0, i * 39);
    lv_obj_set_style_bg_color(wifi_list_rows[i], lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_width(wifi_list_rows[i], 1, 0);
    lv_obj_set_style_border_color(wifi_list_rows[i], lv_color_hex(0x22c55e), 0); // same outline theme as T9 keys
    lv_obj_set_style_radius(wifi_list_rows[i], 6, 0);
    lv_obj_set_style_pad_all(wifi_list_rows[i], 0, 0);
    lv_obj_clear_flag(wifi_list_rows[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(wifi_list_rows[i], LV_OBJ_FLAG_CLICKABLE); // display-only; Select confirms

    wifi_list_row_labels[i] = lv_label_create(wifi_list_rows[i]);
    lv_obj_set_style_text_color(wifi_list_row_labels[i], lv_color_hex(0xffffff), 0);
    lv_label_set_long_mode(wifi_list_row_labels[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(wifi_list_row_labels[i], 258);
    lv_obj_align(wifi_list_row_labels[i], LV_ALIGN_LEFT_MID, 8, 0);
  }

  if (n < 0) {
    for (int i = 1; i < WIFI_LIST_VISIBLE_ROWS; i++) lv_obj_add_flag(wifi_list_rows[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(wifi_list_row_labels[0], "Scan failed (code %d)", n);
  } else {
    updateWifiListRows();
  }

  // --- Column 2: Up / Select / Down / Back / Skip, all stacked. 5 buttons
  // in 202px -> 37px each with 4px gaps (5*37 + 4*4 = 201).
  const int32_t WIFI_COL2_X = 310, WIFI_COL2_W = 160;
  const int32_t WIFI_COL2_ROW_H = 37, WIFI_COL2_GAP = 4;
  int32_t col2Y = 113;

  wifi_up_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(wifi_up_btn, WIFI_COL2_W, WIFI_COL2_ROW_H);
  lv_obj_align(wifi_up_btn, LV_ALIGN_TOP_LEFT, WIFI_COL2_X, col2Y);
  lv_obj_set_style_radius(wifi_up_btn, 10, 0);
  lv_obj_set_style_bg_color(wifi_up_btn, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(wifi_up_btn, 1, 0);
  lv_obj_set_style_border_color(wifi_up_btn, lv_color_hex(0x22c55e), 0); // same outline theme as T9 keys
  lv_obj_add_event_cb(wifi_up_btn, wifiUpBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *up_label = lv_label_create(wifi_up_btn);
  lv_label_set_text(up_label, LV_SYMBOL_UP);
  lv_obj_center(up_label);
  col2Y += WIFI_COL2_ROW_H + WIFI_COL2_GAP;

  wifi_select_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(wifi_select_btn, WIFI_COL2_W, WIFI_COL2_ROW_H);
  lv_obj_align(wifi_select_btn, LV_ALIGN_TOP_LEFT, WIFI_COL2_X, col2Y);
  lv_obj_set_style_radius(wifi_select_btn, 10, 0);
  lv_obj_set_style_bg_color(wifi_select_btn, lv_color_hex(0x22c55e), 0);
  lv_obj_add_event_cb(wifi_select_btn, wifiSelectBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *select_label = lv_label_create(wifi_select_btn);
  lv_label_set_text(select_label, "Select");
  lv_obj_center(select_label);
  col2Y += WIFI_COL2_ROW_H + WIFI_COL2_GAP;

  wifi_down_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(wifi_down_btn, WIFI_COL2_W, WIFI_COL2_ROW_H);
  lv_obj_align(wifi_down_btn, LV_ALIGN_TOP_LEFT, WIFI_COL2_X, col2Y);
  lv_obj_set_style_radius(wifi_down_btn, 10, 0);
  lv_obj_set_style_bg_color(wifi_down_btn, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(wifi_down_btn, 1, 0);
  lv_obj_set_style_border_color(wifi_down_btn, lv_color_hex(0x22c55e), 0); // same outline theme as T9 keys
  lv_obj_add_event_cb(wifi_down_btn, wifiDownBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *down_label = lv_label_create(wifi_down_btn);
  lv_label_set_text(down_label, LV_SYMBOL_DOWN);
  lv_obj_center(down_label);
  col2Y += WIFI_COL2_ROW_H + WIFI_COL2_GAP;

  // Cancel (reconnect to whatever was working before) / Skip (deliberately
  // go offline) - distinct actions, see wifiListCancelBtnEventCb()'s
  // comment for why they can't be the same button.
  wifi_list_cancel_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(wifi_list_cancel_btn, WIFI_COL2_W, WIFI_COL2_ROW_H);
  lv_obj_align(wifi_list_cancel_btn, LV_ALIGN_TOP_LEFT, WIFI_COL2_X, col2Y);
  lv_obj_set_style_radius(wifi_list_cancel_btn, 10, 0);
  lv_obj_set_style_bg_color(wifi_list_cancel_btn, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(wifi_list_cancel_btn, 1, 0);
  lv_obj_set_style_border_color(wifi_list_cancel_btn, lv_color_hex(0x22c55e), 0); // same outline theme as T9 keys
  lv_obj_add_event_cb(wifi_list_cancel_btn, wifiListCancelBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *cancel_label = lv_label_create(wifi_list_cancel_btn);
  lv_label_set_text(cancel_label, LV_SYMBOL_LEFT " Back");
  lv_obj_center(cancel_label);
  col2Y += WIFI_COL2_ROW_H + WIFI_COL2_GAP;

  wifi_list_skip_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(wifi_list_skip_btn, WIFI_COL2_W, WIFI_COL2_ROW_H);
  lv_obj_align(wifi_list_skip_btn, LV_ALIGN_TOP_LEFT, WIFI_COL2_X, col2Y);
  lv_obj_set_style_radius(wifi_list_skip_btn, 10, 0);
  lv_obj_set_style_bg_color(wifi_list_skip_btn, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(wifi_list_skip_btn, 1, 0);
  lv_obj_set_style_border_color(wifi_list_skip_btn, lv_color_hex(0x22c55e), 0); // same outline theme as T9 keys
  lv_obj_add_event_cb(wifi_list_skip_btn, wifiListSkipBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *skip_label = lv_label_create(wifi_list_skip_btn);
  lv_label_set_text(skip_label, "Skip");
  lv_obj_center(skip_label);

  // --- Password view - built here but hidden; showWifiPasswordView() and
  // showWifiListView() toggle which view is visible without ever rebuilding
  // either one. Same coordinates as the chat screen's typing mode / the
  // list above (all field-tested clear of the dead zone).
  wifi_pw_title = lv_label_create(lv_scr_act());
  lv_label_set_text(wifi_pw_title, "Password for:");
  lv_obj_set_style_text_color(wifi_pw_title, lv_color_hex(0x22c55e), 0);
  lv_obj_align(wifi_pw_title, LV_ALIGN_TOP_MID, 0, 8);

  wifi_pw_ta = lv_textarea_create(lv_scr_act());
  lv_obj_set_size(wifi_pw_ta, 460, 40);
  lv_obj_align(wifi_pw_ta, LV_ALIGN_TOP_MID, 0, 35);
  lv_textarea_set_one_line(wifi_pw_ta, true);
  lv_textarea_set_password_mode(wifi_pw_ta, false); // shown in plain text, not masked, per request
  lv_textarea_set_placeholder_text(wifi_pw_ta, "Password");
  lv_obj_set_style_radius(wifi_pw_ta, 16, 0);
  lv_obj_set_style_border_width(wifi_pw_ta, 2, 0);
  lv_obj_set_style_border_color(wifi_pw_ta, lv_color_hex(0x22c55e), 0);
  lv_obj_set_style_bg_color(wifi_pw_ta, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_text_color(wifi_pw_ta, lv_color_hex(0xffffff), 0);

  wifi_pw_status_label = lv_label_create(lv_scr_act());
  lv_label_set_text(wifi_pw_status_label, "");
  lv_obj_set_style_text_color(wifi_pw_status_label, lv_color_hex(0xffffff), 0);
  lv_obj_align(wifi_pw_status_label, LV_ALIGN_TOP_MID, 0, 80);

  wifi_pw_back_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(wifi_pw_back_btn, 70, 202);
  lv_obj_align(wifi_pw_back_btn, LV_ALIGN_TOP_RIGHT, -10, 113);
  lv_obj_add_event_cb(wifi_pw_back_btn, wifiBackToListBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_set_style_radius(wifi_pw_back_btn, 16, 0);
  lv_obj_set_style_bg_color(wifi_pw_back_btn, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(wifi_pw_back_btn, 1, 0);
  lv_obj_set_style_border_color(wifi_pw_back_btn, lv_color_hex(0x22c55e), 0); // same outline theme as T9 keys
  lv_obj_t *back_label = lv_label_create(wifi_pw_back_btn);
  lv_label_set_text(back_label, LV_SYMBOL_LEFT "\nBack");
  lv_obj_set_style_text_align(back_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(back_label);

  wifi_pw_kb = lv_btnmatrix_create(lv_scr_act());
  lv_btnmatrix_set_map(wifi_pw_kb, chat_t9_map);
  lv_obj_add_event_cb(wifi_pw_kb, wifiPwKeypadEventCb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_set_style_bg_opa(wifi_pw_kb, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(wifi_pw_kb, 0, 0);
  lv_obj_set_style_pad_all(wifi_pw_kb, 4, 0);
  lv_obj_set_style_pad_row(wifi_pw_kb, 6, 0);
  lv_obj_set_style_pad_column(wifi_pw_kb, 6, 0);
  lv_obj_set_style_radius(wifi_pw_kb, 10, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(wifi_pw_kb, lv_color_hex(0x1e293b), LV_PART_ITEMS);
  lv_obj_set_style_border_width(wifi_pw_kb, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(wifi_pw_kb, lv_color_hex(0x22c55e), LV_PART_ITEMS);
  lv_obj_set_style_text_color(wifi_pw_kb, lv_color_hex(0xffffff), LV_PART_ITEMS);
  lv_obj_set_size(wifi_pw_kb, 380, 202);
  lv_obj_align(wifi_pw_kb, LV_ALIGN_TOP_LEFT, 10, 113);

  showWifiListView(); // start on the list view

  // Without this, nothing built above reliably appeared until some later
  // unrelated event happened to trigger a repaint - seen before with the
  // per-item list too. Force it explicitly right after the long blocking
  // WiFi.scanNetworks() call rather than hoping the next natural tick does it.
  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(NULL);
}

// Blocking, like runTouchCalibration(): shows the setup screen and keeps
// LVGL ticking until a network connects successfully or the user skips.
// List <-> password view switching happens inside the button handlers
// above (show/hide, no rebuild) without touching g_wifiSetupDone.
bool runWifiSetupFlow() {
  // createChatScreen() always runs before this (boot fallback or the gear
  // button), so its header timer is always active at this point. Left
  // running, it would keep firing every 2s and writing into
  // chat_time_label/chat_wifi_label - destroyed by the lv_obj_clean() below
  // but the global pointers don't get nulled out, so that write lands on
  // freed memory.
  if (g_chatHeaderTimer) {
    lv_timer_del(g_chatHeaderTimer);
    g_chatHeaderTimer = nullptr;
  }

  g_wifiSetupDone = false;
  g_wifiSetupConnected = false;
  createWifiSetupScreen();
  while (!g_wifiSetupDone) {
    lv_timer_handler();
    delay(15);
  }
  return g_wifiSetupConnected;
}

// Entry point from the mode-select screen - the WiFi-connect + first-screen
// portion of examples/gemini-chatbot's original setup(), run after the
// shared driver/lvgl/calibration init that now happens once in the real
// setup() below instead of per-app.
void start() {
  Serial.println("Creating chat screen...");
  createChatScreen();
  lv_refr_now(NULL);

  // The shared setup() already tried the saved-creds + secrets.h fallback
  // (see its comment - it does this up front so andon:: gets a real clock
  // too, not just this demo) before the mode-select screen even showed.
  // Retrying the exact same creds here would just re-fail identically after
  // another ~10-20s of blocking - so either it's already connected, or the
  // only thing left to try is the on-device setup flow.
  bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  if (!wifiConnected) {
    Serial.println("No working saved WiFi - starting on-device setup...");
    wifiConnected = runWifiSetupFlow();
    createChatScreen(); // the setup screens overwrote the chat UI - rebuild it
    lv_refr_now(NULL);
  }

  if (wifiConnected) {
    Serial.println("Configuring time...");
    configTime(GMT_OFFSET, DAYLIGHT_OFFSET, NTP_SERVER);
    lv_label_set_text(chat_response_label, "Connected! Ask me anything.");
  } else {
    Serial.println("No WiFi - running in offline mode");
    lv_label_set_text(chat_response_label, "No WiFi - couldn't reach Gemini. Ask me anything once connected.");
  }
  lv_refr_now(NULL);
}

// Called from the shared loop() only while this demo is active - see the
// note on g_wifiSetupRequested's declaration above for why this can't just
// call runWifiSetupFlow() directly from the gear button's click handler.
void loopExtra() {
  if (g_wifiSetupRequested) {
    g_wifiSetupRequested = false;
    bool connected = runWifiSetupFlow();
    createChatScreen(); // the setup screens overwrote the chat UI - rebuild it
    lv_refr_now(NULL);
    lv_label_set_text(chat_response_label,
                       connected ? "WiFi updated! Ask me anything."
                                 : "Still offline - ask me anything once connected.");
  }
}

} // namespace chatapp

//------------------------------------------------------------------------------
// Digital Andon terminal demo (ported unmodified from examples/andon-system)
//------------------------------------------------------------------------------
namespace andon {

//------------------------------------------------------------------------------
// Andon UI - design tokens (design.md SS5) and layout constants
//------------------------------------------------------------------------------
// UI-ONLY BUILD: everything below drives the operator flow from a local mock
// AndonState - there is no MQTT/backend here. See ../PLAN.md for scope and
// ../design.md for the screen spec this implements. Functions that a real
// build would wire to the backend are marked TODO(backend); a few controls
// exist only to demo state transitions a technician's own device would
// normally trigger - those are marked DEMO ONLY and styled/labeled so they
// don't read as real product controls.
#define COLOR_BG_BASE         0x07131C
#define COLOR_BG_PANEL        0x10212C
#define COLOR_BG_RAISED       0x162A36
#define COLOR_BORDER          0x3E535F
#define COLOR_TEXT_PRIMARY    0xF4F7FA
#define COLOR_TEXT_SECONDARY  0xAFC0C9
#define COLOR_INFO            0x00A8F3
#define COLOR_RUNNING         0x35C759
#define COLOR_WAITING         0xF2A914
#define COLOR_FAULT           0xEF3E3E
#define COLOR_QUALITY         0x7B4BC4
#define COLOR_MATERIAL        0x1976D2
#define COLOR_DISABLED        0x52636C

#define CONTENT_W  480          // == screenWidth
#define MARGIN     8
#define GAP        8

// Hardware note: on-device testing (see examples/gemini-chatbot's history)
// found touches below roughly screenY=110-113 don't register in this
// rotation/calibration. Non-interactive elements (banners, timers, titles)
// may sit above that line; every clickable widget's top edge must not.
// The header (0-40) plus each screen's title/banner zone stay comfortably
// above it; every button/tile below is placed well past y=113 with margin.

//------------------------------------------------------------------------------
// Mock domain data (design.md SS7, PRD.md AND-002/AND-003)
//------------------------------------------------------------------------------
static const char *REASONS_MAINTENANCE[] = {
  "Machine jam", "Cutting fault", "Welding fault", "Sensor fault", "Utility", "Other"
};
// Quality/Material/Supervisor reason sets aren't specified anywhere yet (PRD.md
// AND-003 says these are plant-configurable) - these six per category are
// placeholders so all four category flows are demoable, not the real taxonomy.
static const char *REASONS_QUALITY[] = {
  "Dimension out of spec", "Surface defect", "Contamination", "Rework required", "Inspection hold", "Other"
};
static const char *REASONS_MATERIAL[] = {
  "Material shortage", "Wrong material", "Delayed delivery", "Packaging issue", "Rack full", "Other"
};
static const char *REASONS_SUPERVISOR[] = {
  "Line coordination", "Safety concern", "Schedule change", "Quality escalation", "Staffing issue", "Other"
};

struct CategoryInfo {
  const char *label;
  const char *icon; // closest LV_SYMBOL_* match - LVGL's built-in symbol set has
                     // no wrench/magnifier/box/person glyphs, see PLAN.md note
  uint32_t color;
  const char **reasons;
  uint8_t reasonCount;
};

// Order matches the reference mockup's 2x2 grid: top-left/top-right/
// bottom-left/bottom-right.
static const CategoryInfo CATEGORIES[4] = {
  {"MAINTENANCE", LV_SYMBOL_SETTINGS, COLOR_FAULT,    REASONS_MAINTENANCE, 6},
  {"QUALITY",      LV_SYMBOL_EYE_OPEN, COLOR_QUALITY,  REASONS_QUALITY,      6},
  {"MATERIAL",     LV_SYMBOL_DRIVE,    COLOR_MATERIAL, REASONS_MATERIAL,     6},
  {"SUPERVISOR",   LV_SYMBOL_CALL,     COLOR_WAITING,  REASONS_SUPERVISOR,   6},
};

//------------------------------------------------------------------------------
// Flow state (design.md SS9 state machine, held locally instead of synced
// from a backend)
//------------------------------------------------------------------------------
enum AndonScreenId {
  SCR_NORMAL,
  SCR_CATEGORY,
  SCR_REASON,
  SCR_ACTIVE,
  SCR_QUEUED_OFFLINE,
  SCR_ACKNOWLEDGED, // covers both "on the way" and "handling" sub-states
  SCR_RESOLVED,
  SCR_UPDATE_PRODUCTION, // operator-facing counter to log pipes completed
};

// Work order target stays fixed - only the completed count is editable from
// the terminal (see showScreenUpdateProduction()).
#define PRODUCTION_TARGET 120

struct AndonState {
  AndonScreenId screen = SCR_NORMAL;
  int8_t categoryIdx = -1;
  int8_t reasonIdx = -1;
  uint32_t requestOpenedMs = 0;
  bool handling = false;     // false = "on the way", true = "issue being handled"
  uint32_t downtimeSec = 0;  // frozen at resolve time for SCR-06's summary
  bool mockConnected = true; // dev-toggleable fake connectivity (long-press header)
  int productionCount = 72;  // TODO(backend): synced from the line, not operator-owned truth
};
static AndonState g_andon;

//------------------------------------------------------------------------------
// Shared UI objects
//------------------------------------------------------------------------------
static lv_obj_t *g_header = nullptr;
static lv_obj_t *g_headerTimeLabel = nullptr;
static lv_obj_t *g_headerConnDot = nullptr;
static lv_obj_t *g_content = nullptr; // rebuilt per screen; header stays put

// Live widgets the 1Hz tick updates in place (design.md SS11: "update the
// timer once per second; do not redraw the entire screen"). Null when the
// current screen doesn't have one.
static lv_obj_t *g_elapsedLabel = nullptr;
static lv_obj_t *g_waitLabel = nullptr;

// SCR_UPDATE_PRODUCTION's counter - edited value lives here, separate from
// g_andon.productionCount, so CANCEL can discard it without touching the
// committed count.
static lv_obj_t *g_productionCounterLabel = nullptr;
static int g_productionEditValue = 0;

static void showScreenNormal();
static void showScreenCategory();
static void showScreenReason(int8_t categoryIdx);
static void showScreenActive();
static void showScreenQueuedOffline();
static void showScreenAcknowledged();
static void showScreenResolved();
static void showScreenUpdateProduction();
static void updateHeaderConnDot();

//------------------------------------------------------------------------------
// Style + widget helpers (design.md SS11: centralize styling, don't rebuild
// style objects per screen transition - these are lightweight per-object
// style calls, not full lv_style_t allocations, which keeps this simple
// without the bookkeeping a full style-object cache would need at this UI's
// size)
//------------------------------------------------------------------------------
static void styleFilledBox(lv_obj_t *obj, uint32_t color, uint8_t radius) {
  lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_radius(obj, radius, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void stylePanel(lv_obj_t *obj) {
  lv_obj_set_style_bg_color(obj, lv_color_hex(COLOR_BG_PANEL), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(obj, lv_color_hex(COLOR_BORDER), 0);
  lv_obj_set_style_border_width(obj, 1, 0);
  lv_obj_set_style_radius(obj, 8, 0);
  lv_obj_set_style_pad_all(obj, 6, 0);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

// Non-interactive colored status strip with a centered label - SCR banners.
static lv_obj_t *makeBanner(lv_obj_t *parent, int x, int y, int w, int h,
                             uint32_t color, const char *text, const lv_font_t *font) {
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_set_pos(box, x, y);
  lv_obj_set_size(box, w, h);
  styleFilledBox(box, color, 8);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *label = lv_label_create(box);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_center(label);
  return box;
}

// Filled, tappable rectangle with a centered label - buttons/tiles.
static lv_obj_t *makeButton(lv_obj_t *parent, int x, int y, int w, int h,
                             uint32_t color, const char *text, const lv_font_t *font,
                             lv_event_cb_t cb, void *userData) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_size(btn, w, h);
  styleFilledBox(btn, color, 8);
  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  // Constrain to the button's width and let long text wrap instead of
  // spilling past the tile edge (was overflowing on longer reason labels).
  lv_obj_set_width(label, w - 12);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_center(label);
  if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, userData);
  return btn;
}

// Small label-over-value panel - work order/production/rate, issue/status, etc.
// Returns the panel; child 0 is the caption label, child 1 is the value label
// (used by callers that need to recolor or live-update the value).
static lv_obj_t *makeMetric(lv_obj_t *parent, int x, int y, int w, int h,
                             const char *label, const char *value) {
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_set_pos(box, x, y);
  lv_obj_set_size(box, w, h);
  stylePanel(box);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *lbl = lv_label_create(box);
  lv_label_set_text(lbl, label);
  lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_width(lbl, w - 12);
  lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t *val = lv_label_create(box);
  lv_label_set_text(val, value);
  lv_obj_set_style_text_color(val, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
  lv_obj_set_style_text_font(val, &lv_font_montserrat_16, 0);
  // Same overflow fix as makeButton() - long values (e.g. a long reason
  // string in the ISSUE box) wrap within the panel instead of running into
  // whatever sits next to it.
  lv_obj_set_width(val, w - 12);
  lv_label_set_long_mode(val, LV_LABEL_LONG_WRAP);
  lv_obj_align(val, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  return box;
}

// One bordered card holding 3 icon/label/value columns separated by thin
// dividers (SCR-01's work order/production/rate row - was 3 separate boxes,
// now a single card per the reference layout).
static void makeMetricCard3(lv_obj_t *parent, int x, int y, int w, int h,
                             const char *icon0, const char *label0, const char *value0,
                             const char *icon1, const char *label1, const char *value1,
                             const char *icon2, const char *label2, const char *value2) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, w, h);
  stylePanel(card);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

  const char *icons[3] = {icon0, icon1, icon2};
  const char *labels[3] = {label0, label1, label2};
  const char *values[3] = {value0, value1, value2};
  int colW = w / 3;
  for (int i = 0; i < 3; i++) {
    int cx = i * colW;
    if (i > 0) {
      lv_obj_t *divider = lv_obj_create(card);
      lv_obj_set_pos(divider, cx, 8);
      lv_obj_set_size(divider, 1, h - 16);
      lv_obj_set_style_bg_color(divider, lv_color_hex(COLOR_BORDER), 0);
      lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(divider, 0, 0);
      lv_obj_set_style_radius(divider, 0, 0);
      lv_obj_clear_flag(divider, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    }

    // Three stacked rows per column - icon, then caption, then value - each
    // centered within the column instead of left-aligned. Row offsets are
    // spread out for a taller card (h=124) with room to breathe.
    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, icons[i]);
    lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_INFO), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(icon, colW);
    lv_obj_set_pos(icon, cx, 16);

    lv_obj_t *cap = lv_label_create(card);
    lv_label_set_text(cap, labels[i]);
    lv_obj_set_style_text_color(cap, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(cap, colW);
    lv_obj_set_pos(cap, cx, 46);

    lv_obj_t *val = lv_label_create(card);
    lv_label_set_text(val, values[i]);
    lv_obj_set_style_text_color(val, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(val, colW - 8);
    lv_label_set_long_mode(val, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(val, cx + 4, 68);
  }
}

static void formatMMSS(uint32_t ms, char *out, size_t outLen) {
  uint32_t totalSec = ms / 1000;
  uint32_t mm = (totalSec / 60) % 100; // cap at 99:59 for display
  uint32_t ss = totalSec % 60;
  snprintf(out, outLen, "%02u:%02u", (unsigned)mm, (unsigned)ss);
}

//------------------------------------------------------------------------------
// Global header (design.md SS6) - built once, never destroyed; only its
// labels/dot get updated as screens change.
//------------------------------------------------------------------------------
static void updateHeaderConnDot() {
  if (!g_headerConnDot) return;
  lv_obj_set_style_bg_color(g_headerConnDot,
                             lv_color_hex(g_andon.mockConnected ? COLOR_RUNNING : COLOR_FAULT), 0);
}

// DEV/DEMO ONLY: long-press the header to flip the mock connectivity flag,
// so the QueuedOffline path (SCR-04B) can be demoed without real WiFi loss.
// Not part of the product UI - design.md SS11 allows "a hidden admin gesture"
// as long as it stays out of the operator flow; this is that gesture.
static void headerLongPressCb(lv_event_t *e) {
  g_andon.mockConnected = !g_andon.mockConnected;
  updateHeaderConnDot();
  Serial.printf("[demo] mockConnected toggled -> %d\r\n", g_andon.mockConnected);
}

static void buildHeader() {
  g_header = lv_obj_create(lv_scr_act());
  lv_obj_set_pos(g_header, 0, 0);
  lv_obj_set_size(g_header, CONTENT_W, 40);
  stylePanel(g_header);
  lv_obj_set_style_radius(g_header, 0, 0);
  lv_obj_set_style_pad_all(g_header, 0, 0);
  lv_obj_add_event_cb(g_header, headerLongPressCb, LV_EVENT_LONG_PRESSED, nullptr);

  lv_obj_t *station = lv_label_create(g_header);
  lv_label_set_text(station, "PIPE LINE 02");
  lv_obj_set_style_text_color(station, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
  lv_obj_set_style_text_font(station, &lv_font_montserrat_14, 0);
  lv_obj_align(station, LV_ALIGN_LEFT_MID, 6, 0);

  lv_obj_t *shift = lv_label_create(g_header);
  lv_label_set_text(shift, "SHIFT A");
  lv_obj_set_style_text_color(shift, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
  lv_obj_set_style_text_font(shift, &lv_font_montserrat_14, 0);
  lv_obj_align(shift, LV_ALIGN_LEFT_MID, 128, 0);

  // Fixed left-edge slots instead of right-anchored (LV_ALIGN_RIGHT_MID)
  // positions - right-anchoring places each element at
  // (header_right - x_ofs - actual_rendered_width), so any label whose
  // real width came out wider than assumed silently ate into its neighbor's
  // space (dot/MQTT ended up ~2px apart, clock got squeezed/clipped). Fixed
  // left edges with generous gaps make the layout independent of exact text
  // width as long as it's under each slot's reserved room.
  lv_obj_t *wifi = lv_label_create(g_header);
  lv_label_set_text(wifi, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_color(wifi, lv_color_hex(COLOR_INFO), 0);
  lv_obj_align(wifi, LV_ALIGN_LEFT_MID, 300, 0);

  g_headerConnDot = lv_obj_create(g_header);
  lv_obj_set_size(g_headerConnDot, 10, 10);
  lv_obj_set_style_radius(g_headerConnDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(g_headerConnDot, 0, 0);
  lv_obj_clear_flag(g_headerConnDot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(g_headerConnDot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(g_headerConnDot, LV_ALIGN_LEFT_MID, 330, 0);
  updateHeaderConnDot();

  lv_obj_t *mqttLabel = lv_label_create(g_header);
  lv_label_set_text(mqttLabel, "MQTT");
  lv_obj_set_style_text_color(mqttLabel, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
  lv_obj_set_style_text_font(mqttLabel, &lv_font_montserrat_14, 0);
  lv_obj_align(mqttLabel, LV_ALIGN_LEFT_MID, 346, 0);

  g_headerTimeLabel = lv_label_create(g_header);
  lv_label_set_text(g_headerTimeLabel, "08:00"); // placeholder until tickTimerCb's first tick
  lv_obj_set_style_text_color(g_headerTimeLabel, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
  lv_obj_set_style_text_font(g_headerTimeLabel, &lv_font_montserrat_14, 0);
  lv_obj_align(g_headerTimeLabel, LV_ALIGN_LEFT_MID, 420, 0);
}

static void buildContent() {
  g_content = lv_obj_create(lv_scr_act());
  lv_obj_set_pos(g_content, 0, 40);
  lv_obj_set_size(g_content, CONTENT_W, screenHeight - 40);
  lv_obj_set_style_bg_color(g_content, lv_color_hex(COLOR_BG_BASE), 0);
  lv_obj_set_style_bg_opa(g_content, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(g_content, 0, 0);
  lv_obj_set_style_radius(g_content, 0, 0);
  lv_obj_set_style_pad_all(g_content, 0, 0);
  lv_obj_clear_flag(g_content, LV_OBJ_FLAG_SCROLLABLE);
}

static void clearContent() {
  g_elapsedLabel = nullptr;
  g_waitLabel = nullptr;
  g_productionCounterLabel = nullptr;
  lv_obj_clean(g_content);
}

// 1Hz - header clock (real wall time via NTP, same source as chatapp's
// header - see the shared setup()'s boot-time WiFi/configTime() call) plus
// elapsed/wait counter for whichever screen is showing one.
static void tickTimerCb(lv_timer_t *timer) {
  time_t now = time(nullptr);
  if (now > 1000000000) { // NTP-synced (after year 2001)
    struct tm *ti = localtime(&now);
    char clockBuf[8];
    snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d", ti->tm_hour, ti->tm_min);
    lv_label_set_text(g_headerTimeLabel, clockBuf);
  } else {
    lv_label_set_text(g_headerTimeLabel, "08:00"); // no NTP sync yet/ever - dummy fallback
  }

  if (g_elapsedLabel) {
    char buf[8];
    formatMMSS(millis() - g_andon.requestOpenedMs, buf, sizeof(buf));
    lv_label_set_text(g_elapsedLabel, buf);
  }
  if (g_waitLabel) {
    char buf[8];
    formatMMSS(millis() - g_andon.requestOpenedMs, buf, sizeof(buf));
    lv_label_set_text(g_waitLabel, buf);
  }
}

//------------------------------------------------------------------------------
// Confirmation dialog (design.md SS8.1/SS8.2) - one generic modal reused for
// send and cancel confirmation.
//------------------------------------------------------------------------------
typedef void (*VoidCb)();

static lv_obj_t *g_modal = nullptr;

static void closeModal() {
  if (g_modal) {
    lv_obj_del(g_modal);
    g_modal = nullptr;
  }
}

static void modalDismissCb(lv_event_t *e) { closeModal(); }

static void modalConfirmCb(lv_event_t *e) {
  VoidCb cb = (VoidCb)lv_event_get_user_data(e);
  closeModal();
  if (cb) cb();
}

static void showConfirmDialog(const char *title, const char *message,
                               const char *dismissLabel, const char *confirmLabel,
                               uint32_t confirmColor, VoidCb onConfirm) {
  closeModal();

  g_modal = lv_obj_create(lv_scr_act());
  lv_obj_set_pos(g_modal, 0, 0);
  lv_obj_set_size(g_modal, CONTENT_W, screenHeight);
  lv_obj_set_style_bg_color(g_modal, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(g_modal, LV_OPA_70, 0);
  lv_obj_set_style_border_width(g_modal, 0, 0);
  lv_obj_set_style_radius(g_modal, 0, 0);
  lv_obj_set_style_pad_all(g_modal, 0, 0);
  lv_obj_clear_flag(g_modal, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(g_modal, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *panel = lv_obj_create(g_modal);
  lv_obj_set_pos(panel, 40, 70);
  // Tall enough to leave real margin below the button row (was exactly
  // button-bottom + padding with zero slack, so CANCEL/SEND visually
  // touched/clipped against the panel's own rounded bottom edge).
  lv_obj_set_size(panel, 400, 192);
  stylePanel(panel);
  lv_obj_set_style_pad_all(panel, 16, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *titleLbl = lv_label_create(panel);
  lv_label_set_text(titleLbl, title);
  lv_obj_set_style_text_color(titleLbl, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
  lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_22, 0);
  lv_obj_set_pos(titleLbl, 0, 0);

  lv_obj_t *msgLbl = lv_label_create(panel);
  lv_label_set_text(msgLbl, message);
  lv_obj_set_style_text_color(msgLbl, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
  lv_obj_set_style_text_font(msgLbl, &lv_font_montserrat_16, 0);
  lv_label_set_long_mode(msgLbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(msgLbl, 368);
  lv_obj_set_pos(msgLbl, 0, 32);

  int halfW = (400 - 32 - GAP) / 2;
  makeButton(panel, 0, 104, halfW, 48, COLOR_DISABLED, dismissLabel, &lv_font_montserrat_16,
             modalDismissCb, nullptr);
  makeButton(panel, halfW + GAP, 104, halfW, 48, confirmColor, confirmLabel, &lv_font_montserrat_16,
             modalConfirmCb, (void *)onConfirm);
}

//------------------------------------------------------------------------------
// Event handlers / backend-integration stubs
//------------------------------------------------------------------------------
static void onNeedAssistance(lv_event_t *e) { showScreenCategory(); }

static void onOpenUpdateProduction(lv_event_t *e) {
  g_productionEditValue = g_andon.productionCount; // edit a scratch copy so CANCEL can discard it
  showScreenUpdateProduction();
}

static void refreshProductionCounterLabel() {
  if (!g_productionCounterLabel) return;
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", g_productionEditValue);
  lv_label_set_text(g_productionCounterLabel, buf);
}

static void onProductionMinus(lv_event_t *e) {
  if (g_productionEditValue > 0) g_productionEditValue--;
  refreshProductionCounterLabel();
}

static void onProductionPlus(lv_event_t *e) {
  g_productionEditValue++;
  refreshProductionCounterLabel();
}

static void onProductionCancel(lv_event_t *e) { showScreenNormal(); } // discards g_productionEditValue

// TODO(backend): replace with MQTT publish/HTTP call to report the updated
// production count once there's somewhere to send it.
static void onProductionConfirm(lv_event_t *e) {
  g_andon.productionCount = g_productionEditValue;
  showScreenNormal();
}
static void onCategoryCancel(lv_event_t *e) { showScreenNormal(); }
static void onReasonBack(lv_event_t *e) { showScreenCategory(); }
static void onOfflineCancel(lv_event_t *e); // fwd - defined after cancelRequest()

static void onCategoryTap(lv_event_t *e) {
  intptr_t idx = (intptr_t)lv_event_get_user_data(e);
  g_andon.categoryIdx = (int8_t)idx;
  g_andon.reasonIdx = -1;
  showScreenReason((int8_t)idx);
}

static void onReasonTap(lv_event_t *e) {
  intptr_t idx = (intptr_t)lv_event_get_user_data(e);
  g_andon.reasonIdx = (int8_t)idx;
  showScreenReason(g_andon.categoryIdx); // rebuild with the new selection highlighted
}

// TODO(backend): replace with MQTT publish to andon/v1/.../request (see
// architectur.md SS8). mockConnected stands in for a real connectivity check.
static void submitRequest() {
  g_andon.requestOpenedMs = millis();
  g_andon.handling = false;
  if (g_andon.mockConnected) {
    showScreenActive();
  } else {
    showScreenQueuedOffline();
  }
}

static void onReasonSend(lv_event_t *e) {
  if (g_andon.reasonIdx < 0) return; // inert until a reason tile is selected
  const CategoryInfo &cat = CATEGORIES[g_andon.categoryIdx];
  char msg[96];
  snprintf(msg, sizeof(msg), "%s - %s", cat.label, cat.reasons[g_andon.reasonIdx]);
  showConfirmDialog("SEND REQUEST?", msg, "CANCEL", "SEND", cat.color, submitRequest);
}

// TODO(backend): replace with MQTT publish to andon/v1/.../cancel
static void cancelRequest() {
  // Only the incident-flow fields reset here - connectivity and the
  // production tally aren't part of the incident, so both would otherwise
  // silently snap back to their defaults on every cancel.
  bool wasConnected = g_andon.mockConnected;
  int production = g_andon.productionCount;
  g_andon = AndonState();
  g_andon.mockConnected = wasConnected;
  g_andon.productionCount = production;
  showScreenNormal();
}

static void onActiveCancel(lv_event_t *e) {
  showConfirmDialog("CANCEL REQUEST?", "This will withdraw the current request.",
                     "KEEP REQUEST", "CONFIRM CANCEL", COLOR_FAULT, cancelRequest);
}

static void onActiveAddNote(lv_event_t *e) {
  // A real build opens a preset-note list (design.md SS7 SCR-04) and
  // publishes it; not implemented in this UI-only pass.
  Serial.println("[demo] ADD NOTE tapped (preset-note list not implemented yet)");
}

// DEMO ONLY: stands in for a technician's own device acknowledging the
// request - the operator terminal has no real control over this.
static void onSimulateAck(lv_event_t *e) { showScreenAcknowledged(); }

static void onOfflineRetry(lv_event_t *e) {
  g_andon.mockConnected = true;
  updateHeaderConnDot();
  showScreenActive();
}

static void onOfflineCancel(lv_event_t *e) { cancelRequest(); }

// TODO(backend): replace with MQTT publish to andon/v1/.../status. Doubles
// as the demo's way to step Acknowledged -> Handling -> Resolved, since a
// real "handling" update would normally come from the technician's device.
static void onUpdateStatus(lv_event_t *e) {
  if (!g_andon.handling) {
    g_andon.handling = true;
    showScreenAcknowledged();
  } else {
    g_andon.downtimeSec = (millis() - g_andon.requestOpenedMs) / 1000;
    showScreenResolved();
  }
}

static void onReopen(lv_event_t *e) {
  // Reopened incidents resume in HANDLING per design.md SS7 SCR-06.
  g_andon.handling = true;
  showScreenAcknowledged();
}

static void closeSuccessTimerCb(lv_timer_t *timer) {
  lv_timer_del(timer);
  showScreenNormal();
}

// TODO(backend): replace with MQTT publish to andon/v1/.../confirm-run
static void onConfirmRun(lv_event_t *e) {
  bool wasConnected = g_andon.mockConnected;
  int production = g_andon.productionCount;
  g_andon = AndonState();
  g_andon.mockConnected = wasConnected;
  g_andon.productionCount = production;

  // design.md SS7 SCR-06: "show a two-second success state, then return to
  // SCR-01" - not a real screen enum, just a transient checkmark.
  clearContent();
  lv_obj_t *check = lv_label_create(g_content);
  lv_label_set_text(check, LV_SYMBOL_OK "  LINE READY");
  lv_obj_set_style_text_color(check, lv_color_hex(COLOR_RUNNING), 0);
  lv_obj_set_style_text_font(check, &lv_font_montserrat_32, 0);
  lv_obj_center(check);
  lv_timer_create(closeSuccessTimerCb, 2000, nullptr);
}

//------------------------------------------------------------------------------
// Screens (design.md SS7)
//------------------------------------------------------------------------------
// SCR-01 - Normal status
static void showScreenNormal() {
  g_andon.screen = SCR_NORMAL;
  clearContent();

  // Banner : card : button height ratio is roughly 1:2:1 (62:124:62, with
  // 8px margins/gaps eating the rest of the 280px content height) - the
  // card was cramped relative to the other two blocks before.
  if (g_andon.mockConnected) {
    makeBanner(g_content, MARGIN, 8, CONTENT_W - 2 * MARGIN, 62, COLOR_RUNNING,
               LV_SYMBOL_OK "  LINE RUNNING", &lv_font_montserrat_28);
  } else {
    makeBanner(g_content, MARGIN, 8, CONTENT_W - 2 * MARGIN, 62, COLOR_WAITING,
               LV_SYMBOL_WARNING "  OFFLINE - LOCAL MODE", &lv_font_montserrat_22);
  }

  // One card, 3 columns (was 3 separate boxes) - matches the reference layout.
  char productionText[16];
  snprintf(productionText, sizeof(productionText), "%d / %d", g_andon.productionCount, PRODUCTION_TARGET);
  makeMetricCard3(g_content, MARGIN, 78, CONTENT_W - 2 * MARGIN, 124,
                   LV_SYMBOL_LIST, "WORK ORDER", "WO-240811-07",
                   LV_SYMBOL_DRIVE, "PRODUCTION", productionText,
                   LV_SYMBOL_CHARGE, "RATE", "18 pcs/h");

  // UPDATE PRODUCTION : NEED ASSISTANCE is a 2:1 width split (was
  // NEED ASSISTANCE alone, full width).
  int unit = (CONTENT_W - 2 * MARGIN - GAP) / 3;
  int updateW = unit * 2;
  int assistW = unit;
  makeButton(g_content, MARGIN, 210, updateW, 62, COLOR_MATERIAL,
             LV_SYMBOL_EDIT "  UPDATE PRODUCTION", &lv_font_montserrat_16, onOpenUpdateProduction, nullptr);
  // Amber, not red - red is reserved for "there's an active fault" states
  // elsewhere (MAINTENANCE CALLED banner, STATUS: OPEN, etc.); using it here
  // too would make this entry point look like a fault already exists before
  // the operator has even tapped it.
  makeButton(g_content, MARGIN + updateW + GAP, 210, assistW, 62, COLOR_WAITING,
             LV_SYMBOL_BELL "  NEED\nASSISTANCE", &lv_font_montserrat_14, onNeedAssistance, nullptr);
}

// Update production count - reached from SCR-01's UPDATE PRODUCTION button.
// Not one of design.md's numbered screens (this is new scope, see the
// morning's conversation) but follows the same "no typing" philosophy as
// the rest of the flow: a big counter plus +/- steppers instead of a
// keyboard. Tap = +/-1; hold (LV_EVENT_LONG_PRESSED_REPEAT, built into
// LVGL's indev - no custom repeat timer needed) auto-repeats for fast
// adjustment over a larger range.
static void showScreenUpdateProduction() {
  clearContent();

  lv_obj_t *title = lv_label_create(g_content);
  lv_label_set_text(title, "UPDATE PRODUCTION");
  lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  lv_obj_t *subtitle = lv_label_create(g_content);
  char subtitleText[32];
  snprintf(subtitleText, sizeof(subtitleText), "Target: %d pipes", PRODUCTION_TARGET);
  lv_label_set_text(subtitle, subtitleText);
  lv_obj_set_style_text_color(subtitle, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
  lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_12, 0);
  lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 38);

  const int rowY = 66, rowH = 120, sideW = 100;
  lv_obj_t *minusBtn = makeButton(g_content, MARGIN, rowY, sideW, rowH, COLOR_BG_RAISED,
                                   LV_SYMBOL_MINUS, &lv_font_montserrat_28, onProductionMinus, nullptr);
  lv_obj_add_event_cb(minusBtn, onProductionMinus, LV_EVENT_LONG_PRESSED_REPEAT, nullptr);

  lv_obj_t *plusBtn = makeButton(g_content, CONTENT_W - MARGIN - sideW, rowY, sideW, rowH, COLOR_BG_RAISED,
                                  LV_SYMBOL_PLUS, &lv_font_montserrat_28, onProductionPlus, nullptr);
  lv_obj_add_event_cb(plusBtn, onProductionPlus, LV_EVENT_LONG_PRESSED_REPEAT, nullptr);

  g_productionCounterLabel = lv_label_create(g_content);
  lv_obj_set_style_text_color(g_productionCounterLabel, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
  lv_obj_set_style_text_font(g_productionCounterLabel, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_align(g_productionCounterLabel, LV_TEXT_ALIGN_CENTER, 0);
  int counterX = MARGIN + sideW + GAP;
  lv_obj_set_width(g_productionCounterLabel, CONTENT_W - 2 * (MARGIN + sideW + GAP));
  lv_obj_set_pos(g_productionCounterLabel, counterX, rowY + (rowH - 56) / 2);
  refreshProductionCounterLabel();

  int halfW = (CONTENT_W - 2 * MARGIN - GAP) / 2;
  makeButton(g_content, MARGIN, 210, halfW, 56, COLOR_DISABLED,
             LV_SYMBOL_LEFT "  CANCEL", &lv_font_montserrat_18, onProductionCancel, nullptr);
  makeButton(g_content, MARGIN + halfW + GAP, 210, halfW, 56, COLOR_RUNNING,
             LV_SYMBOL_OK "  CONFIRM", &lv_font_montserrat_18, onProductionConfirm, nullptr);
}

// SCR-02 - Category selection
static void showScreenCategory() {
  g_andon.screen = SCR_CATEGORY;
  clearContent();

  lv_obj_t *title = lv_label_create(g_content);
  lv_label_set_text(title, "CALL ANDON");
  lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

  const int gridTop = 48; // snug under the title, not floating with a big gap
  int tileW = (CONTENT_W - 2 * MARGIN - GAP) / 2;
  const int rowH = 76; // taller tiles now that the gap above them is gone
  for (int i = 0; i < 4; i++) {
    int col = i % 2, row = i / 2;
    int x = MARGIN + col * (tileW + GAP);
    int y = gridTop + row * (rowH + GAP);
    char tileText[40];
    snprintf(tileText, sizeof(tileText), "%s\n%s", CATEGORIES[i].icon, CATEGORIES[i].label);
    makeButton(g_content, x, y, tileW, rowH, CATEGORIES[i].color, tileText,
               &lv_font_montserrat_18, onCategoryTap, (void *)(intptr_t)i);
  }

  makeButton(g_content, MARGIN, gridTop + 2 * rowH + 2 * GAP, CONTENT_W - 2 * MARGIN, 48, COLOR_DISABLED,
             LV_SYMBOL_CLOSE "  CANCEL", &lv_font_montserrat_18, onCategoryCancel, nullptr);
}

// SCR-03 - Reason selection
static void showScreenReason(int8_t categoryIdx) {
  g_andon.screen = SCR_REASON;
  g_andon.categoryIdx = categoryIdx;
  clearContent();
  const CategoryInfo &cat = CATEGORIES[categoryIdx];

  lv_obj_t *title = lv_label_create(g_content);
  lv_label_set_text(title, cat.label);
  lv_obj_set_style_text_color(title, lv_color_hex(cat.color), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

  const int gridTop = 48;
  const int colW = (CONTENT_W - 2 * MARGIN - 2 * GAP) / 3;
  const int rowH = 70;
  for (int i = 0; i < cat.reasonCount && i < 6; i++) {
    int col = i % 3, row = i / 3;
    int x = MARGIN + col * (colW + GAP);
    int y = gridTop + row * (rowH + GAP);
    bool selected = (g_andon.reasonIdx == i);
    // Reuses the category's icon on every tile - there's no per-reason icon
    // set defined anywhere (these reason lists are placeholders to begin
    // with, see the REASONS_* comment above), so this at least matches the
    // reference's "icon + label" tile look instead of text-only tiles.
    char tileText[40];
    snprintf(tileText, sizeof(tileText), "%s  %s", cat.icon, cat.reasons[i]);
    lv_obj_t *btn = makeButton(g_content, x, y, colW, rowH, selected ? cat.color : COLOR_BG_RAISED,
                                tileText, &lv_font_montserrat_12, onReasonTap, (void *)(intptr_t)i);
    if (selected) {
      lv_obj_set_style_border_width(btn, 2, 0);
      lv_obj_set_style_border_color(btn, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    }
  }

  int actionY = gridTop + 2 * rowH + 2 * GAP;
  int halfW = (CONTENT_W - 2 * MARGIN - GAP) / 2;
  makeButton(g_content, MARGIN, actionY, halfW, 56, COLOR_DISABLED,
             LV_SYMBOL_LEFT "  BACK", &lv_font_montserrat_18, onReasonBack, nullptr);

  bool canSend = g_andon.reasonIdx >= 0;
  makeButton(g_content, MARGIN + halfW + GAP, actionY, halfW, 56, canSend ? cat.color : COLOR_DISABLED,
             LV_SYMBOL_OK "  SEND REQUEST", &lv_font_montserrat_18, onReasonSend, nullptr);
}

// SCR-04 - Request active
static void showScreenActive() {
  g_andon.screen = SCR_ACTIVE;
  clearContent();
  const CategoryInfo &cat = CATEGORIES[g_andon.categoryIdx];

  char bannerText[48];
  snprintf(bannerText, sizeof(bannerText), LV_SYMBOL_BELL "  %s CALLED", cat.label);
  makeBanner(g_content, MARGIN, 8, CONTENT_W - 2 * MARGIN, 48, cat.color, bannerText, &lv_font_montserrat_24);

  g_elapsedLabel = lv_label_create(g_content);
  lv_label_set_text(g_elapsedLabel, "00:00");
  lv_obj_set_style_text_color(g_elapsedLabel, lv_color_hex(cat.color), 0);
  lv_obj_set_style_text_font(g_elapsedLabel, &lv_font_montserrat_48, 0);
  lv_obj_align(g_elapsedLabel, LV_ALIGN_TOP_MID, 0, 60);

  lv_obj_t *caption = lv_label_create(g_content);
  lv_label_set_text(caption, "ELAPSED TIME");
  lv_obj_set_style_text_color(caption, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
  lv_obj_set_style_text_font(caption, &lv_font_montserrat_12, 0);
  lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, 114);

  // Taller than a single text line (was 40px) so a long ISSUE reason string
  // wraps inside its own box instead of running into STATUS next to it.
  int halfW = (CONTENT_W - 2 * MARGIN - GAP) / 2;
  makeMetric(g_content, MARGIN, 134, halfW, 52, LV_SYMBOL_SETTINGS "  ISSUE", cat.reasons[g_andon.reasonIdx]);
  lv_obj_t *statusBox = makeMetric(g_content, MARGIN + halfW + GAP, 134, halfW, 52, LV_SYMBOL_LIST "  STATUS", "OPEN");
  lv_obj_t *statusVal = lv_obj_get_child(statusBox, 1);
  if (statusVal) lv_obj_set_style_text_color(statusVal, lv_color_hex(COLOR_FAULT), 0);

  makeButton(g_content, MARGIN, 194, halfW, 40, COLOR_DISABLED,
             LV_SYMBOL_CLOSE "  CANCEL REQUEST", &lv_font_montserrat_16, onActiveCancel, nullptr);
  makeButton(g_content, MARGIN + halfW + GAP, 194, halfW, 40, COLOR_DISABLED,
             LV_SYMBOL_EDIT "  ADD NOTE", &lv_font_montserrat_16, onActiveAddNote, nullptr);

  lv_obj_t *demoBtn = makeButton(g_content, (CONTENT_W - 280) / 2, 240, 280, 36, COLOR_BG_RAISED,
                                  "(DEMO) SIMULATE TECHNICIAN ACK", &lv_font_montserrat_12, onSimulateAck, nullptr);
  lv_obj_set_style_border_width(demoBtn, 1, 0);
  lv_obj_set_style_border_color(demoBtn, lv_color_hex(COLOR_DISABLED), 0);
}

// SCR-04B - Queued offline
static void showScreenQueuedOffline() {
  g_andon.screen = SCR_QUEUED_OFFLINE;
  clearContent();
  const CategoryInfo &cat = CATEGORIES[g_andon.categoryIdx];

  makeBanner(g_content, MARGIN, 8, CONTENT_W - 2 * MARGIN, 48, COLOR_WAITING,
             LV_SYMBOL_WARNING "  QUEUED OFFLINE", &lv_font_montserrat_22);

  char summary[64];
  snprintf(summary, sizeof(summary), "%s - %s", cat.label, cat.reasons[g_andon.reasonIdx]);
  lv_obj_t *summaryLbl = lv_label_create(g_content);
  lv_label_set_text(summaryLbl, summary);
  lv_obj_set_style_text_color(summaryLbl, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
  lv_obj_set_style_text_font(summaryLbl, &lv_font_montserrat_18, 0);
  lv_obj_align(summaryLbl, LV_ALIGN_TOP_MID, 0, 64);

  lv_obj_t *note = lv_label_create(g_content);
  lv_label_set_text(note, "Request will be sent when connection returns");
  lv_obj_set_style_text_color(note, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
  lv_obj_set_style_text_font(note, &lv_font_montserrat_14, 0);
  lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(note, CONTENT_W - 2 * MARGIN);
  lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 94);

  g_elapsedLabel = lv_label_create(g_content);
  lv_label_set_text(g_elapsedLabel, "00:00");
  lv_obj_set_style_text_color(g_elapsedLabel, lv_color_hex(COLOR_WAITING), 0);
  lv_obj_set_style_text_font(g_elapsedLabel, &lv_font_montserrat_32, 0);
  lv_obj_align(g_elapsedLabel, LV_ALIGN_TOP_MID, 0, 136);

  int halfW = (CONTENT_W - 2 * MARGIN - GAP) / 2;
  makeButton(g_content, MARGIN, 200, halfW, 56, COLOR_DISABLED,
             LV_SYMBOL_CLOSE "  CANCEL LOCAL", &lv_font_montserrat_16, onOfflineCancel, nullptr);
  makeButton(g_content, MARGIN + halfW + GAP, 200, halfW, 56, COLOR_WAITING,
             LV_SYMBOL_REFRESH "  RETRY NOW", &lv_font_montserrat_16, onOfflineRetry, nullptr);
}

// SCR-05 - Acknowledged / handling
static void showScreenAcknowledged() {
  g_andon.screen = SCR_ACKNOWLEDGED;
  clearContent();

  const char *bannerText = g_andon.handling ? LV_SYMBOL_REFRESH "  ISSUE BEING HANDLED"
                                             : LV_SYMBOL_CALL "  TECHNICIAN ON THE WAY";
  makeBanner(g_content, MARGIN, 8, CONTENT_W - 2 * MARGIN, 48, COLOR_WAITING, bannerText, &lv_font_montserrat_20);

  lv_obj_t *respCard = lv_obj_create(g_content);
  lv_obj_set_pos(respCard, MARGIN, 64);
  lv_obj_set_size(respCard, CONTENT_W - 2 * MARGIN, 48);
  stylePanel(respCard);
  lv_obj_clear_flag(respCard, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *respLbl = lv_label_create(respCard);
  lv_label_set_text(respLbl, LV_SYMBOL_CALL "  BUDI  *  MT-04");
  lv_obj_set_style_text_color(respLbl, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
  lv_obj_set_style_text_font(respLbl, &lv_font_montserrat_16, 0);
  lv_obj_center(respLbl);

  // Stepper: OPEN -> ACK -> HANDLING (design.md SS7 SCR-05). OPEN/ACK are
  // always satisfied by the time this screen shows, so they're always
  // marked done; HANDLING turns green/checked too once UPDATE STATUS has
  // flipped g_andon.handling (banner changes to "ISSUE BEING HANDLED" at
  // the same time) - it's the reached/current step at that point, not still
  // pending. (Earlier this stayed neutral even while active, which read as
  // "never reached" - wrong once handling is actually true.)
  const char *steps[3] = {"OPEN", "ACK", "HANDLING"};
  int stepW = (CONTENT_W - 2 * MARGIN - 2 * GAP) / 3;
  for (int i = 0; i < 3; i++) {
    int x = MARGIN + i * (stepW + GAP);
    bool done = (i < 2) || g_andon.handling;
    lv_obj_t *step = lv_obj_create(g_content);
    lv_obj_set_pos(step, x, 120);
    lv_obj_set_size(step, stepW, 36);
    styleFilledBox(step, done ? COLOR_RUNNING : COLOR_BG_RAISED, 6);
    lv_obj_clear_flag(step, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *stepLbl = lv_label_create(step);
    char stepText[16];
    snprintf(stepText, sizeof(stepText), done ? LV_SYMBOL_OK " %s" : "%s", steps[i]);
    lv_label_set_text(stepLbl, stepText);
    lv_obj_set_style_text_color(stepLbl, lv_color_hex(done ? COLOR_BG_BASE : COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(stepLbl, &lv_font_montserrat_14, 0);
    lv_obj_center(stepLbl);
  }

  // h=40 was too short for a top-anchored caption + bottom-anchored value at
  // this font size - they overlapped ("bertumpuk"). Same fix as SCR-04's
  // ISSUE/STATUS boxes: give the panel more height.
  int halfW = (CONTENT_W - 2 * MARGIN - GAP) / 2;
  makeMetric(g_content, MARGIN, 160, halfW, 52, "RESPONSE", "01:12"); // mock/static, not live
  lv_obj_t *waitBox = makeMetric(g_content, MARGIN + halfW + GAP, 160, halfW, 52, "WAITING", "00:00");
  g_waitLabel = lv_obj_get_child(waitBox, 1);

  makeButton(g_content, MARGIN, 218, CONTENT_W - 2 * MARGIN, 56, COLOR_WAITING,
             LV_SYMBOL_REFRESH "  UPDATE STATUS", &lv_font_montserrat_18, onUpdateStatus, nullptr);
}

// SCR-06 - Resolved confirmation
static void showScreenResolved() {
  g_andon.screen = SCR_RESOLVED;
  clearContent();
  const CategoryInfo &cat = CATEGORIES[g_andon.categoryIdx];

  makeBanner(g_content, MARGIN, 8, CONTENT_W - 2 * MARGIN, 48, COLOR_RUNNING,
             LV_SYMBOL_OK "  ISSUE RESOLVED", &lv_font_montserrat_24);

  char downtimeStr[8];
  formatMMSS(g_andon.downtimeSec * 1000UL, downtimeStr, sizeof(downtimeStr));

  lv_obj_t *summary = lv_obj_create(g_content);
  lv_obj_set_pos(summary, MARGIN, 64);
  lv_obj_set_size(summary, CONTENT_W - 2 * MARGIN, 96);
  stylePanel(summary);
  lv_obj_clear_flag(summary, LV_OBJ_FLAG_CLICKABLE);

  char line1[40], line2[40], line3[64];
  snprintf(line1, sizeof(line1), LV_SYMBOL_REFRESH "  DOWNTIME    %s", downtimeStr);
  snprintf(line2, sizeof(line2), LV_SYMBOL_CALL "  HANDLED BY  BUDI");
  snprintf(line3, sizeof(line3), LV_SYMBOL_EDIT "  DETAIL      %s reset", cat.reasons[g_andon.reasonIdx]);
  const char *lines[3] = {line1, line2, line3};
  for (int i = 0; i < 3; i++) {
    lv_obj_t *l = lv_label_create(summary);
    lv_label_set_text(l, lines[i]);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(l, 8, 6 + i * 28);
  }

  lv_obj_t *question = lv_label_create(g_content);
  lv_label_set_text(question, "MACHINE READY TO RUN?");
  lv_obj_set_style_text_color(question, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
  lv_obj_set_style_text_font(question, &lv_font_montserrat_16, 0);
  lv_obj_align(question, LV_ALIGN_TOP_MID, 0, 170);

  int halfW = (CONTENT_W - 2 * MARGIN - GAP) / 2;
  makeButton(g_content, MARGIN, 208, halfW, 56, COLOR_DISABLED,
             LV_SYMBOL_LOOP "  REOPEN", &lv_font_montserrat_16, onReopen, nullptr);
  makeButton(g_content, MARGIN + halfW + GAP, 208, halfW, 56, COLOR_RUNNING,
             LV_SYMBOL_PLAY "  CONFIRM & RUN", &lv_font_montserrat_16, onConfirmRun, nullptr);
}


// Entry point from the mode-select screen - what examples/andon-system's
// own setup() did after the (now-shared) driver/lvgl/calibration init.
void start() {
  // mockConnected intentionally does NOT follow real WiFi status - it's a
  // demo-only toggle for showing the SCR-04B offline path on purpose (see
  // headerLongPressCb()), independent from whether the header clock is
  // real (tickTimerCb checks NTP directly). Defaulting it to the real WiFi
  // state would make this demo boot into "OFFLINE - LOCAL MODE" instead of
  // the polished "LINE RUNNING" screen whenever the venue's WiFi is flaky -
  // exactly the wrong first impression for a demo.
  buildHeader();
  buildContent();
  showScreenNormal();
  lv_refr_now(NULL);
  lv_timer_create(tickTimerCb, 1000, nullptr);
}

} // namespace andon

//------------------------------------------------------------------------------
// Mode select screen - shown once, right after calibration
//------------------------------------------------------------------------------
void onSelectChat(lv_event_t *e) {
  g_mode = MODE_CHAT;
  chatapp::start();
}

void onSelectAndon(lv_event_t *e) {
  g_mode = MODE_ANDON;
  andon::start();
}

void showModeSelectScreen() {
  lv_obj_clean(lv_scr_act());
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0f172a), 0);

  lv_obj_t *title = lv_label_create(lv_scr_act());
  lv_label_set_text(title, "SELECT DEMO");
  lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

  // Explicit y=130 (well past the confirmed touch dead zone below y~113,
  // see runTouchCalibration()'s P1Y) rather than LV_ALIGN_*_MID + an
  // offset, so it's unambiguous both buttons are fully in the safe zone.
  lv_obj_t *chatBtn = lv_btn_create(lv_scr_act());
  lv_obj_set_pos(chatBtn, 40, 130);
  lv_obj_set_size(chatBtn, 190, 160);
  lv_obj_set_style_bg_color(chatBtn, lv_color_hex(0x22c55e), 0);
  lv_obj_set_style_radius(chatBtn, 12, 0);
  lv_obj_add_event_cb(chatBtn, onSelectChat, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *chatLbl = lv_label_create(chatBtn);
  lv_label_set_text(chatLbl, LV_SYMBOL_CALL "\n\nGEMINI\nCHATBOT");
  lv_obj_set_style_text_align(chatLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(chatLbl, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_font(chatLbl, &lv_font_montserrat_20, 0);
  lv_obj_center(chatLbl);

  lv_obj_t *andonBtn = lv_btn_create(lv_scr_act());
  lv_obj_set_pos(andonBtn, 250, 130);
  lv_obj_set_size(andonBtn, 190, 160);
  lv_obj_set_style_bg_color(andonBtn, lv_color_hex(0x00A8F3), 0);
  lv_obj_set_style_radius(andonBtn, 12, 0);
  lv_obj_add_event_cb(andonBtn, onSelectAndon, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *andonLbl = lv_label_create(andonBtn);
  lv_label_set_text(andonLbl, LV_SYMBOL_BELL "\n\nANDON\nSYSTEM");
  lv_obj_set_style_text_align(andonLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(andonLbl, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_font(andonLbl, &lv_font_montserrat_20, 0);
  lv_obj_center(andonLbl);
}

void setup() {
  Serial.begin(115200);
  Serial.println("Starting ESP32-3248S035 Demo Hub...");

  // TEMP/DEBUG: print why the chip actually reset - see gemini-chatbot's
  // history for why this earns its keep (a hung boot and a fresh
  // power-on look identical to the user otherwise).
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  Serial.println("Reset reason: POWERON (normal power-on)"); break;
    case ESP_RST_SW:       Serial.println("Reset reason: SW (esp_restart() called)"); break;
    case ESP_RST_PANIC:    Serial.println("Reset reason: PANIC - crashed last boot!"); break;
    case ESP_RST_INT_WDT:  Serial.println("Reset reason: INT_WDT - interrupt watchdog!"); break;
    case ESP_RST_TASK_WDT: Serial.println("Reset reason: TASK_WDT - code hung last boot!"); break;
    case ESP_RST_WDT:      Serial.println("Reset reason: WDT - other watchdog reset!"); break;
    case ESP_RST_BROWNOUT: Serial.println("Reset reason: BROWNOUT - power dipped!"); break;
    default:                Serial.printf("Reset reason: %d (see esp_reset_reason_t)\r\n", (int)esp_reset_reason()); break;
  }

  gt911_init();

  lv_init();
#if LV_USE_LOG != 0
  lv_log_register_print_cb(my_print);
#endif

  tft.begin();
  tft.setRotation(1); // landscape
  tft.fillScreen(TFT_BLACK);

  lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 10);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  Serial.println("Running touch calibration...");
  runTouchCalibration();

  // Connect WiFi once, up front, shared by both demos - so andon::'s clock
  // can be real (NTP) too, not just chatapp::'s. Same saved-creds-then-
  // secrets.h fallback chatapp::start() used to do entirely on its own;
  // it still runs its own on-device setup flow afterward if this didn't
  // manage to connect (see chatapp::start()).
  lv_obj_clean(lv_scr_act());
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0f172a), 0);
  lv_obj_t *connLabel = lv_label_create(lv_scr_act());
  lv_label_set_text(connLabel, "Connecting to WiFi...");
  lv_obj_set_style_text_color(connLabel, lv_color_hex(0xffffff), 0);
  lv_obj_center(connLabel);
  lv_refr_now(NULL);

  bool wifiConnected = false;
  String savedSsid, savedPass;
  if (chatapp::loadSavedWifi(savedSsid, savedPass)) {
    wifiConnected = chatapp::tryConnectWifi(savedSsid, savedPass, connLabel);
  }
  if (!wifiConnected && strlen(WIFI_SSID) > 0) {
    wifiConnected = chatapp::tryConnectWifi(WIFI_SSID, WIFI_PASSWORD, connLabel);
    if (wifiConnected) chatapp::saveWifi(WIFI_SSID, WIFI_PASSWORD);
  }
  if (wifiConnected) {
    Serial.println("WiFi connected - configuring time...");
    configTime(chatapp::GMT_OFFSET, chatapp::DAYLIGHT_OFFSET, chatapp::NTP_SERVER);
  } else {
    Serial.println("No WiFi at boot - each demo can still open its own setup flow.");
  }

  showModeSelectScreen();
  lv_refr_now(NULL);

  Serial.println("Setup done - pick a demo.");
}

void loop() {
  lv_timer_handler();

  // Only the chat demo needs anything extra done here (see loopExtra()'s
  // comment) - the andon demo's per-second work runs entirely off its own
  // lv_timer_create() timer, same as everything else lv_timer_handler()
  // already drives.
  if (g_mode == MODE_CHAT) chatapp::loopExtra();
  delay(10);
}
