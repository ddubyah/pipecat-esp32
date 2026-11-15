// Waveshare ESP32-S3-Touch-LCD-1.54 board support.
#pragma once

#include <driver/gpio.h>
#include <esp_codec_dev.h>

// Battery hold: drive high at boot so the board stays on when on battery.
#define BOARD_PIN_BAT_HOLD GPIO_NUM_2

// I2C bus shared by the ES8311 (speaker) and ES7210 (mic) codecs.
#define BOARD_PIN_I2C_SDA GPIO_NUM_42
#define BOARD_PIN_I2C_SCL GPIO_NUM_41

// I2S, shared by both codecs: TX to ES8311, RX (TDM) from ES7210.
#define BOARD_PIN_I2S_MCLK GPIO_NUM_8
#define BOARD_PIN_I2S_BCLK GPIO_NUM_9
#define BOARD_PIN_I2S_LRCK GPIO_NUM_10
#define BOARD_PIN_I2S_DOUT GPIO_NUM_12
#define BOARD_PIN_I2S_DIN GPIO_NUM_11
// Speaker amplifier enable, driven high by the ES8311 driver when it is open.
#define BOARD_PIN_PA_EN GPIO_NUM_7

// ST7789 240x240 LCD on SPI.
#define BOARD_PIN_LCD_DC GPIO_NUM_45
#define BOARD_PIN_LCD_CS GPIO_NUM_21
#define BOARD_PIN_LCD_SCK GPIO_NUM_38
#define BOARD_PIN_LCD_MOSI GPIO_NUM_39
#define BOARD_PIN_LCD_RST GPIO_NUM_40
#define BOARD_PIN_LCD_BL GPIO_NUM_46
#define BOARD_LCD_WIDTH 240
#define BOARD_LCD_HEIGHT 240

// Buttons (active low).
#define BOARD_PIN_BTN_BOOT GPIO_NUM_0
#define BOARD_PIN_BTN_PWR GPIO_NUM_5
#define BOARD_PIN_BTN_PLUS GPIO_NUM_4

// Hold battery power on. Call first thing in app_main.
void board_power_hold();

// Create the speaker (ES8311) and mic (ES7210) codec devices. Not opened yet.
void board_audio_init(esp_codec_dev_handle_t *mic, esp_codec_dev_handle_t *spk);
