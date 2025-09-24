#pragma once

#include <Arduino.h>
#include <functional>
#include <chrono>
#include <driver/adc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

/// Wrapper for milliseconds
using msecu32_t = std::chrono::duration<uint32_t, std::milli>;

/// Base Controller class
class Controller {
public:
  virtual bool init() = 0;
  virtual void update(msecu32_t const elapsed) = 0;
};

/// Atomic wrapper
template <typename T = void>
struct Atomic {
  struct scope {
    scope() { portENTER_CRITICAL(&mux); }
    ~scope() { portEXIT_CRITICAL(&mux); }
  };
  static portMUX_TYPE mux;
  static scope make() { return {}; }
};
template <typename T>
portMUX_TYPE Atomic<T>::mux = portMUX_INITIALIZER_UNLOCKED;

/// ---------------------------------------------------------------------------
/// LCD Controller
/// ---------------------------------------------------------------------------
template <typename AtomicType, typename SPIType, typename I2CType>
class TPC_LCD : public Controller, public AtomicType {
public:
  using atomic_type = AtomicType;

#if (LV_USE_LOG)
  using log_type = std::function<void(lv_log_level_t, const char *)>;
  static const log_type log;
#endif

  bool init() override {
    lv_init();
    return true;
  }

  void update(msecu32_t const) override {
    lv_timer_handler();  // LVGL v8 API
  }
};

#if (LV_USE_LOG)
template <typename AtomicType, typename SPIType, typename I2CType>
const typename TPC_LCD<AtomicType, SPIType, I2CType>::log_type
    TPC_LCD<AtomicType, SPIType, I2CType>::log = nullptr;
#endif

/// ---------------------------------------------------------------------------
/// RGB PWM Controller
/// ---------------------------------------------------------------------------
template <typename AtomicType = Atomic<>>
class RGB_PWM : public Controller, public AtomicType {
public:
  using atomic_type = AtomicType;
  static constexpr uint8_t _pinR = 25;
  static constexpr uint8_t _pinG = 26;
  static constexpr uint8_t _pinB = 27;

  bool init() override {
    ledcSetup(0, 5000, 8);
    ledcSetup(1, 5000, 8);
    ledcSetup(2, 5000, 8);
    ledcAttachPin(_pinR, 0);
    ledcAttachPin(_pinG, 1);
    ledcAttachPin(_pinB, 2);
    return true;
  }

  void update(msecu32_t const) override {}

  void set(uint8_t r, uint8_t g, uint8_t b) {
    auto scope = atomic_type::make();
    ledcWrite(0, r);
    ledcWrite(1, g);
    ledcWrite(2, b);
  }
};

/// ---------------------------------------------------------------------------
/// Amplifier PWM Controller
/// ---------------------------------------------------------------------------
template <typename AtomicType = Atomic<>>
class AMP_PWM : public Controller, public AtomicType {
public:
  using atomic_type = AtomicType;
  static constexpr uint8_t _pin = 32;

  bool init() override {
    ledcSetup(3, 5000, 8);
    ledcAttachPin(_pin, 3);
    return true;
  }

  void update(msecu32_t const) override {}

  void set(uint8_t v) {
    auto scope = atomic_type::make();
    ledcWrite(3, v);
  }
};

/// ---------------------------------------------------------------------------
/// CDS ADC Sensor (Light Sensor)
/// ---------------------------------------------------------------------------
template <typename AtomicType = Atomic<>>
class CDS_ADC : public Controller, public AtomicType {
public:
  using atomic_type = AtomicType;

protected:
  static adc_attenuation_t const _att = ADC_0db;
  static constexpr int _pin = 34;
  int _val{0};

public:
  bool init() override {
    analogSetAttenuation(_att);
    pinMode(_pin, INPUT);
    update(msecu32_t{0});
    return true;
  }

  void update(msecu32_t const) override {
    read();
  }

  int get() {
    auto scope = atomic_type::make();
    return _val;
  }

  int read() {
    auto scope = atomic_type::make();
    _val = analogRead(_pin);
    return _val;
  }
};

/// ---------------------------------------------------------------------------
/// Utility Scheduler
/// ---------------------------------------------------------------------------
template <const uint32_t N>
void run(std::function<void(void)> func) {
  static constexpr TickType_t freq = N / portTICK_PERIOD_MS;
  TickType_t last = xTaskGetTickCount();
  for (;;) {
#ifndef TASK_SCHED_GREEDY
    if (xTaskDelayUntil(&last, freq) == pdTRUE) {
      func();
    }
#else
    xTaskDelayUntil(&last, freq);
    func();
#endif
  }
}
