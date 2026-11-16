// Buttons (active low), polled from a small low-priority task with a
// debounce, so a bouncy press counts once and no interrupt work touches audio
// or the screen.
//
// PLUS (GPIO4): during a call, pause or resume listening (the call stays up).
// BOOT (GPIO0): during a call, hang up. BOOT is a strapping pin, but it is
//   only read at reset (held down then, it starts the ROM bootloader), so it
//   is an ordinary input once the app runs. PWR (GPIO5) is kept free for
//   power on/off.
// With no call (hung up, or the bot hung up), either button calls the bot.
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"
#include "main.h"

#define BUTTON_POLL_MS 10
#define BUTTON_DEBOUNCE_MS 30

typedef struct {
  gpio_num_t pin;
  int stable;  // 1 = released
  int last_raw;
  TickType_t changed_at;
} button_t;

// True once per press (on the debounced release-to-pressed edge).
static bool button_pressed(button_t *b, TickType_t now) {
  int raw = gpio_get_level(b->pin);
  if (raw != b->last_raw) {
    b->last_raw = raw;
    b->changed_at = now;
  } else if (raw != b->stable &&
             now - b->changed_at >= pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) {
    b->stable = raw;
    return raw == 0;
  }
  return false;
}

static bool no_call() {
  call_state_t state = pipecat_call_state();
  return state == CALL_IDLE || state == CALL_ENDED;
}

static void on_plus() {
  if (no_call()) {
    pipecat_set_listening(true);
    pipecat_request_call();
    ESP_LOGI(LOG_TAG, "PLUS pressed: calling");
  } else if (pipecat_call_state() == CALL_CONNECTED) {
    bool listening = !pipecat_is_listening();
    pipecat_set_listening(listening);
    ESP_LOGI(LOG_TAG, "PLUS pressed: %s", listening ? "listening" : "paused");
  }
}

static void on_boot() {
  if (no_call()) {
    pipecat_set_listening(true);
    pipecat_request_call();
    ESP_LOGI(LOG_TAG, "BOOT pressed: calling");
  } else if (pipecat_call_state() == CALL_CONNECTED) {
    pipecat_request_hangup();
    ESP_LOGI(LOG_TAG, "BOOT pressed: hanging up");
  }
}

static void button_task(void *arg) {
  TickType_t now = xTaskGetTickCount();
  button_t plus = {BOARD_PIN_BTN_PLUS, 1, 1, now};
  button_t boot = {BOARD_PIN_BTN_BOOT, 1, 1, now};

  while (1) {
    now = xTaskGetTickCount();
    if (button_pressed(&plus, now)) on_plus();
    if (button_pressed(&boot, now)) on_boot();
    vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
  }
}

void pipecat_init_button() {
  gpio_config_t cfg = {};
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pin_bit_mask = (1ULL << BOARD_PIN_BTN_PLUS) | (1ULL << BOARD_PIN_BTN_BOOT);
  cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK(gpio_config(&cfg));

  xTaskCreatePinnedToCore(button_task, "button", 3072, NULL, 1, NULL, 1);
  ESP_LOGI(LOG_TAG, "Buttons ready (PLUS: pause/resume, BOOT: hang up; either calls)");
}
