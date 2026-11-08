#include "board.h"

#include <driver/i2c_master.h>

namespace board {

SemaphoreHandle_t i2cMutex = nullptr;

void begin() {
    if (!i2cMutex) i2cMutex = xSemaphoreCreateRecursiveMutex();
}

}  // namespace board

// Enabled with -Wl,--wrap=... in platformio.ini.
namespace {
struct OptionalLock {
    OptionalLock() {
        if (board::i2cMutex) xSemaphoreTakeRecursive(board::i2cMutex, portMAX_DELAY);
    }
    ~OptionalLock() {
        if (board::i2cMutex) xSemaphoreGiveRecursive(board::i2cMutex);
    }
};
}  // namespace

extern "C" {

esp_err_t __real_i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t* buf, size_t len, int timeout);
esp_err_t __real_i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t* buf, size_t len, int timeout);
esp_err_t __real_i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t* wbuf, size_t wlen,
                                             uint8_t* rbuf, size_t rlen, int timeout);
esp_err_t __real_i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int timeout);

esp_err_t __wrap_i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t* buf, size_t len, int timeout) {
    OptionalLock lock;
    return __real_i2c_master_transmit(dev, buf, len, timeout);
}

esp_err_t __wrap_i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t* buf, size_t len, int timeout) {
    OptionalLock lock;
    return __real_i2c_master_receive(dev, buf, len, timeout);
}

esp_err_t __wrap_i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t* wbuf, size_t wlen,
                                             uint8_t* rbuf, size_t rlen, int timeout) {
    OptionalLock lock;
    return __real_i2c_master_transmit_receive(dev, wbuf, wlen, rbuf, rlen, timeout);
}

esp_err_t __wrap_i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int timeout) {
    OptionalLock lock;
    return __real_i2c_master_probe(bus, addr, timeout);
}

}  // extern "C"
