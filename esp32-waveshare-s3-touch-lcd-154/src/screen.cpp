// Screen for the Waveshare ESP32-S3-Touch-LCD-1.54 (ST7789 240x240), drawn
// with LVGL through esp_lvgl_port.
//
// Top: a bar showing the call: Waking up... (calling the bot), Listening,
// Paused (PLUS), or Sleeping (no call: hung up, or the bot hung up), with a
// hint below to press a button.
// Below: captions, "You:" (what the child said) and "Pet:" (the bot's words,
// as they are spoken), newest text at the bottom.
//
// Other tasks (RTVI, button) never touch LVGL: they write into caption
// buffers under a small mutex, and an LVGL timer copies them onto the screen.
// So the audio and WebRTC tasks never wait on drawing.
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include "board.h"
#include "main.h"

#define LCD_SPI_HOST SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_DRAW_LINES 20
#define LCD_DRAW_BUFFER_PIXELS (BOARD_LCD_WIDTH * LCD_DRAW_LINES)

#define STATUS_BAR_HEIGHT 44
#define SCREEN_REFRESH_MS 100
#define CAPTION_MAX 320  // characters kept per speaker; older text drops off the front

#define COLOR_LISTENING 0x2E7D32  // green
#define COLOR_PAUSED 0xC62828     // red
#define COLOR_WAKING 0xE65100     // orange
#define COLOR_SLEEPING 0x37474F   // grey-blue
#define COLOR_YOU 0x80D8FF        // light blue
#define COLOR_PET 0xFFE082        // yellow

// ---- Caption state, shared with the RTVI task --------------------------------

enum speaker_t { SPEAKER_NONE, SPEAKER_YOU, SPEAKER_PET };

static SemaphoreHandle_t captions_mutex;
static char you_final[CAPTION_MAX + 1];
static char you_interim[CAPTION_MAX + 1];
static char pet_text[CAPTION_MAX + 1];
static speaker_t last_speaker = SPEAKER_NONE;
static bool pet_turn_over = false;
static bool captions_dirty = false;

// Copy text as plain ASCII: the built-in fonts only have ASCII, so map curly
// quotes, dashes and ellipses, and drop anything else.
static size_t to_ascii(const char *in, char *out, size_t out_size) {
  size_t n = 0;
  const unsigned char *s = (const unsigned char *)in;
  while (*s && n + 3 < out_size) {
    if (*s < 0x80) {
      out[n++] = (*s == '\n' || *s == '\r' || *s == '\t') ? ' ' : *s;
      s++;
    } else if (s[0] == 0xE2 && s[1] == 0x80 && s[2]) {
      unsigned char c = s[2];
      if (c == 0x98 || c == 0x99) out[n++] = '\'';
      else if (c == 0x9C || c == 0x9D) out[n++] = '"';
      else if (c == 0x93 || c == 0x94) out[n++] = '-';
      else if (c == 0xA6) { out[n++] = '.'; out[n++] = '.'; out[n++] = '.'; }
      s += 3;
    } else {
      s++;
      while ((*s & 0xC0) == 0x80) s++;  // skip the rest of the UTF-8 sequence
    }
  }
  out[n] = '\0';
  return n;
}

// Append text to a caption, with a space between, keeping the newest text:
// when full, drop whole words from the front.
static void caption_append(char *caption, const char *text) {
  char clean[CAPTION_MAX + 1];
  size_t add = to_ascii(text, clean, sizeof(clean));
  if (add == 0) return;

  size_t len = strlen(caption);
  size_t need = len + (len ? 1 : 0) + add;
  if (need > CAPTION_MAX) {
    size_t drop = need - CAPTION_MAX;
    if (drop >= len) {
      len = 0;
    } else {
      while (drop < len && caption[drop] != ' ') drop++;
      while (drop < len && caption[drop] == ' ') drop++;
      memmove(caption, caption + drop, len - drop + 1);
      len -= drop;
    }
    caption[len] = '\0';
  }
  if (len) caption[len++] = ' ';
  size_t room = CAPTION_MAX - len;
  if (add > room) add = room;
  memcpy(caption + len, clean, add);
  caption[len + add] = '\0';
}

static void clear_captions() {
  you_final[0] = you_interim[0] = pet_text[0] = '\0';
}

void pipecat_screen_user_text(const char *text, bool final) {
  xSemaphoreTake(captions_mutex, portMAX_DELAY);
  if (last_speaker != SPEAKER_YOU) {
    // The child started a new turn: start the screen afresh.
    clear_captions();
    last_speaker = SPEAKER_YOU;
  }
  if (final) {
    caption_append(you_final, text);
    you_interim[0] = '\0';
  } else {
    to_ascii(text, you_interim, sizeof(you_interim));
  }
  captions_dirty = true;
  xSemaphoreGive(captions_mutex);
}

