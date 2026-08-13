// ESP32-3248S035C - Andon system (WIP)
//
// Fresh start for a new application on the same board as
// examples/gemini-chatbot/. Carried over from there: the GT911 bit-banged
// touch driver and the on-device 2-point touch calibration flow (both
// hard-won - see notes inline). Everything app-specific (chat UI, T9
// keypad, WiFi manager, Gemini calls) was deliberately left behind; build
// the andon UI from here.

#include <Arduino.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <WiFi.h> // WiFi.status() - real header connectivity indicator, see updateHeaderConnDot()
#include <time.h> // struct tm/getLocalTime/strftime - NTP header clock, see tickTimerCb()

#include "andon_config.hpp" // reason-list sync from the backend - see setup(), andon_config.cpp
#include "andon_wifi.hpp" // on-device WiFi setup screen - see onOpenConfig(), loop()
#include "andon_mqtt.hpp" // Send Request submission - see submitRequest()

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

// Machine codes for the reasons above, same order/index - what
// AndonMqtt::submitAndonRequest() actually sends as reasonCode
// (architectur.md SS8.2); the arrays above are just what SCR-03 displays.
// Matches contracts/http/v1/get-configuration-stations.example.json's
// codes exactly, so a live AndonConfig::sync() fetch (which DOES carry
// real codes from the backend - see andon_config.cpp) and this hardcoded
// placeholder agree on the same taxonomy.
static const char *REASON_CODES_MAINTENANCE[] = {
  "MACHINE_JAM", "CUTTING_FAULT", "WELDING_FAULT", "SENSOR_FAULT", "UTILITY", "OTHER"
};
static const char *REASON_CODES_QUALITY[] = {
  "DIMENSION_OUT_OF_SPEC", "SURFACE_DEFECT", "CONTAMINATION", "REWORK_REQUIRED", "INSPECTION_HOLD", "OTHER"
};
static const char *REASON_CODES_MATERIAL[] = {
  "MATERIAL_SHORTAGE", "WRONG_MATERIAL", "DELAYED_DELIVERY", "PACKAGING_ISSUE", "RACK_FULL", "OTHER"
};
static const char *REASON_CODES_SUPERVISOR[] = {
  "LINE_COORDINATION", "SAFETY_CONCERN", "SCHEDULE_CHANGE", "QUALITY_ESCALATION", "STAFFING_ISSUE", "OTHER"
};

// CategoryInfo itself lives in andon_types.hpp now (shared with
// andon_config.cpp - see andon_config.hpp), not defined here anymore.
#include "andon_types.hpp"

// Order matches the reference mockup's 2x2 grid: top-left/top-right/
// bottom-left/bottom-right. Deliberately NOT `static const` (unlike every
// other data table in this file): AndonConfig::sync() (see
// andon_config.cpp) rewrites a matched category's .reasons/.reasonCodes/
// .reasonCount in place after fetching PRD.md AND-003's plant-configurable
// reason list - PRD.md AND-002 fixes everything else about these four
// categories (count, label/code, icon, color), so only those three fields
// ever change at runtime.
CategoryInfo CATEGORIES[4] = {
  {"MAINTENANCE", LV_SYMBOL_SETTINGS, COLOR_FAULT,    REASONS_MAINTENANCE, REASON_CODES_MAINTENANCE, 6},
  {"QUALITY",      LV_SYMBOL_EYE_OPEN, COLOR_QUALITY,  REASONS_QUALITY,     REASON_CODES_QUALITY,     6},
  {"MATERIAL",     LV_SYMBOL_DRIVE,    COLOR_MATERIAL, REASONS_MATERIAL,    REASON_CODES_MATERIAL,    6},
  {"SUPERVISOR",   LV_SYMBOL_CALL,     COLOR_WAITING,  REASONS_SUPERVISOR,  REASON_CODES_SUPERVISOR,  6},
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
  String incidentId = "";    // backend-assigned - set once AndonMqtt::submitAndonRequest() actually returns ACCEPTED (see submitRequest()); empty while queued/offline
};
static AndonState g_andon;

//------------------------------------------------------------------------------
// Shared UI objects
//------------------------------------------------------------------------------
static lv_obj_t *g_header = nullptr;
static lv_obj_t *g_headerTimeLabel = nullptr;
static lv_obj_t *g_headerWifiIcon = nullptr;
static lv_obj_t *g_headerConnDot = nullptr;
static lv_obj_t *g_content = nullptr; // rebuilt per screen; header stays put

