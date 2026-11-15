// Waveshare ESP32-S3-Touch-LCD-1.54 audio and power setup, adapted from
// Waveshare's ESP-IDF factory example (components/esp_bsp/bsp_codec.c).
#include "board.h"

#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <driver/i2s_tdm.h>
#include <esp_codec_dev_defaults.h>
#include <esp_log.h>

#include <cassert>

#define BOARD_TAG "board"
#define BOARD_SAMPLE_RATE 16000

void board_power_hold() {
  gpio_config_t io_conf = {};
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = 1ULL << BOARD_PIN_BAT_HOLD;
  ESP_ERROR_CHECK(gpio_config(&io_conf));
  ESP_ERROR_CHECK(gpio_set_level(BOARD_PIN_BAT_HOLD, 1));
}

static i2c_master_bus_handle_t board_i2c_init() {
  i2c_master_bus_config_t cfg = {};
  cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  cfg.i2c_port = I2C_NUM_0;
  cfg.scl_io_num = BOARD_PIN_I2C_SCL;
  cfg.sda_io_num = BOARD_PIN_I2C_SDA;
  cfg.glitch_ignore_cnt = 7;
  cfg.flags.enable_internal_pullup = 1;

  i2c_master_bus_handle_t bus = NULL;
  ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus));
  return bus;
}

// One I2S port in duplex: STD mode TX to the ES8311, TDM mode RX from the
// ES7210 (4 mic slots), sharing MCLK/BCLK/LRCK.
static void board_i2s_init(i2s_chan_handle_t *tx, i2s_chan_handle_t *rx) {
  i2s_chan_config_t chan_cfg = {};
  chan_cfg.id = I2S_NUM_0;
  chan_cfg.role = I2S_ROLE_MASTER;
  chan_cfg.dma_desc_num = 6;
  chan_cfg.dma_frame_num = 240;
  chan_cfg.auto_clear_after_cb = true;
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, tx, rx));

  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg.sample_rate_hz = BOARD_SAMPLE_RATE;
  std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
  std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
  std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
  std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
  std_cfg.slot_cfg.bit_shift = true;
  std_cfg.slot_cfg.left_align = true;
  std_cfg.gpio_cfg.mclk = BOARD_PIN_I2S_MCLK;
  std_cfg.gpio_cfg.bclk = BOARD_PIN_I2S_BCLK;
  std_cfg.gpio_cfg.ws = BOARD_PIN_I2S_LRCK;
  std_cfg.gpio_cfg.dout = BOARD_PIN_I2S_DOUT;
  std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;

  i2s_tdm_config_t tdm_cfg = {};
  tdm_cfg.clk_cfg.sample_rate_hz = BOARD_SAMPLE_RATE;
  tdm_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
  tdm_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  tdm_cfg.clk_cfg.bclk_div = 8;
  tdm_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
  tdm_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
  tdm_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
  tdm_cfg.slot_cfg.slot_mask = (i2s_tdm_slot_mask_t)(
      I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3);
  tdm_cfg.slot_cfg.ws_width = I2S_TDM_AUTO_WS_WIDTH;
  tdm_cfg.slot_cfg.bit_shift = true;
  tdm_cfg.slot_cfg.total_slot = I2S_TDM_AUTO_SLOT_NUM;
  tdm_cfg.gpio_cfg.mclk = BOARD_PIN_I2S_MCLK;
  tdm_cfg.gpio_cfg.bclk = BOARD_PIN_I2S_BCLK;
  tdm_cfg.gpio_cfg.ws = BOARD_PIN_I2S_LRCK;
  tdm_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  tdm_cfg.gpio_cfg.din = BOARD_PIN_I2S_DIN;

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(*tx, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(*rx, &tdm_cfg));
}

void board_audio_init(esp_codec_dev_handle_t *mic, esp_codec_dev_handle_t *spk) {
  i2c_master_bus_handle_t i2c_bus = board_i2c_init();

  i2s_chan_handle_t tx = NULL, rx = NULL;
  board_i2s_init(&tx, &rx);

  audio_codec_i2s_cfg_t i2s_cfg = {};
  i2s_cfg.port = I2S_NUM_0;
  i2s_cfg.rx_handle = rx;
  i2s_cfg.tx_handle = tx;
  const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
  assert(data_if);

  // Speaker: ES8311 DAC, amplifier enable on GPIO7.
  audio_codec_i2c_cfg_t i2c_cfg = {};
  i2c_cfg.addr = ES8311_CODEC_DEFAULT_ADDR;
  i2c_cfg.bus_handle = i2c_bus;
  const audio_codec_ctrl_if_t *out_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
  assert(out_ctrl_if);

  const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
  assert(gpio_if);

  es8311_codec_cfg_t es8311_cfg = {};
  es8311_cfg.ctrl_if = out_ctrl_if;
  es8311_cfg.gpio_if = gpio_if;
  es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
  es8311_cfg.pa_pin = BOARD_PIN_PA_EN;
  es8311_cfg.use_mclk = true;
  es8311_cfg.hw_gain.pa_voltage = 5.0;
  es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
  const audio_codec_if_t *out_codec_if = es8311_codec_new(&es8311_cfg);
  assert(out_codec_if);

  esp_codec_dev_cfg_t dev_cfg = {};
  dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
  dev_cfg.codec_if = out_codec_if;
  dev_cfg.data_if = data_if;
  *spk = esp_codec_dev_new(&dev_cfg);
  assert(*spk);

  // Mic: ES7210 ADC.
  i2c_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
  const audio_codec_ctrl_if_t *in_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
  assert(in_ctrl_if);

  es7210_codec_cfg_t es7210_cfg = {};
  es7210_cfg.ctrl_if = in_ctrl_if;
  es7210_cfg.mic_selected = ES7120_SEL_MIC1 | ES7120_SEL_MIC2 |
                            ES7120_SEL_MIC3 | ES7120_SEL_MIC4;
  const audio_codec_if_t *in_codec_if = es7210_codec_new(&es7210_cfg);
  assert(in_codec_if);

  dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
  dev_cfg.codec_if = in_codec_if;
  *mic = esp_codec_dev_new(&dev_cfg);
  assert(*mic);

  ESP_LOGI(BOARD_TAG, "Audio codecs created (ES8311 out, ES7210 in)");
}
