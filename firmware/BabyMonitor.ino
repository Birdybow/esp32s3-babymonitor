/**
 * BabyMonitor – Freenove ESP32-S3 WROOM (N16R8)
 * ================================================
 * Video: OV5640 → HTTP MJPEG  på port 80,  GET /stream
 * Lyd:   INMP441 → HTTP PCM   på port 81,  GET /audio
 *
 * go2rtc-konfig i HA (/config/go2rtc.yaml):
 * ------------------------------------------
 *   streams:
 *     babymonitor:
 *       - ffmpeg:http://babymonitor.local/stream#video=mjpeg
 *       - ffmpeg:-f s16le -ar 16000 -ac 1 -i http://babymonitor.local:81/audio#audio=pcm
 *
 * Arduino IDE-innstillinger:
 * ---------------------------
 *   Board:              ESP32S3 Dev Module
 *   Flash:              16MB (128Mb)
 *   Partition Scheme:   Huge APP (3MB No OTA/1MB SPIFFS)
 *   PSRAM:              OPI PSRAM
 *   USB CDC On Boot:    Disabled
 *   Port:               UART-porten (høyre USB-C – "USB-Enhanced-SERIAL CH343", COM5)
 *
 * Første flash: USB-kabel til venstre USB-C (CH343).
 * Hold BOOT-knappen nede ved oppstart hvis nødvendig.
 */

#include "esp_camera.h"
#include "esp_http_server.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <driver/i2s.h>
#include <driver/gpio.h>

// ─── WiFi / nettverksnavn ─────────────────────────────────────────────────────
static const char* WIFI_SSID     = "DITT_WIFI_NAVN";     // ← endre til ditt nettverksnavn
static const char* WIFI_PASS     = "DITT_WIFI_PASSORD";  // ← endre til ditt passord
static const char* MDNS_HOSTNAME = "babymonitor";        // http://babymonitor.local

// ─── Kamera-pins (Freenove ESP32-S3 EYE – OV5640) ────────────────────────────
#define CAM_PWDN   -1
#define CAM_RESET  -1
#define CAM_XCLK   15
#define CAM_SIOD    4
#define CAM_SIOC    5
#define CAM_Y9     16
#define CAM_Y8     17
#define CAM_Y7     18
#define CAM_Y6     12
#define CAM_Y5     10
#define CAM_Y4      8
#define CAM_Y3      9
#define CAM_Y2     11
#define CAM_VSYNC   6
#define CAM_HREF    7
#define CAM_PCLK   13

// ─── Mikrofon-pins (INMP441 – I2S standard) ───────────────────────────────────
// Merk: GPIO14 er defekt (stuck HIGH) på dette brettet – bruk GPIO3 for SCK.
// GPIO3 er strapping-pin, men fungerer fint som I2S SCK etter boot.
#define MIC_SCK   3
#define MIC_WS   21
#define MIC_SD    1

// ─── Høyttaler-pins (MAX98357A – I2S standard) ───────────────────────────────
#define SPK_BCLK  41
#define SPK_LRC   42
#define SPK_DOUT  47

// ─── Lydkonfigurasjon ─────────────────────────────────────────────────────────
#define AUDIO_SAMPLE_RATE  16000   // Hz – godt nok for stemmer og gråt
#define AUDIO_BUF_SAMPLES   256   // Samples per I2S-lesning (256 × 4B = 1 kB)
#define SPEAK_BUF_SAMPLES   512   // Samples per I2S-skriving til høyttaler

// Skiftverdi for konvertering fra INMP441 32-bit til 16-bit.
// INMP441 sender 24-bit verdi left-justified i 32-bit ord.
// >> 11 gir god amplitude for tale. Reduser (f.eks. >> 8) hvis lyden er for svak.
#define MIC_GAIN_SHIFT  11

// ─── HTTP-serverinstanser ─────────────────────────────────────────────────────
// Tre separate servere – esp_http_server er entrådet, strømmende handlers
// blokkerer andre forespørsler på samme server.
static httpd_handle_t video_server = NULL;   // port 80: /, /stream, /capture, /player
static httpd_handle_t audio_server = NULL;   // port 81: /audio (inn fra mikrofon)
static httpd_handle_t speak_server = NULL;   // port 82: /speak  (ut til høyttaler)

