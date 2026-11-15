// PLUS button (GPIO4, active low): each press toggles listening on and off.
// Polled from a small low-priority task with a debounce, so a bouncy press
// counts once and no interrupt work touches audio or the screen.
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"
#include "main.h"

#define BUTTON_POLL_MS 10
#define BUTTON_DEBOUNCE_MS 30

static void button_task(void *arg) {
  int stable = 1;  // released
  int last_raw = 1;
  TickType_t changed_at = xTaskGetTickCount();

  while (1) {
    int raw = gpio_get_level(BOARD_PIN_BTN_PLUS);
    TickType_t now = xTaskGetTickCount();
    if (raw != last_raw) {
      last_raw = raw;
      changed_at = now;
    } else if (raw != stable &&
               now - changed_at >= pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) {
      stable = raw;
      if (stable == 0) {  // pressed
        bool listening = !pipecat_is_listening();
        pipecat_set_listening(listening);
        ESP_LOGI(LOG_TAG, "PLUS pressed: %s", listening ? "listening" : "paused");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
  }
}

void pipecat_init_button() {
  gpio_config_t cfg = {};
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pin_bit_mask = 1ULL << BOARD_PIN_BTN_PLUS;
  cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK(gpio_config(&cfg));

  xTaskCreatePinnedToCore(button_task, "button", 3072, NULL, 1, NULL, 1);
  ESP_LOGI(LOG_TAG, "PLUS button ready (press to pause or resume listening)");
}
