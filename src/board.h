#pragma once

#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace board {

// The BMI270, touch controller, RTC, power monitor and camera sensor share one I2C bus.
// M5Unified drives it at register level, outside the ESP-IDF driver's own bus lock, so
// every user must hold this lock. ESP-IDF i2c_master transfers (the camera) take it
// automatically through linker wraps in board.cpp. Recursive, so nesting is safe.
extern SemaphoreHandle_t i2cMutex;

void begin();

inline int64_t nowUs() { return esp_timer_get_time(); }
inline double nowS() { return esp_timer_get_time() * 1e-6; }

class I2CLock {
public:
    I2CLock() { xSemaphoreTakeRecursive(i2cMutex, portMAX_DELAY); }
    ~I2CLock() { xSemaphoreGiveRecursive(i2cMutex); }
    I2CLock(const I2CLock&) = delete;
    I2CLock& operator=(const I2CLock&) = delete;
};

}  // namespace board