// =============================================================================
// KAMERA
// =============================================================================

static bool kamera_init() {
  camera_config_t cfg = {};
  cfg.ledc_channel  = LEDC_CHANNEL_0;
  cfg.ledc_timer    = LEDC_TIMER_0;
  cfg.pin_d0        = CAM_Y2;
  cfg.pin_d1        = CAM_Y3;
  cfg.pin_d2        = CAM_Y4;
  cfg.pin_d3        = CAM_Y5;
  cfg.pin_d4        = CAM_Y6;
  cfg.pin_d5        = CAM_Y7;
  cfg.pin_d6        = CAM_Y8;
  cfg.pin_d7        = CAM_Y9;
  cfg.pin_xclk      = CAM_XCLK;
  cfg.pin_pclk      = CAM_PCLK;
  cfg.pin_vsync     = CAM_VSYNC;
  cfg.pin_href      = CAM_HREF;
  cfg.pin_sscb_sda  = CAM_SIOD;
  cfg.pin_sscb_scl  = CAM_SIOC;
  cfg.pin_pwdn      = CAM_PWDN;
  cfg.pin_reset     = CAM_RESET;
  cfg.xclk_freq_hz  = 20000000;
  cfg.pixel_format  = PIXFORMAT_JPEG;
  cfg.frame_size    = FRAMESIZE_VGA;      // 640×480 – god balanse kvalitet/båndbredde
  cfg.jpeg_quality  = 12;                 // 0 = best, 63 = dårligst
  cfg.fb_count      = 2;                  // dobbel-buffering i PSRAM
  cfg.grab_mode     = CAMERA_GRAB_LATEST; // alltid ferskest bilde
  cfg.fb_location   = CAMERA_FB_IN_PSRAM;

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    Serial.printf("[KAMERA] Init feilet: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);     // kameraet er montert opp-ned
  s->set_hmirror(s, 1);
  s->set_brightness(s, 1);

  Serial.println("[KAMERA] OK – VGA JPEG");
  return true;
}

// =============================================================================
// MIKROFON
// =============================================================================

static bool mikrofon_init() {
  // Frigjør GPIO1 fra evt. ADC/touch-funksjon
  gpio_reset_pin(GPIO_NUM_1);
  gpio_set_direction(GPIO_NUM_1, GPIO_MODE_INPUT);

  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = AUDIO_SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,  // INMP441 sender 24-bit i 32-bit ord
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,  // INMP441 L/R-pin koblet til GND = venstre kanal
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = AUDIO_BUF_SAMPLES,
    .use_apll             = false,
  };

  i2s_pin_config_t pins = {
    .mck_io_num   = I2S_PIN_NO_CHANGE,
    .bck_io_num   = MIC_SCK,
    .ws_io_num    = MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = MIC_SD,
  };

  esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  if (err != ESP_OK) { Serial.printf("[MIC] i2s_driver_install feilet: 0x%x\n", err); return false; }

  err = i2s_set_pin(I2S_NUM_0, &pins);
  if (err != ESP_OK) { Serial.printf("[MIC] i2s_set_pin feilet: 0x%x\n", err); return false; }

  Serial.println("[MIC] OK – INMP441 16kHz I2S");
  return true;
}

// =============================================================================
// HØYTTALER
// =============================================================================

static bool hoyttaler_init() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = AUDIO_SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = SPEAK_BUF_SAMPLES,
    .use_apll             = false,
  };

  i2s_pin_config_t pins = {
    .mck_io_num   = I2S_PIN_NO_CHANGE,
    .bck_io_num   = SPK_BCLK,
    .ws_io_num    = SPK_LRC,
    .data_out_num = SPK_DOUT,
    .data_in_num  = I2S_PIN_NO_CHANGE,
  };

  esp_err_t err = i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL);
  if (err != ESP_OK) { Serial.printf("[SPK] install feilet: 0x%x\n", err); return false; }

  err = i2s_set_pin(I2S_NUM_1, &pins);
  if (err != ESP_OK) { Serial.printf("[SPK] pin feilet: 0x%x\n", err); return false; }

  // Send stilhet for å tømme DMA-buffer og unngå pop ved første avspilling
  int16_t silence[SPEAK_BUF_SAMPLES] = {0};
  size_t written;
  i2s_write(I2S_NUM_1, silence, sizeof(silence), &written, 100);

  Serial.println("[SPK] OK – MAX98357A 16kHz I2S");
  return true;
}

