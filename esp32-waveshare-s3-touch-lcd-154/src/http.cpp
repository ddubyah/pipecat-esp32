#include <cJSON.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>
#ifndef LINUX_BUILD
#include <esp_crt_bundle.h>
#endif

#include "main.h"

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  static int output_len;
  switch (evt->event_id) {
    case HTTP_EVENT_REDIRECT:
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_REDIRECT");
      esp_http_client_set_header(evt->client, "From", "user@example.com");
      esp_http_client_set_header(evt->client, "Accept", "text/html");
      esp_http_client_set_redirection(evt->client);
      break;
    case HTTP_EVENT_ERROR:
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_ERROR");
      break;
    case HTTP_EVENT_ON_CONNECTED:
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_ON_CONNECTED");
      break;
    case HTTP_EVENT_HEADER_SENT:
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_HEADER_SENT");
      break;
    case HTTP_EVENT_ON_HEADER:
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s",
               evt->header_key, evt->header_value);
      break;
    case HTTP_EVENT_ON_DATA: {
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
      // An error page (e.g. a 502 while a sleeping cloud bot wakes) is not the
      // answer: skip it, and the request is retried.
      if (esp_http_client_get_status_code(evt->client) != 200) {
        break;
      }
      if (esp_http_client_is_chunked_response(evt->client)) {
        ESP_LOGE(LOG_TAG, "Chunked HTTP response not supported");
#ifndef LINUX_BUILD
        esp_restart();
#endif
      }

      if (output_len == 0 && evt->user_data) {
        memset(evt->user_data, 0, MAX_HTTP_OUTPUT_BUFFER);
      }

      // If user_data buffer is configured, copy the response into the buffer
      int copy_len = 0;
      if (evt->user_data) {
        // The last byte in evt->user_data is kept for the NULL character in
        // case of out-of-bound access.
        copy_len = MIN(evt->data_len, (MAX_HTTP_OUTPUT_BUFFER - output_len));
        if (copy_len) {
          memcpy(((char *)evt->user_data) + output_len, evt->data, copy_len);
        }
      }
      output_len += copy_len;

      break;
    }
    case HTTP_EVENT_ON_FINISH:
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_ON_FINISH");
      output_len = 0;
      break;
    case HTTP_EVENT_DISCONNECTED:
      ESP_LOGI(LOG_TAG, "HTTP_EVENT_DISCONNECTED");
      output_len = 0;
      break;
  }
  return ESP_OK;
}

void pipecat_http_request(char *offer, char *answer) {
  esp_http_client_config_t config;
  memset(&config, 0, sizeof(esp_http_client_config_t));

  config.url = PIPECAT_SMALLWEBRTC_URL;
  config.event_handler = http_event_handler;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.user_data = answer;
#ifndef LINUX_BUILD
  config.crt_bundle_attach = esp_crt_bundle_attach;  // for an https bot URL
#endif

  ESP_LOGI(LOG_TAG, "Connecting to %s", config.url);

  cJSON *j_offer = cJSON_CreateObject();
  if (j_offer == NULL) {
    ESP_LOGE(LOG_TAG, "Unable to create JSON offer");
    return;
  }
  if (cJSON_AddStringToObject(j_offer, "sdp", offer) == NULL) {
    cJSON_Delete(j_offer);
    ESP_LOGE(LOG_TAG, "Unable to create JSON offer");
    return;
  }
  if (cJSON_AddStringToObject(j_offer, "type", "offer") == NULL) {
    cJSON_Delete(j_offer);
    ESP_LOGE(LOG_TAG, "Unable to create JSON offer");
    return;
  }

  ESP_LOGD(LOG_TAG, "OFFER\n%s", offer);

  char *j_offer_str = cJSON_Print(j_offer);

  cJSON_Delete(j_offer);

  // A cloud bot that is asleep may refuse the first requests (502) or not
  // answer while it wakes, so retry with backoff for about 30 s.
  const int64_t give_up_at_us = esp_timer_get_time() + OFFER_RETRY_FOR_MS * 1000LL;
  int delay_ms = 1000;
  esp_http_client_handle_t client;
  for (int attempt = 1;; attempt++) {
    client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
#ifdef PIPECAT_BOT_TOKEN
    esp_http_client_set_header(client, "Authorization", "Bearer " PIPECAT_BOT_TOKEN);
#endif
    esp_http_client_set_post_field(client, j_offer_str, strlen(j_offer_str));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    if (err == ESP_OK && status_code == 200) {
      break;
    }

    bool retryable = err != ESP_OK || status_code == 502 ||
                     status_code == 503 || status_code == 504;
    bool time_left =
        esp_timer_get_time() + delay_ms * 1000LL < give_up_at_us;
    ESP_LOGE(LOG_TAG, "Offer attempt %d failed: %s (status %d)%s", attempt,
             esp_err_to_name(err), status_code,
             retryable && time_left ? ", retrying" : "");
    esp_http_client_cleanup(client);
    if (!retryable || !time_left) {
#ifndef LINUX_BUILD
      esp_restart();
#endif
      free(j_offer_str);
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    delay_ms = MIN(delay_ms * 2, 8000);
  }
  free(j_offer_str);

  cJSON *j_response = cJSON_Parse((const char *)answer);
  if (j_response == NULL) {
    ESP_LOGE(LOG_TAG, "Error parsing HTTP response");
#ifndef LINUX_BUILD
    esp_restart();
#endif
  }

  cJSON *j_answer = cJSON_GetObjectItem(j_response, "sdp");
  if (j_answer == NULL) {
    ESP_LOGE(LOG_TAG, "Unable to find `sdp` field in response");
#ifndef LINUX_BUILD
    esp_restart();
#endif
  }

  memset(answer, 0, MAX_HTTP_OUTPUT_BUFFER + 1);
  memcpy(answer, j_answer->valuestring, strlen(j_answer->valuestring));

  ESP_LOGD(LOG_TAG, "ANSWER\n%s", answer);

  cJSON_Delete(j_response);

  esp_http_client_cleanup(client);
}
