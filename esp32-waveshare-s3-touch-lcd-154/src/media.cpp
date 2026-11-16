#include <esp_codec_dev.h>
#include <esp_heap_caps.h>
#include <opus.h>

#include <atomic>
#include <cstring>

#include "esp_log.h"
#include "board.h"
#include "main.h"

#define SAMPLE_RATE (16000)

#define OPUS_BUFFER_SIZE 1276  // 1276 bytes is recommended by opus_encode
#define PCM_BUFFER_SIZE 640

#define OPUS_ENCODER_BITRATE 30000
#define OPUS_ENCODER_COMPLEXITY 0

// Set after opening, and checked by board_mic_check(). 30 dB matches the level
// the mic ran at in every working test so far (quiet room peaks ~75); the old
// 42 dB, set before opening, did not stick. At 0 dB the mic is near silent.
#define MIC_GAIN_DB 30.0

// Define AUDIO_DIAG to log the mic's raw peak and the mute flags once a second.
// #define AUDIO_DIAG

std::atomic<bool> is_playing = false;
std::atomic<int> diag_dec_frames = 0;  // AUDIO_DIAG: bot frames since the last report
std::atomic<int> diag_dec_peak = 0;
void set_is_playing(int16_t *in_buf, size_t in_samples) {
  bool any_set = false;
  int peak = 0;
  for (size_t i = 0; i < in_samples; i++) {
    if (in_buf[i] != -1 && in_buf[i] != 0 && in_buf[i] != 1) {
      any_set = true;
    }
    int a = abs(in_buf[i]);
    if (a > peak) peak = a;
  }
  is_playing = any_set;
  diag_dec_frames++;
  if (peak > diag_dec_peak) diag_dec_peak = peak;
}

std::atomic<bool> is_listening = true;
bool pipecat_is_listening() { return is_listening; }
void pipecat_set_listening(bool listening) { is_listening = listening; }

esp_codec_dev_handle_t mic_codec_dev = NULL;
esp_codec_dev_handle_t spk_codec_dev = NULL;

void pipecat_init_audio_capture() {
  board_audio_init(&mic_codec_dev, &spk_codec_dev);

  esp_codec_dev_set_out_vol(spk_codec_dev, 255);

  esp_codec_dev_sample_info_t fs = {
      .bits_per_sample = 16,
      .channel = 1,
      .sample_rate = SAMPLE_RATE,
  };
  esp_codec_dev_open(spk_codec_dev, &fs);

  // The ES7210 sends 4 TDM slots; keep only mic 1 so reads are mono.
  esp_codec_dev_sample_info_t mic_fs = fs;
  mic_fs.channel = 4;
  mic_fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0);
  if (esp_codec_dev_open(mic_codec_dev, &mic_fs) != ESP_CODEC_DEV_OK) {
    ESP_LOGE(LOG_TAG, "Mic codec failed to open");
  }
  esp_codec_dev_set_in_gain(mic_codec_dev, MIC_GAIN_DB);
  board_mic_check(mic_codec_dev, MIC_GAIN_DB);
}

opus_int16 *decoder_buffer = NULL;
OpusDecoder *opus_decoder = NULL;

void pipecat_init_audio_decoder() {
  int decoder_error = 0;
  opus_decoder = opus_decoder_create(SAMPLE_RATE, 1, &decoder_error);
  if (decoder_error != OPUS_OK) {
    printf("Failed to create OPUS decoder");
    return;
  }

  decoder_buffer = (opus_int16 *)malloc(PCM_BUFFER_SIZE);
}

void pipecat_audio_decode(uint8_t *data, size_t size) {
  esp_err_t ret;
  int decoded_size =
      opus_decode(opus_decoder, data, size, decoder_buffer, PCM_BUFFER_SIZE, 0);

  if (decoded_size > 0) {
    set_is_playing(decoder_buffer, decoded_size);
    if ((ret = esp_codec_dev_write(spk_codec_dev, decoder_buffer,
                                   decoded_size * sizeof(uint16_t))) !=
        ESP_OK) {
      ESP_LOGE(LOG_TAG, "esp_codec_dev_write failed: %s", esp_err_to_name(ret));
    }
  }
}

OpusEncoder *opus_encoder = NULL;
uint8_t *encoder_output_buffer = NULL;
uint8_t *read_buffer = NULL;

void pipecat_init_audio_encoder() {
  int encoder_error;
  opus_encoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP,
                                     &encoder_error);
  if (encoder_error != OPUS_OK) {
    printf("Failed to create OPUS encoder");
    return;
  }

  if (opus_encoder_init(opus_encoder, SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP) !=
      OPUS_OK) {
    printf("Failed to initialize OPUS encoder");
    return;
  }

  opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(OPUS_ENCODER_BITRATE));
  opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY(OPUS_ENCODER_COMPLEXITY));
  opus_encoder_ctl(opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

  read_buffer =
      (uint8_t *)heap_caps_malloc(PCM_BUFFER_SIZE, MALLOC_CAP_DEFAULT);
  encoder_output_buffer = (uint8_t *)malloc(OPUS_BUFFER_SIZE);
}

void pipecat_send_audio(PeerConnection *peer_connection) {
  if (esp_codec_dev_read(mic_codec_dev, read_buffer, PCM_BUFFER_SIZE) !=
      ESP_OK) {
    printf("esp_codec_dev_read failed");
    return;
  }

#ifdef AUDIO_DIAG
  {
    static int frames = 0, mic_peak = 0, muted = 0;
    const int16_t *pcm = (const int16_t *)read_buffer;
    for (int i = 0; i < PCM_BUFFER_SIZE / 2; i++) {
      int a = abs(pcm[i]);
      if (a > mic_peak) mic_peak = a;
    }
    if (is_playing || !is_listening) muted++;
    if (++frames >= 50) {
      ESP_LOGI(LOG_TAG, "DIAG mic_peak=%d muted=%d/50 playing=%d listening=%d dec_frames=%d dec_peak=%d",
               mic_peak, muted, (int)is_playing, (int)is_listening,
               diag_dec_frames.exchange(0), diag_dec_peak.exchange(0));
      frames = mic_peak = muted = 0;
    }
  }
#endif

  if (is_playing || !is_listening) {
    memset(read_buffer, 0, PCM_BUFFER_SIZE);
  }

  auto encoded_size = opus_encode(opus_encoder, (const opus_int16 *)read_buffer,
                                  PCM_BUFFER_SIZE / sizeof(uint16_t),
                                  encoder_output_buffer, OPUS_BUFFER_SIZE);
  peer_connection_send_audio(peer_connection, encoder_output_buffer,
                             encoded_size);
}