// Handle for tickTimerCb's 1Hz lv_timer_create() below (see setup()) - kept
// so loop() can delete it before AndonWifi::runSetupFlow() wipes the whole
// screen (header included). Without this, the timer kept firing during the
// WiFi setup screen and wrote into g_headerConnDot/g_headerTimeLabel after
// they'd already been deleted by that wipe - a dangling-pointer crash
// (Guru Meditation Error, LoadProhibited) reproducible every time the WiFi
// list populated after a scan. gemini-chatbot hit and fixed this exact bug
// for its own header timer (g_chatHeaderTimer) - this wasn't carried over
// when the WiFi manager got ported to andon-system.
static lv_timer_t *g_tickTimer = nullptr;

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
// Real WiFi status now that AndonWifi exists (see andon_wifi.cpp) - was
// previously tied to g_andon.mockConnected, same as the LINE RUNNING/
// OFFLINE banner. Those are different concepts (this is raw radio-level
// connectivity; mockConnected is "backend/incident state permits LINE
// RUNNING", still mocked since there's no MQTT backend yet - see
// showScreenNormal()) - deliberately decoupled: WiFi being up doesn't mean
// no incident is blocking the line, and vice versa isn't true either.
static void updateHeaderConnDot() {
  bool wifiUp = (WiFi.status() == WL_CONNECTED);
  if (g_headerConnDot) {
    lv_obj_set_style_bg_color(g_headerConnDot, lv_color_hex(wifiUp ? COLOR_RUNNING : COLOR_FAULT), 0);
  }
  if (g_headerWifiIcon) {
    lv_obj_set_style_text_color(g_headerWifiIcon, lv_color_hex(wifiUp ? COLOR_RUNNING : COLOR_FAULT), 0);
  }
}