// =============================================================================
// HTTP-HANDLERE
// =============================================================================

// ── /stream – MJPEG multipart ─────────────────────────────────────────────────
#define MJPEG_BOUNDARY     "mjpegboundary"
#define MJPEG_CONTENT_TYPE "multipart/x-mixed-replace;boundary=" MJPEG_BOUNDARY
#define MJPEG_BOUNDARY_HDR "\r\n--" MJPEG_BOUNDARY "\r\n"
#define MJPEG_PART_HDR     "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

static esp_err_t stream_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, MJPEG_CONTENT_TYPE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

  char part_hdr[64];
  Serial.println("[STREAM] Klient tilkoblet");

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[STREAM] Feil: ingen kamera-frame");
      return ESP_FAIL;
    }

    esp_err_t res = httpd_resp_send_chunk(req, MJPEG_BOUNDARY_HDR, strlen(MJPEG_BOUNDARY_HDR));

    if (res == ESP_OK) {
      size_t hlen = snprintf(part_hdr, sizeof(part_hdr), MJPEG_PART_HDR, fb->len);
      res = httpd_resp_send_chunk(req, part_hdr, hlen);
    }

    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    }

    esp_camera_fb_return(fb);

    if (res != ESP_OK) {
      Serial.println("[STREAM] Klient koblet fra");
      break;
    }
  }

  return ESP_OK;
}

// ── /capture – enkeltbilde JPEG (brukes av HA Generic Camera som stillbilde-URL) ──
static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

// ── / – enkel statusside ──────────────────────────────────────────────────────
static esp_err_t index_handler(httpd_req_t *req) {
  char html[512];
  snprintf(html, sizeof(html),
    "<html><body style='font-family:sans-serif;padding:24px;max-width:400px'>"
    "<h2>BabyMonitor</h2>"
    "<p><a href='/stream'>Video (MJPEG)</a></p>"
    "<p><a href='/player'>Lydspiller</a></p>"
    "<p><a href='/capture'>Stillbilde</a></p>"
    "<hr><small>ESP32-S3 | OV5640 | INMP441</small>"
    "</body></html>");
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_sendstr(req, html);
}