void pipecat_screen_bot_text(const char *text) {
  xSemaphoreTake(captions_mutex, portMAX_DELAY);
  if (last_speaker != SPEAKER_PET) {
    // The pet answers: keep what the child said above it.
    pet_text[0] = '\0';
    last_speaker = SPEAKER_PET;
  } else if (pet_turn_over) {
    // The pet speaks again without the child talking in between.
    clear_captions();
  }
  pet_turn_over = false;
  caption_append(pet_text, text);
  captions_dirty = true;
  xSemaphoreGive(captions_mutex);
}

void pipecat_screen_bot_done() {
  xSemaphoreTake(captions_mutex, portMAX_DELAY);
  if (last_speaker == SPEAKER_PET && !pet_turn_over) {
    pet_turn_over = true;
    ESP_LOGI(LOG_TAG, "Pet said: %s", pet_text);
  }
  xSemaphoreGive(captions_mutex);
}

// ---- LVGL ---------------------------------------------------------------------

static lv_obj_t *status_bar;
static lv_obj_t *status_dot;
static lv_obj_t *status_label;
static lv_obj_t *captions;
static lv_obj_t *you_label;
static lv_obj_t *pet_label;
static lv_obj_t *hint_label;

enum status_t { STATUS_NONE, STATUS_WAKING, STATUS_LISTENING, STATUS_PAUSED, STATUS_SLEEPING };
static status_t shown_status = STATUS_NONE;

static status_t current_status() {
  switch (pipecat_call_state()) {
    case CALL_CONNECTING:
      return STATUS_WAKING;
    case CALL_CONNECTED:
      return pipecat_is_listening() ? STATUS_LISTENING : STATUS_PAUSED;
    default:
      return STATUS_SLEEPING;
  }
}

