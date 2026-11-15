#include <esp_log.h>

#include "main.h"

static void on_bot_started_speaking() {}

static void on_bot_stopped_speaking() {
  pipecat_screen_bot_done();
}

static void on_bot_tts_text(const char *text) {
  pipecat_screen_bot_text(text);
}

static void on_user_transcription(const char *text, bool final) {
  if (final) {
    ESP_LOGI(LOG_TAG, "You said: %s", text);
  }
  pipecat_screen_user_text(text, final);
}

rtvi_callbacks_t pipecat_rtvi_callbacks = {
    .on_bot_started_speaking = on_bot_started_speaking,
    .on_bot_stopped_speaking = on_bot_stopped_speaking,
    .on_bot_tts_text = on_bot_tts_text,
    .on_user_transcription = on_user_transcription,
};
