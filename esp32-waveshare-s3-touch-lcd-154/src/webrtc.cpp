// The call to the bot: one WebRTC peer connection at a time.
//
// Everything that creates or destroys the peer connection runs in the main
// loop task (pipecat_webrtc_loop). Other tasks only ask, through
// pipecat_request_call() / pipecat_request_hangup(), and the audio publisher
// sends through pipecat_webrtc_send_audio(), which holds a mutex so the
// connection can't be destroyed under it.
//
// A call ends when the board hangs up (BOOT button), when the bot hangs up
// (libpeer notices about 10 s later: no more ICE checks), when the offer
// fails, or when connecting takes too long. The board then waits, showing
// "Sleeping", until a button asks for a new call: it never redials on its own,
// so it can't hammer the bot.
#ifndef LINUX_BUILD
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

#include <esp_event.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>

#include <atomic>

#include "main.h"

#define CONNECT_TIMEOUT_MS 20000

enum request_t { REQUEST_NONE, REQUEST_CALL, REQUEST_HANGUP };

static PeerConnection *peer_connection = NULL;
static std::atomic<call_state_t> call_state = CALL_IDLE;
static std::atomic<int> request = REQUEST_NONE;
// Set from libpeer callbacks (inside peer_connection_loop / create_offer),
// acted on afterwards, because a callback must not destroy the connection.
static bool peer_ended = false;
static bool peer_completed = false;
static bool offer_failed = false;
static int64_t connecting_since_us = 0;
static SemaphoreHandle_t peer_mutex;

call_state_t pipecat_call_state() { return call_state; }
void pipecat_request_call() { request = REQUEST_CALL; }
void pipecat_request_hangup() { request = REQUEST_HANGUP; }

bool pipecat_webrtc_send_audio(const uint8_t *data, size_t size) {
  bool sent = false;
  xSemaphoreTake(peer_mutex, portMAX_DELAY);
  if (peer_connection && call_state == CALL_CONNECTED) {
    sent = peer_connection_send_audio(peer_connection, data, size) >= 0;
  }
  xSemaphoreGive(peer_mutex);
  return sent;
}

#ifndef LINUX_BUILD
StaticTask_t task_buffer;
void pipecat_send_audio_task(void *user_data) {
  pipecat_init_audio_encoder();

  while (1) {
    pipecat_send_audio();
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }
}
#endif

static void pipecat_ondatachannel_onmessage_task(char *msg, size_t len,
                                                 void *userdata, uint16_t sid) {
#ifdef LOG_DATACHANNEL_MESSAGES
  ESP_LOGI(LOG_TAG, "DataChannel Message: %s", msg);
#endif
  pipecat_rtvi_handle_message(msg);
}

static void pipecat_ondatachannel_onopen_task(void *userdata) {
  if (peer_connection_create_datachannel(peer_connection, DATA_CHANNEL_RELIABLE,
                                         0, 0, (char *)"rtvi-ai",
                                         (char *)"") != -1) {
    ESP_LOGI(LOG_TAG, "DataChannel created");
  } else {
    ESP_LOGE(LOG_TAG, "Failed to create DataChannel");
  }
}

static void pipecat_onconnectionstatechange_task(PeerConnectionState state,
                                                 void *user_data) {
  ESP_LOGI(LOG_TAG, "PeerConnectionState: %s",
           peer_connection_state_to_string(state));

  if (state == PEER_CONNECTION_DISCONNECTED ||
      state == PEER_CONNECTION_CLOSED || state == PEER_CONNECTION_FAILED) {
    peer_ended = true;
  } else if (state == PEER_CONNECTION_COMPLETED) {
    peer_completed = true;
  }
}

static void pipecat_on_icecandidate_task(char *description, void *user_data) {
  char *local_buffer = (char *)malloc(MAX_HTTP_OUTPUT_BUFFER + 1);
  memset(local_buffer, 0, MAX_HTTP_OUTPUT_BUFFER + 1);
  if (pipecat_http_request(description, local_buffer)) {
    peer_connection_set_remote_description(peer_connection, local_buffer,
                                           SDP_TYPE_ANSWER);
  } else {
    offer_failed = true;
  }
  free(local_buffer);
}