static void show_status(status_t status) {
  uint32_t color = COLOR_SLEEPING;
  const char *text = "Sleeping";
  switch (status) {
    case STATUS_WAKING: color = COLOR_WAKING; text = "Waking up..."; break;
    case STATUS_LISTENING: color = COLOR_LISTENING; text = "Listening"; break;
    case STATUS_PAUSED: color = COLOR_PAUSED; text = "Paused"; break;
    default: break;
  }
  lv_obj_set_style_bg_color(status_bar, lv_color_hex(color), 0);
  // A solid dot while listening, a hollow one otherwise.
  lv_obj_set_style_bg_opa(status_dot,
                          status == STATUS_LISTENING ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  lv_label_set_text(status_label, text);
  if (status == STATUS_SLEEPING) {
    lv_obj_remove_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(captions, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(captions, LV_OBJ_FLAG_HIDDEN);
  }
}

// Runs in the LVGL task, which already holds the LVGL lock.
static void refresh_timer_cb(lv_timer_t *timer) {
  status_t status = current_status();
  if (status != shown_status) {
    if (status == STATUS_WAKING) {
      // A new call is a new conversation: start with empty captions.
      xSemaphoreTake(captions_mutex, portMAX_DELAY);
      clear_captions();
      captions_dirty = true;
      xSemaphoreGive(captions_mutex);
    }
    show_status(status);
    shown_status = status;
  }

  static char you_line[2 * CAPTION_MAX + 8];
  static char pet_line[CAPTION_MAX + 8];
  if (xSemaphoreTake(captions_mutex, 0) != pdTRUE) return;  // try next tick
  if (!captions_dirty) {
    xSemaphoreGive(captions_mutex);
    return;
  }
  captions_dirty = false;
  bool has_you = you_final[0] || you_interim[0];
  bool has_pet = pet_text[0];
  snprintf(you_line, sizeof(you_line), "You: %s%s%s", you_final,
           (you_final[0] && you_interim[0]) ? " " : "", you_interim);
  snprintf(pet_line, sizeof(pet_line), "Pet: %s", pet_text);
  xSemaphoreGive(captions_mutex);

  lv_label_set_text(you_label, you_line);
  lv_label_set_text(pet_label, pet_line);
  if (has_you) lv_obj_remove_flag(you_label, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(you_label, LV_OBJ_FLAG_HIDDEN);
  if (has_pet) lv_obj_remove_flag(pet_label, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(pet_label, LV_OBJ_FLAG_HIDDEN);

  // Keep the newest words in view.
  lv_obj_update_layout(captions);
  lv_obj_scroll_by_bounded(captions, 0, -lv_obj_get_scroll_bottom(captions),
                           LV_ANIM_OFF);
}

static lv_obj_t *make_caption_label(lv_obj_t *parent, uint32_t color) {
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_width(label, lv_pct(100));
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
  return label;
}

static void build_ui() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  status_bar = lv_obj_create(scr);
  lv_obj_set_size(status_bar, BOARD_LCD_WIDTH, STATUS_BAR_HEIGHT);
  lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_radius(status_bar, 0, 0);
  lv_obj_set_style_border_width(status_bar, 0, 0);
  lv_obj_set_style_pad_all(status_bar, 0, 0);
  lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);

  status_dot = lv_obj_create(status_bar);
  lv_obj_set_size(status_dot, 20, 20);
  lv_obj_align(status_dot, LV_ALIGN_LEFT_MID, 14, 0);
  lv_obj_set_style_radius(status_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(status_dot, lv_color_white(), 0);
  lv_obj_set_style_border_color(status_dot, lv_color_white(), 0);
  lv_obj_set_style_border_width(status_dot, 3, 0);
  lv_obj_remove_flag(status_dot, LV_OBJ_FLAG_SCROLLABLE);

  status_label = lv_label_create(status_bar);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
  lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 46, 0);

  captions = lv_obj_create(scr);
  lv_obj_set_size(captions, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT - STATUS_BAR_HEIGHT);
  lv_obj_align(captions, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(captions, lv_color_black(), 0);
  lv_obj_set_style_radius(captions, 0, 0);
  lv_obj_set_style_border_width(captions, 0, 0);
  lv_obj_set_style_pad_all(captions, 6, 0);
  lv_obj_set_style_pad_row(captions, 8, 0);
  lv_obj_set_flex_flow(captions, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scrollbar_mode(captions, LV_SCROLLBAR_MODE_OFF);

  you_label = make_caption_label(captions, COLOR_YOU);
  pet_label = make_caption_label(captions, COLOR_PET);

  hint_label = lv_label_create(scr);
  lv_obj_set_width(hint_label, BOARD_LCD_WIDTH - 24);
  lv_label_set_long_mode(hint_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(hint_label, lv_color_white(), 0);
  lv_label_set_text(hint_label, "Press a button to wake me up");
  lv_obj_align(hint_label, LV_ALIGN_CENTER, 0, STATUS_BAR_HEIGHT / 2);
  lv_obj_add_flag(hint_label, LV_OBJ_FLAG_HIDDEN);

  lv_timer_create(refresh_timer_cb, SCREEN_REFRESH_MS, NULL);
}

void pipecat_init_screen() {
  captions_mutex = xSemaphoreCreateMutex();

  spi_bus_config_t buscfg = {};
  buscfg.sclk_io_num = BOARD_PIN_LCD_SCK;
  buscfg.mosi_io_num = BOARD_PIN_LCD_MOSI;
  buscfg.miso_io_num = -1;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = LCD_DRAW_BUFFER_PIXELS * sizeof(uint16_t);
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
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

  // LVGL runs at low priority on core 1, away from the audio publisher and
  // the WebRTC loop on core 0.
  lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  port_cfg.task_priority = 1;
  port_cfg.task_affinity = 1;
  ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

  lvgl_port_display_cfg_t disp_cfg = {};
  disp_cfg.io_handle = io;
  disp_cfg.panel_handle = panel;
  disp_cfg.buffer_size = LCD_DRAW_BUFFER_PIXELS;
  disp_cfg.double_buffer = true;
  disp_cfg.hres = BOARD_LCD_WIDTH;
  disp_cfg.vres = BOARD_LCD_HEIGHT;
  disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
  disp_cfg.flags.buff_dma = 1;
  disp_cfg.flags.swap_bytes = 1;
  lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
  if (disp == NULL) {
    ESP_LOGE(LOG_TAG, "Failed to add LVGL display");
    return;
  }

  lvgl_port_lock(0);
  build_ui();
  lvgl_port_unlock();

  gpio_config_t bl_cfg = {};
  bl_cfg.mode = GPIO_MODE_OUTPUT;
  bl_cfg.pin_bit_mask = 1ULL << BOARD_PIN_LCD_BL;
  ESP_ERROR_CHECK(gpio_config(&bl_cfg));
  ESP_ERROR_CHECK(gpio_set_level(BOARD_PIN_LCD_BL, 1));

  ESP_LOGI(LOG_TAG, "Display initialized (LVGL %d.%d.%d)", LVGL_VERSION_MAJOR,
           LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
}

void pipecat_screen_system_log(const char *text) {
  ESP_LOGI(LOG_TAG, "%s", text);
}

void pipecat_screen_new_log() {}

void pipecat_screen_log(const char *text) {}
