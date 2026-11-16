#include <peer.h>

#define LOG_TAG "pipecat"
#define MAX_HTTP_OUTPUT_BUFFER 4096
#define HTTP_TIMEOUT_MS 10000
#define OFFER_RETRY_FOR_MS 30000
#define TICK_INTERVAL 15

// Wifi
extern void pipecat_init_wifi();

// WebRTC / Media
extern void pipecat_init_audio_capture();
extern void pipecat_init_audio_decoder();
extern void pipecat_init_audio_encoder();
extern void pipecat_send_audio();
extern void pipecat_audio_decode(uint8_t *data, size_t size);
// Call over: forget the bot's last audio, so the mic isn't left muted.
extern void pipecat_audio_call_ended();
// Listening: when off (paused), the board sends no mic audio at all, so the
// bot's speech-to-text gets nothing to bill; the call stays up.
extern bool pipecat_is_listening();
extern void pipecat_set_listening(bool listening);

// WebRTC / Signalling
typedef enum {
  CALL_IDLE,        // hung up with the button; a button press calls again
  CALL_CONNECTING,  // offer sent, waiting for ICE and DTLS
  CALL_CONNECTED,
  CALL_ENDED,       // the bot hung up, or the call failed; a button press calls again
} call_state_t;
extern void pipecat_init_webrtc();
extern void pipecat_webrtc_loop();
extern call_state_t pipecat_call_state();
// Safe from any task; acted on by the main loop.
extern void pipecat_request_call();
extern void pipecat_request_hangup();
extern bool pipecat_webrtc_send_audio(const uint8_t *data, size_t size);
// Returns false if the bot couldn't be reached or gave no usable answer.
extern bool pipecat_http_request(char *offer, char *answer);

// RTVI
typedef struct {
  void (*on_bot_started_speaking)();
  void (*on_bot_stopped_speaking)();
  void (*on_bot_tts_text)(const char *text);
  void (*on_user_transcription)(const char *text, bool final);
} rtvi_callbacks_t;

extern rtvi_callbacks_t pipecat_rtvi_callbacks;

extern void pipecat_init_rtvi(rtvi_callbacks_t *callbacks);
extern void pipecat_rtvi_send_client_ready(PeerConnection *peer_connection);
// Ask the bot to end its pipeline (sent just before the board hangs up).
extern void pipecat_rtvi_send_disconnect_bot(PeerConnection *peer_connection);
extern void pipecat_rtvi_handle_message(const char* msg);

// Screen
extern void pipecat_init_screen();
extern void pipecat_screen_system_log(const char *text);
extern void pipecat_screen_new_log();
extern void pipecat_screen_log(const char *text);
// Captions: safe to call from any task; the screen redraws on its own task.
extern void pipecat_screen_user_text(const char *text, bool final);
extern void pipecat_screen_bot_text(const char *text);
extern void pipecat_screen_bot_done();

// Buttons: PLUS pauses and resumes listening; BOOT hangs up. When there is
// no call, either one calls the bot.
extern void pipecat_init_button();
