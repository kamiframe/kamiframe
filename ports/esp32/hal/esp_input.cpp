/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: input, ESP32 implementation.
 *
 * Reports RAW button state only, same contract sdl_input.cpp implements for
 * desktop: no debounce, no repeat, no edge detection here. core does all of
 * that (see debounce() in app.cpp) so the device and the simulator feel
 * identical -- a backend that "helpfully" debounced here would make the
 * device diverge from what was tuned against the simulator.
 *
 * Every button in kf_esp_pins.h is wired active-low with the internal
 * pull-up enabled: a press ties the pin to GND, so kf_input_poll() reports a
 * button as held when gpio_get_level() reads 0, not 1. No external resistor
 * needed for bring-up; if the real board ends up with external pull-ups or
 * a different active level, that is a kf_esp_pins.h + this file change, not
 * a core change.
 */

#include "kf/hal/input.h"

#include "kf/hal/log.h"
#include "kf/hal/time.h"
#include "kf_esp_pins.h"

#include "driver/gpio.h"

namespace {

constexpr const char *TAG = "input";

struct Binding {
    gpio_num_t pin;
    kf_button button;
};

constexpr Binding kBindings[] = {
    {KF_ESP_PIN_BTN_UP, KF_BTN_UP},     {KF_ESP_PIN_BTN_DOWN, KF_BTN_DOWN},
    {KF_ESP_PIN_BTN_LEFT, KF_BTN_LEFT}, {KF_ESP_PIN_BTN_RIGHT, KF_BTN_RIGHT},
    {KF_ESP_PIN_BTN_A, KF_BTN_A},       {KF_ESP_PIN_BTN_B, KF_BTN_B},
    {KF_ESP_PIN_BTN_MENU, KF_BTN_MENU},
};

} // namespace

kf_result kf_input_init(void) {
    uint64_t pin_mask = 0;
    for (const Binding &b : kBindings) {
        pin_mask |= (1ULL << b.pin);
    }

    gpio_config_t cfg{};
    cfg.pin_bit_mask = pin_mask;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;

    const esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        KF_LOGE(TAG, "gpio_config failed for button pins: %d", err);
        return KF_ERR_IO;
    }

    KF_LOGI(TAG, "%d buttons configured, active-low with internal pull-ups",
            static_cast<int>(sizeof(kBindings) / sizeof(kBindings[0])));
    return KF_OK;
}

kf_result kf_input_poll(kf_input_raw *out) {
    if (out == nullptr) {
        return KF_ERR_INVALID;
    }

    uint32_t mask = 0;
    for (const Binding &b : kBindings) {
        /* Active-low: pressed == 0. */
        if (gpio_get_level(b.pin) == 0) {
            mask |= static_cast<uint32_t>(b.button);
        }
    }

    out->buttons = mask;
    out->sampled_at_us = kf_time_mono_us();
    /* No hardware "quit" concept on the device -- that is a desktop-only
     * escape hatch for closing the SDL window. */
    out->quit_requested = false;
    return KF_OK;
}

void kf_input_shutdown(void) {}