// ── /player – innebygd HTML-lydspiller (embed i HA-dashboard) ────────────────
// Bruker JavaScript for å opprette ny tilkobling ved hver play-hendelse,
// slik at lyden alltid er live og ikke fra buffer.
static esp_err_t player_handler(httpd_req_t *req) {
  const char* html =
    "<!DOCTYPE html><html>"
    "<head><meta charset='utf-8'>"
    "<style>"
    "  body{margin:0;background:#1a1a2e;display:flex;flex-direction:column;"
    "       align-items:center;justify-content:center;height:100vh;gap:14px;}"
    "  button{padding:12px 28px;font-size:15px;border:none;border-radius:8px;"
    "         cursor:pointer;font-family:sans-serif;transition:background 0.15s;}"
    "  #btnPlay{background:#4caf50;color:#fff;}"
    "  #btnStop{background:#e53935;color:#fff;}"
    "  #btnTale{background:#2196f3;color:#fff;width:160px;height:60px;"
    "           font-size:14px;border-radius:30px;user-select:none;}"
    "  #status{color:#aaa;font-family:sans-serif;font-size:12px;text-align:center;}"
    "</style></head>"
    "<body>"
    "  <button id='btnPlay' onclick='startLyd()'>&#9654; Lytt live</button>"
    "  <button id='btnStop' onclick='stoppLyd()' disabled>&#9646;&#9646; Stopp</button>"
    "  <button id='btnTale'>&#127908; Hold for tale</button>"
    "  <p id='status'>BabyMonitor – trykk for live lyd</p>"
    "<script>"
    // ── Lytte ──────────────────────────────────────────────────────────────
    "  var a=null;"
    "  function startLyd(){"
    "    stoppLyd();"
    "    a=new Audio('http://babymonitor.local:81/audio?t='+Date.now());"
    "    a.play();"
    "    st('Lytter live...');"
    "    document.getElementById('btnPlay').disabled=true;"
    "    document.getElementById('btnStop').disabled=false;"
    "  }"
    "  function stoppLyd(){"
    "    if(a){a.pause();a.src='';a.load();a=null;}"
    "    st('Stoppet');"
    "    document.getElementById('btnPlay').disabled=false;"
    "    document.getElementById('btnStop').disabled=true;"
    "  }"
    // ── Push-to-talk ────────────────────────────────────────────────────────
    "  var ctx,src,proc,strm,talking=false;"
    "  async function startTale(){"
    "    if(talking)return;"
    "    talking=true;"
    "    document.getElementById('btnTale').style.background='#ff9800';"
    "    try{"
    "      strm=await navigator.mediaDevices.getUserMedia({audio:true,video:false});"
    "      ctx=new AudioContext({sampleRate:16000});"
    "      src=ctx.createMediaStreamSource(strm);"
    "      proc=ctx.createScriptProcessor(2048,1,1);"
    "      src.connect(proc);proc.connect(ctx.destination);"
    "      proc.onaudioprocess=function(e){"
    "        if(!talking)return;"
    "        var f=e.inputBuffer.getChannelData(0);"
    "        var b=new Int16Array(f.length);"
    "        for(var i=0;i<f.length;i++){"
    "          var s=Math.max(-1,Math.min(1,f[i]));"
    "          b[i]=s<0?s*32768:s*32767;"
    "        }"
    "        fetch('http://babymonitor.local:82/speak',{"
    "          method:'POST',"
    "          headers:{'Content-Type':'application/octet-stream'},"
    "          body:b.buffer"
    "        }).catch(function(){});"
    "      };"
    "      st('Snakker...');"
    "    }catch(e){talking=false;st('Mikrofon: '+e.message);"
    "      document.getElementById('btnTale').style.background='#2196f3';}"
    "  }"
    "  function stoppTale(){"
    "    if(!talking)return;"
    "    talking=false;"
    "    if(proc){proc.disconnect();proc=null;}"
    "    if(src){src.disconnect();src=null;}"
    "    if(ctx){ctx.close();ctx=null;}"
    "    if(strm){strm.getTracks().forEach(function(t){t.stop()});strm=null;}"
    "    document.getElementById('btnTale').style.background='#2196f3';"
    "    st('Lytter live...');"
    "  }"
    "  var bt=document.getElementById('btnTale');"
    "  bt.addEventListener('mousedown',startTale);"
    "  bt.addEventListener('touchstart',function(e){e.preventDefault();startTale();});"
    "  bt.addEventListener('mouseup',stoppTale);"
    "  bt.addEventListener('mouseleave',stoppTale);"
    "  bt.addEventListener('touchend',stoppTale);"
    "  function st(t){document.getElementById('status').textContent=t;}"
    "</script>"
    "</body></html>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_sendstr(req, html);
}