static void start_call() {
  PeerConfiguration peer_connection_config = {
      // STUN gives the board its public address (a server-reflexive candidate), so a
      // cloud bot that can only use a TURN relay can still reach it.
      .ice_servers = {{.urls = "stun:stun.cloudflare.com:3478",
                       .username = NULL,
                       .credential = NULL}},
      .audio_codec = CODEC_OPUS,
      .video_codec = CODEC_NONE,
      .datachannel = DATA_CHANNEL_STRING,
      .onaudiotrack = [](uint8_t *data, size_t size, void *userdata) -> void {
#ifndef LINUX_BUILD
        pipecat_audio_decode(data, size);
#endif
      },
      .onvideotrack = NULL,
      .on_request_keyframe = NULL,
      .user_data = NULL,
  };

  ESP_LOGI(LOG_TAG, "Calling the bot");
  peer_ended = peer_completed = offer_failed = false;
  call_state = CALL_CONNECTING;

  PeerConnection *pc = peer_connection_create(&peer_connection_config);
  if (pc == NULL) {
    ESP_LOGE(LOG_TAG, "Failed to create peer connection");
    call_state = CALL_ENDED;
    return;
  }

  peer_connection_oniceconnectionstatechange(
      pc, pipecat_onconnectionstatechange_task);
  peer_connection_onicecandidate(pc, pipecat_on_icecandidate_task);
  peer_connection_ondatachannel(pc, pipecat_ondatachannel_onmessage_task,
                                pipecat_ondatachannel_onopen_task, NULL);

  xSemaphoreTake(peer_mutex, portMAX_DELAY);
  peer_connection = pc;
  xSemaphoreGive(peer_mutex);

  // Sends the offer to the bot (with retries) and applies its answer.
  peer_connection_create_offer(pc);
  connecting_since_us = esp_timer_get_time();  // ICE and DTLS from here
}

static void end_call(call_state_t next) {
  call_state = next;  // the audio publisher stops sending first
  xSemaphoreTake(peer_mutex, portMAX_DELAY);
  PeerConnection *pc = peer_connection;
  peer_connection = NULL;
  xSemaphoreGive(peer_mutex);
  if (pc) {
    peer_connection_destroy(pc);
  }
  pipecat_audio_call_ended();
  ESP_LOGI(LOG_TAG, "Call ended (%s)",
           next == CALL_IDLE ? "hung up" : "lost or refused");
}

void pipecat_init_webrtc() {
  peer_mutex = xSemaphoreCreateMutex();
  pipecat_init_rtvi(&pipecat_rtvi_callbacks);

#ifndef LINUX_BUILD
  StackType_t *stack_memory = (StackType_t *)heap_caps_malloc(
      30000 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
  xTaskCreateStaticPinnedToCore(pipecat_send_audio_task, "audio_publisher",
                                30000, NULL, 7, stack_memory, &task_buffer, 0);
#endif

  start_call();
}

void pipecat_webrtc_loop() {
  int req = request.exchange(REQUEST_NONE);
  call_state_t state = call_state;

  if (req == REQUEST_HANGUP && state == CALL_CONNECTED) {
    // Tell the bot, so it ends its pipeline now instead of timing out.
    pipecat_rtvi_send_disconnect_bot(peer_connection);
    end_call(CALL_IDLE);
    return;
  }
  if (req == REQUEST_CALL && (state == CALL_IDLE || state == CALL_ENDED)) {
    start_call();
    state = call_state;
  }

  if (peer_connection == NULL) {
    return;
  }
  if (offer_failed) {
    end_call(CALL_ENDED);
    return;
  }

  peer_connection_loop(peer_connection);

  if (peer_ended) {
    end_call(CALL_ENDED);
  } else if (peer_completed && state == CALL_CONNECTING) {
    call_state = CALL_CONNECTED;
    ESP_LOGI(LOG_TAG, "Call connected");
  } else if (state == CALL_CONNECTING &&
             esp_timer_get_time() - connecting_since_us >
                 CONNECT_TIMEOUT_MS * 1000LL) {
    ESP_LOGE(LOG_TAG, "Call didn't connect within %d s", CONNECT_TIMEOUT_MS / 1000);
    end_call(CALL_ENDED);
  }
}
