// Screen stub for the Waveshare ESP32-S3-Touch-LCD-1.54: brings up the ST7789,
// clears it to black and turns the backlight on. Log calls go to the serial
// console only, for now.
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>

#include "board.h"
#include "main.h"

#define LCD_SPI_HOST SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_CLEAR_LINES 20

void pipecat_init_screen() {
  spi_bus_config_t buscfg = {};
  buscfg.sclk_io_num = BOARD_PIN_LCD_SCK;
  buscfg.mosi_io_num = BOARD_PIN_LCD_MOSI;
  buscfg.miso_io_num = -1;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = BOARD_LCD_WIDTH * LCD_CLEAR_LINES * sizeof(uint16_t);
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_handle_t io = NULL;
  esp_lcd_panel_io_spi_config_t io_cfg = {};
  io_cfg.cs_gpio_num = BOARD_PIN_LCD_CS;
  io_cfg.dc_gpio_num = BOARD_PIN_LCD_DC;
  io_cfg.spi_mode = 3;
  io_cfg.pclk_hz = LCD_PIXEL_CLOCK_HZ;
  io_cfg.trans_queue_depth = 10;
  io_cfg.lcd_cmd_bits = 8;
  io_cfg.lcd_param_bits = 8;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                           &io_cfg, &io));

  esp_lcd_panel_handle_t panel = NULL;
  esp_lcd_panel_dev_config_t panel_cfg = {};
  panel_cfg.reset_gpio_num = BOARD_PIN_LCD_RST;
  panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_cfg.bits_per_pixel = 16;
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));

  // Clear to black, a band of lines at a time. The band stays allocated:
  // draw_bitmap is asynchronous DMA, so it must outlive the transfers.
  static uint16_t band[BOARD_LCD_WIDTH * LCD_CLEAR_LINES] = {};
  for (int y = 0; y < BOARD_LCD_HEIGHT; y += LCD_CLEAR_LINES) {
    esp_lcd_panel_draw_bitmap(panel, 0, y, BOARD_LCD_WIDTH, y + LCD_CLEAR_LINES,
                              band);
  }
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

  gpio_config_t bl_cfg = {};
  bl_cfg.mode = GPIO_MODE_OUTPUT;
  bl_cfg.pin_bit_mask = 1ULL << BOARD_PIN_LCD_BL;
  ESP_ERROR_CHECK(gpio_config(&bl_cfg));
  ESP_ERROR_CHECK(gpio_set_level(BOARD_PIN_LCD_BL, 1));

  ESP_LOGI(LOG_TAG, "Display initialized (blank)");
}

void pipecat_screen_system_log(const char *text) {
  ESP_LOGI(LOG_TAG, "%s", text);
}

void pipecat_screen_new_log() {}

void pipecat_screen_log(const char *text) {}