// ── /speak OPTIONS – CORS preflight (port 80→82 krever dette) ────────────────
static esp_err_t speak_options_handler(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
  httpd_resp_set_hdr(req, "Access-Control-Max-Age",       "86400");
  httpd_resp_set_status(req, "204 No Content");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

// ── /speak POST – mottar int16 PCM fra nettleseren og spiller via høyttaler ──
static esp_err_t speak_handler(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  int total = req->content_len;
  if (total <= 0 || total > 65536) {
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
  }

  // Stilhet før for å unngå pop
  int16_t silence[64] = {0};
  size_t written;
  i2s_write(I2S_NUM_1, silence, sizeof(silence), &written, portMAX_DELAY);

  int16_t buf[SPEAK_BUF_SAMPLES];
  int received = 0;

  while (received < total) {
    int to_read = min((int)sizeof(buf), total - received);
    int ret = httpd_req_recv(req, (char*)buf, to_read);
    if (ret <= 0) break;
    i2s_write(I2S_NUM_1, buf, ret, &written, portMAX_DELAY);
    received += ret;
  }

  // Stilhet etter for å unngå pop
  memset(silence, 0, sizeof(silence));
  i2s_write(I2S_NUM_1, silence, sizeof(silence), &written, portMAX_DELAY);

  Serial.printf("[SPK] Spilte av %d bytes\n", received);
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

// ── /audio – chunked rå PCM (16kHz, 16-bit signed, mono, little-endian) ───────
// WAV-header for uendelig streaming (dataSize = 0xFFFFFFFF)
// ffmpeg auto-detekterer format – ingen mellomrom i go2rtc kilde-URL nødvendig
static void send_wav_header(httpd_req_t *req) {
  const uint32_t SAMPLE_RATE_HZ = AUDIO_SAMPLE_RATE;
  const uint16_t CHANNELS       = 1;
  const uint16_t BITS           = 16;
  const uint32_t BYTE_RATE      = SAMPLE_RATE_HZ * CHANNELS * BITS / 8;
  const uint16_t BLOCK_ALIGN    = CHANNELS * BITS / 8;

  uint8_t hdr[44] = {
    'R','I','F','F',
    0xFF,0xFF,0xFF,0xFF,        // fileSize – ukjent (uendelig strøm)
    'W','A','V','E',
    'f','m','t',' ',
    16,0,0,0,                   // fmtSize = 16
    1,0,                        // audioFormat = PCM
    (uint8_t)CHANNELS, 0,
    (uint8_t)(SAMPLE_RATE_HZ),  (uint8_t)(SAMPLE_RATE_HZ>>8),
    (uint8_t)(SAMPLE_RATE_HZ>>16),(uint8_t)(SAMPLE_RATE_HZ>>24),
    (uint8_t)(BYTE_RATE),       (uint8_t)(BYTE_RATE>>8),
    (uint8_t)(BYTE_RATE>>16),   (uint8_t)(BYTE_RATE>>24),
    (uint8_t)BLOCK_ALIGN, 0,
    BITS, 0,
    'd','a','t','a',
    0xFF,0xFF,0xFF,0xFF         // dataSize – ukjent (uendelig strøm)
  };
  httpd_resp_send_chunk(req, (const char*)hdr, sizeof(hdr));
}

static esp_err_t audio_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "audio/wav");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

  // Send WAV-header først – ffmpeg og nettlesere gjenkjenner formatet automatisk
  send_wav_header(req);

  int32_t buf32[AUDIO_BUF_SAMPLES];
  int16_t buf16[AUDIO_BUF_SAMPLES];
  size_t  bytes_read;

  Serial.println("[AUDIO] Klient tilkoblet (WAV-stream)");

  while (true) {
    esp_err_t err = i2s_read(I2S_NUM_0, buf32, sizeof(buf32), &bytes_read, portMAX_DELAY);
    if (err != ESP_OK) {
      Serial.printf("[AUDIO] I2S read feilet: 0x%x\n", err);
      break;
    }

    int samples = bytes_read / sizeof(int32_t);
    for (int i = 0; i < samples; i++) {
      // INMP441: 24-bit data left-justified i 32-bit ord.
      // Shift ned for å få 16-bit PCM. Juster MIC_GAIN_SHIFT om lyden er for svak/sterk.
      buf16[i] = (int16_t)(buf32[i] >> MIC_GAIN_SHIFT);
    }

    esp_err_t res = httpd_resp_send_chunk(req, (const char*)buf16, samples * sizeof(int16_t));
    if (res != ESP_OK) {
      Serial.println("[AUDIO] Klient koblet fra");
      break;
    }
  }

  return ESP_OK;
}

// =============================================================================
// HTTP-SERVER OPPSETT
// =============================================================================