// DEV/DEMO ONLY: long-press the header to flip the mock connectivity flag,
// so the QueuedOffline path (SCR-04B) can be demoed without real WiFi loss.
// Not part of the product UI - design.md SS11 allows "a hidden admin gesture"
// as long as it stays out of the operator flow; this is that gesture. No
// longer touches the header dot/WiFi icon (see updateHeaderConnDot()'s
// comment) - only the LINE RUNNING/OFFLINE banner responds to this now.
static void headerLongPressCb(lv_event_t *e) {
  g_andon.mockConnected = !g_andon.mockConnected;
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
  g_headerWifiIcon = lv_label_create(g_header);
  lv_label_set_text(g_headerWifiIcon, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_color(g_headerWifiIcon, lv_color_hex(COLOR_INFO), 0);
  lv_obj_align(g_headerWifiIcon, LV_ALIGN_LEFT_MID, 300, 0);

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
  lv_label_set_text(g_headerTimeLabel, "--:--"); // until NTP syncs - see tickTimerCb()
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

// 1Hz - elapsed/wait counter for whichever screen is showing one, the
// header WiFi/connectivity indicators, and the NTP-synced header clock
// (see andon_wifi.cpp's configTime() call).
static void tickTimerCb(lv_timer_t *timer) {
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

  // Real WiFi status (was only refreshed right after explicit connect
  // attempts - this catches drops/reconnects that happen in between too).
  updateHeaderConnDot();

  // NTP-synced clock (see andon_wifi.cpp's configTime() call). A tiny
  // timeout (not the 5s default) so a not-yet-synced/no-WiFi state can
  // never stall this 1Hz timer callback - on failure the label just keeps
  // showing whatever it last had (starts as "--:--" from buildHeader()).
  struct tm timeinfo;
  if (g_headerTimeLabel && getLocalTime(&timeinfo, 5)) {
    char buf[6];
    strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
    lv_label_set_text(g_headerTimeLabel, buf);
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

// Gear button on SCR-01 -> WiFi setup, ported from examples/gemini-chatbot
// (see andon_wifi.hpp/.cpp). Only SETS a flag here rather than calling
// AndonWifi::runSetupFlow() directly: this handler runs SYNCHRONOUSLY
// INSIDE the lv_timer_handler() call in loop() (LVGL dispatches the click
// as part of indev processing), and runSetupFlow() has its own internal
// lv_timer_handler() polling loop - calling it from here would be a
// reentrant call into LVGL, which corrupted indev press/release tracking
// every time gemini-chatbot's history tried that exact shortcut (see that
// file's comments). loop() consumes the flag at the top level instead -
// see below.
static void onOpenConfig(lv_event_t *e) {
  AndonWifi::requestSetup();
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

// eventType PRODUCTION_COUNT_UPDATED - see AndonMqtt::submitProductionUpdate()'s
// comment for why this is an ad-hoc extension, not something architectur.md
// SS8 specifies. Commits the local count either way (this screen's own
// scratch-value/CANCEL semantics aren't backend-dependent) - only the
// backend round trip's success/failure is logged, not surfaced to the
// operator yet (known gap, matches submitRequest()'s "no visual feedback
// during the blocking wait" note).
static void onProductionConfirm(lv_event_t *e) {
  g_andon.productionCount = g_productionEditValue;
  bool acked = AndonMqtt::submitProductionUpdate(g_andon.productionCount, "WO-240811-07");
  Serial.printf("[production] count=%d backend_acked=%d\r\n", g_andon.productionCount, acked);
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

  // Real MQTT round trip now (see andon_mqtt.cpp) - replaces the old
  // mockConnected-only decision. Blocks up to ~5s (AndonMqtt's bounded
  // wait for the backend's COMMAND_RESULT) with no visual feedback yet
  // during that wait (confirm dialog is already closed by this point) -
  // known gap, not implemented in this pass.
  const CategoryInfo &cat = CATEGORIES[g_andon.categoryIdx];
  const char *reasonCode = cat.reasonCodes[g_andon.reasonIdx];
  String incidentId;
  bool accepted = AndonMqtt::submitAndonRequest(cat.label, reasonCode, "WO-240811-07", incidentId);

  if (accepted) {
    g_andon.incidentId = incidentId;
    g_andon.mockConnected = true; // real accept implies real connectivity - keep the header/banner honest
    showScreenActive();
  } else {
    g_andon.incidentId = "";
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

static void onOfflineRetry(lv_event_t *e) {
  g_andon.mockConnected = true;
  updateHeaderConnDot();
  showScreenActive();
}

static void onOfflineCancel(lv_event_t *e) { cancelRequest(); }

// Start Handling / Resolve are deliberately device-only (product decision,
// 2026-08-13 - see AndonMqtt::submitStatusUpdate()'s comment): the
// dashboard's Acknowledge is the only transition that can happen remotely,
// so this is now the ONE real place those two ever get triggered from,
// same "never claim accepted without proof" rule as everywhere else in
// this file - only advances the screen once the backend actually confirms
// it (blocks briefly, same as submitRequest()/onReasonSend()).
static void onUpdateStatus(lv_event_t *e) {
  if (g_andon.incidentId.length() == 0) {
    Serial.println("AndonMqtt: UPDATE STATUS tapped with no known incidentId - ignoring");
    return;
  }

  if (!g_andon.handling) {
    if (AndonMqtt::submitStatusUpdate(g_andon.incidentId.c_str(), "HANDLING")) {
      g_andon.handling = true;
      showScreenAcknowledged();
    } else {
      Serial.println("AndonMqtt: start-handling update rejected/timed out - staying put");
    }
  } else {
    if (AndonMqtt::submitStatusUpdate(g_andon.incidentId.c_str(), "RESOLVED")) {
      g_andon.downtimeSec = (millis() - g_andon.requestOpenedMs) / 1000;
      showScreenResolved();
    } else {
      Serial.println("AndonMqtt: resolve update rejected/timed out - staying put");
    }
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

  // Three equal icon-only squares: UPDATE PRODUCTION | NEED ASSISTANCE |
  // config (gear). Deviates from design.md SCR-01 ("The primary action
  // occupies the full lower width") - explicit user instruction, recorded
  // here and in design.md's changelog per agents.md's precedence rules
  // (explicit current instruction outranks an existing design spec, but
  // the doc must still be updated to match, not left stale).
  int gap2 = GAP * 2;
  int unit = (CONTENT_W - 2 * MARGIN - gap2) / 3;
  int iconY = 210, iconH = 62;

  makeButton(g_content, MARGIN, iconY, unit, iconH, COLOR_MATERIAL,
             LV_SYMBOL_EDIT, &lv_font_montserrat_28, onOpenUpdateProduction, nullptr);
  // Amber, not red - red is reserved for "there's an active fault" states
  // elsewhere (MAINTENANCE CALLED banner, STATUS: OPEN, etc.); using it here
  // too would make this entry point look like a fault already exists before
  // the operator has even tapped it.
  makeButton(g_content, MARGIN + unit + GAP, iconY, unit, iconH, COLOR_WAITING,
             LV_SYMBOL_BELL, &lv_font_montserrat_28, onNeedAssistance, nullptr);
  // Neutral/muted, not a domain color - this is a secondary/system action
  // (WiFi config - see onOpenConfig()), not a primary operator decision;
  // agents.md UI rules: "keep one dominant decision per screen" (NEED
  // ASSISTANCE), this shouldn't visually compete with it.
  makeButton(g_content, MARGIN + 2 * (unit + GAP), iconY, unit, iconH, COLOR_DISABLED,
             LV_SYMBOL_SETTINGS, &lv_font_montserrat_28, onOpenConfig, nullptr);
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

  // Taller now that the old "(DEMO) SIMULATE TECHNICIAN ACK" button below
  // this row is gone - acknowledgement is a real incoming MQTT push now
  // (see applyIncomingStateUpdate() in loop()), not something this screen
  // needs to fake for itself.
  makeButton(g_content, MARGIN, 194, halfW, 76, COLOR_DISABLED,
             LV_SYMBOL_CLOSE "  CANCEL REQUEST", &lv_font_montserrat_16, onActiveCancel, nullptr);
  makeButton(g_content, MARGIN + halfW + GAP, 194, halfW, 76, COLOR_DISABLED,
             LV_SYMBOL_EDIT "  ADD NOTE", &lv_font_montserrat_16, onActiveAddNote, nullptr);
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

void setup() {
  Serial.begin(115200);
  Serial.println("Starting ESP32-3248S035 Andon System...");

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

  buildHeader();
  buildContent();
  showScreenNormal();
  lv_refr_now(NULL);

  // Runs after the UI is already showing, so the terminal is usable
  // immediately regardless of network state - see andon_config.hpp for the
  // full fallback chain (cache, then live fetch, placeholders otherwise).
  // Blocking (bounded ~10s WiFi timeout worst case): acceptable once at
  // boot, but this must never be called again from inside the UI/timer
  // loop without moving it off the LVGL task first.
  AndonConfig::sync();

  g_tickTimer = lv_timer_create(tickTimerCb, 1000, nullptr);

  Serial.println("Setup done.");
}

// Reacts to a real incident-state push from the backend - the dashboard's
// Acknowledge/Start Handling/Resolve actions (see AndonMqtt::hasStateUpdate()'s
// comment). This is what the old "(DEMO) SIMULATE TECHNICIAN ACK" button
// stood in for; it's gone now that this is real.
static void applyIncomingStateUpdate(const String &incidentId, const String &status) {
  bool hasOpenIncident = (g_andon.screen == SCR_ACTIVE || g_andon.screen == SCR_QUEUED_OFFLINE ||
                          g_andon.screen == SCR_ACKNOWLEDGED);
  if (!hasOpenIncident) {
    Serial.printf("AndonMqtt: state update for %s (status=%s) ignored - no open incident on screen\r\n",
                  incidentId.c_str(), status.c_str());
    return;
  }
  // g_andon.incidentId is only set once submitRequest() gets ACCEPTED (see
  // its comment) - empty while still QueuedOffline, in which case there's
  // nothing to match against yet and any push is stale/unexpected.
  if (g_andon.incidentId.length() == 0 || incidentId != g_andon.incidentId) {
    Serial.printf("AndonMqtt: state update for %s ignored - doesn't match open incident '%s'\r\n",
                  incidentId.c_str(), g_andon.incidentId.c_str());
    return;
  }

  if (status == "ACKNOWLEDGED") {
    g_andon.handling = false;
    showScreenAcknowledged();
  } else if (status == "HANDLING") {
    g_andon.handling = true;
    showScreenAcknowledged();
  } else if (status == "RESOLVED") {
    g_andon.downtimeSec = (millis() - g_andon.requestOpenedMs) / 1000;
    showScreenResolved();
  } else {
    Serial.printf("AndonMqtt: unrecognized state '%s' for %s - ignored\r\n", status.c_str(), incidentId.c_str());
  }
}

void loop() {
  lv_timer_handler();

  // Keeps the MQTT connection alive and processes incoming messages -
  // top-level only, see AndonMqtt::poll()'s comment.
  AndonMqtt::poll();
  if (AndonMqtt::hasStateUpdate()) {
    String incidentId, status;
    AndonMqtt::consumeStateUpdate(incidentId, status);
    applyIncomingStateUpdate(incidentId, status);
  }

  // See onOpenConfig()'s comment: AndonWifi::runSetupFlow() must only ever
  // be called from here (top-level, outside any LVGL event callback) - it
  // runs its own internal lv_timer_handler() loop, which would be a
  // reentrant call if invoked from inside the one above.
  if (AndonWifi::isSetupRequested()) {
    AndonWifi::clearSetupRequest();
    // runSetupFlow() is about to wipe the entire screen, header included -
    // stop the 1Hz tick first so it can't fire in between and write into
    // header widgets that no longer exist (see g_tickTimer's comment).
    if (g_tickTimer) {
      lv_timer_del(g_tickTimer);
      g_tickTimer = nullptr;
    }
    bool connected = AndonWifi::runSetupFlow();
    // runSetupFlow() wiped the entire screen (header included) for the
    // scan/select/password flow - rebuild everything and go back to SCR-01.
    buildHeader();
    buildContent();
    showScreenNormal();
    lv_refr_now(NULL);
    g_tickTimer = lv_timer_create(tickTimerCb, 1000, nullptr);
    // WiFi state may have just changed (new network saved, or reconnected
    // to a previously-working one) - worth an immediate reason-list
    // refresh instead of waiting for the next reboot. Non-fatal either way
    // (see AndonConfig::sync()'s fallback chain) whether this connects or not.
    Serial.printf("AndonWifi: setup flow finished (connected=%d) - re-syncing config\r\n", connected);
    AndonConfig::sync();
  }

  delay(10);
}