static void start_servers() {
  // ── Video-server (port 80) ────────────────────────────────────────────────
  httpd_config_t vcfg   = HTTPD_DEFAULT_CONFIG();
  vcfg.server_port      = 80;
  vcfg.stack_size       = 8192;
  vcfg.max_uri_handlers = 6;

  if (httpd_start(&video_server, &vcfg) == ESP_OK) {
    httpd_uri_t idx = { "/",        HTTP_GET, index_handler,   NULL };
    httpd_uri_t str = { "/stream",  HTTP_GET, stream_handler,  NULL };
    httpd_uri_t cap = { "/capture", HTTP_GET, capture_handler, NULL };
    httpd_register_uri_handler(video_server, &idx);
    httpd_register_uri_handler(video_server, &str);
    httpd_register_uri_handler(video_server, &cap);
    Serial.println("[HTTP] Video-server startet på port 80");
  } else {
    Serial.println("[HTTP] FEIL: video-server feilet å starte");
  }

  // ── Lyd-server (port 81) ──────────────────────────────────────────────────
  httpd_config_t acfg   = HTTPD_DEFAULT_CONFIG();
  acfg.server_port      = 81;
  acfg.stack_size       = 8192;
  acfg.max_uri_handlers = 2;
  acfg.ctrl_port        = 32769;   // unngå konflikt med video-serverens kontrollport

  if (httpd_start(&audio_server, &acfg) == ESP_OK) {
    httpd_uri_t aud = { "/audio", HTTP_GET, audio_handler, NULL };
    httpd_register_uri_handler(audio_server, &aud);
    Serial.println("[HTTP] Lyd-server startet på port 81");
  } else {
    Serial.println("[HTTP] FEIL: lyd-server feilet å starte");
  }

  // ── Tale-server (port 82) ──────────────────────────────────────────────────
  httpd_config_t scfg   = HTTPD_DEFAULT_CONFIG();
  scfg.server_port      = 82;
  scfg.stack_size       = 8192;
  scfg.max_uri_handlers = 6;
  scfg.ctrl_port        = 32770;   // unngå konflikt med lyd-serverens kontrollport

  if (httpd_start(&speak_server, &scfg) == ESP_OK) {
    httpd_uri_t spk = { "/speak",  HTTP_POST,    speak_handler,         NULL };
    httpd_uri_t opt = { "/speak",  HTTP_OPTIONS, speak_options_handler, NULL };
    httpd_uri_t ply = { "/player", HTTP_GET,     player_handler,        NULL };
    httpd_register_uri_handler(speak_server, &spk);
    httpd_register_uri_handler(speak_server, &opt);
    httpd_register_uri_handler(speak_server, &ply);
    Serial.println("[HTTP] Tale-server startet på port 82 (/speak + /player)");
  } else {
    Serial.println("[HTTP] FEIL: tale-server feilet å starte");
  }
}

// =============================================================================
// SETUP / LOOP
// =============================================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n╔══════════════════════════╗");
  Serial.println("║      BabyMonitor         ║");
  Serial.println("╚══════════════════════════╝");

  if (!kamera_init()) {
    Serial.println("KRITISK: Kamera feilet – stopper");
    while (true) delay(1000);
  }

  if (!mikrofon_init()) {
    Serial.println("KRITISK: Mikrofon feilet – stopper");
    while (true) delay(1000);
  }

  if (!hoyttaler_init()) {
    Serial.println("KRITISK: Høyttaler feilet – stopper");
    while (true) delay(1000);
  }

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(false);
  Serial.print("[WIFI] Kobler til");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("[WIFI] OK – IP: ");
  Serial.println(WiFi.localIP());

  if (MDNS.begin(MDNS_HOSTNAME)) {
    Serial.printf("[mDNS] http://%s.local\n", MDNS_HOSTNAME);
  }

  start_servers();

  Serial.println("\n─── Klar ───────────────────────────────");
  Serial.printf("Video:   http://%s/stream\n",      WiFi.localIP().toString().c_str());
  Serial.printf("Spiller: http://%s:82/player\n",    WiFi.localIP().toString().c_str());
  Serial.printf("Lyd inn: http://%s:81/audio\n",    WiFi.localIP().toString().c_str());
  Serial.printf("Tale ut: http://%s:82/speak\n",    WiFi.localIP().toString().c_str());
  Serial.println("────────────────────────────────────────");
}

void loop() {
  // Alt håndteres av HTTP-server-tasks og I2S-interrupts.
  // loop() kan brukes til diagnostikk eller statusindikator senere.
  delay(10000);
}
