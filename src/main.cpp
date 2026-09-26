#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266mDNS.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <time.h>

// -----------------------------------------------------------------------------
// GeekMagic Spotify v0.1.8
// Target: GeekMagic SmallTV-Ultra (ESP8266 / ESP-12F / ST7789 240x240)
// UI aprobada: Spotify arriba izquierda + reloj arriba derecha,
// portada grande, título, artista, barra y tiempos. Sin Play/Pausa.
// -----------------------------------------------------------------------------

static const char *FW_NAME = "GeekMagic Spotify";
static const char *FW_VERSION = "0.1.8";
static const char *HOSTNAME = "geekspotify";
static const char *CONFIG_FILE = "/config.json";
static const char *ALBUM_FILE = "/album.jpg";
static const uint8_t PIN_TFT_BL = 5; // active-low PWM
static const uint32_t POLL_NORMAL_MS = 5000;
static const uint32_t UI_TICK_MS = 1000;

// Reposo:
// - 0-30 s sin reproducción: mantiene la última portada.
// - desde 30 s: reloj grande.
// - desde 10 min: apaga solo la retroiluminación.
// Spotify sigue consultándose y despierta automáticamente al volver la música.
static const uint32_t PAUSE_TO_CLOCK_MS = 30000;
static const uint32_t PAUSE_TO_BACKLIGHT_OFF_MS = 600000;

// Scroll suave del título cuando no cabe en los 224 px útiles.
static const uint32_t TITLE_SCROLL_TICK_MS = 105;
static const uint32_t TITLE_SCROLL_PAUSE_MS = 1600;
static const int TITLE_SCROLL_STEP_PX = 1;
static const int TITLE_X = 8;
static const int TITLE_Y = 168;
static const int TITLE_W = 224;
static const int TITLE_H = 26;
static const int TITLE_FONT = 4;

// v0.1.1: dejamos una ventana amplia tras cada arranque para que la web/OTA
// esté disponible antes de iniciar TLS. También separamos token, metadatos y
// portada en ciclos distintos para no encadenar tres conexiones HTTPS.
static const uint32_t SPOTIFY_BOOT_DELAY_MS = 20000;
static const uint32_t SPOTIFY_STAGE_DELAY_MS = 800;

// Europe/Madrid con cambio CET/CEST automático.
static const char *TZ_MADRID = "CET-1CEST,M3.5.0,M10.5.0/3";

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite titleSprite = TFT_eSprite(&tft);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

struct AppConfig {
  String clientId;
  String refreshToken;
  String adminPass = "geekmagic";
  uint8_t brightness = 82;
  uint32_t authorizedAt = 0;
} cfg;

struct PlayerState {
  bool hasItem = false;
  bool isPlaying = false;
  String id;
  String title;
  String artist;
  String artUrl;
  uint32_t durationMs = 0;
  uint32_t progressMs = 0;
  uint32_t progressSyncedAtMs = 0;
} player;

String accessToken;
uint32_t accessTokenExpiryAtMs = 0;
unsigned long nextSpotifyPollAt = 0;
unsigned long spotifyBackoffUntil = 0;
int lastSpotifyHttpCode = 0;
int lastSpotifyContentLength = -1;
uint32_t heapBeforeSpotifyJson = 0;
String lastSpotifyJsonError = "";
unsigned long nextUiTickAt = 0;
bool forceFullRedraw = true;
String statusLine = "Arrancando...";

unsigned long playbackStoppedAtMs = 0;
bool idleClockActive = false;
bool backlightSleeping = false;

bool albumDownloadPending = false;
String pendingAlbumUrl;

// Estado del marquee del título.
bool titleScrollEnabled = false;
int titlePixelWidth = 0;
int titleScrollOffset = 0;
unsigned long nextTitleScrollAt = 0;
unsigned long titleScrollPauseUntil = 0;

// -----------------------------------------------------------------------------
// Utilidades
// -----------------------------------------------------------------------------

String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  return s;
}

String urlEncode(const String &input) {
  static const char *hex = "0123456789ABCDEF";
  String out;
  out.reserve(input.length() * 3);
  for (size_t i = 0; i < input.length(); ++i) {
    uint8_t c = (uint8_t)input[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

String asciiForTft(String s) {
  // Sustituciones frecuentes para que la fuente compacta del TFT no saque basura.
  s.replace("á","a"); s.replace("à","a"); s.replace("ä","a"); s.replace("â","a");
  s.replace("Á","A"); s.replace("À","A"); s.replace("Ä","A"); s.replace("Â","A");
  s.replace("é","e"); s.replace("è","e"); s.replace("ë","e"); s.replace("ê","e");
  s.replace("É","E"); s.replace("È","E"); s.replace("Ë","E"); s.replace("Ê","E");
  s.replace("í","i"); s.replace("ì","i"); s.replace("ï","i"); s.replace("î","i");
  s.replace("Í","I"); s.replace("Ì","I"); s.replace("Ï","I"); s.replace("Î","I");
  s.replace("ó","o"); s.replace("ò","o"); s.replace("ö","o"); s.replace("ô","o");
  s.replace("Ó","O"); s.replace("Ò","O"); s.replace("Ö","O"); s.replace("Ô","O");
  s.replace("ú","u"); s.replace("ù","u"); s.replace("ü","u"); s.replace("û","u");
  s.replace("Ú","U"); s.replace("Ù","U"); s.replace("Ü","U"); s.replace("Û","U");
  s.replace("ñ","n"); s.replace("Ñ","N"); s.replace("ç","c"); s.replace("Ç","C");

  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); ++i) {
    uint8_t c = (uint8_t)s[i];
    if (c >= 32 && c <= 126) out += (char)c;
  }
  return out;
}

String fitText(String s, int maxWidth, int font) {
  s = asciiForTft(s);
  if (tft.textWidth(s, font) <= maxWidth) return s;
  while (s.length() > 3 && tft.textWidth(s + "...", font) > maxWidth) {
    s.remove(s.length() - 1);
  }
  return s + "...";
}

String clockHHMM() {
  time_t now = time(nullptr);
  if (now < 1000000000) return "--:--";
  struct tm tmNow;
  localtime_r(&now, &tmNow);
  char buf[8];
  strftime(buf, sizeof(buf), "%H:%M", &tmNow);
  return String(buf);
}

String mmss(uint32_t ms) {
  uint32_t total = ms / 1000;
  char buf[12];
  snprintf(buf, sizeof(buf), "%lu:%02lu",
           (unsigned long)(total / 60),
           (unsigned long)(total % 60));
  return String(buf);
}

uint32_t currentProgressMs() {
  uint32_t p = player.progressMs;
  if (player.isPlaying) p += (uint32_t)(millis() - player.progressSyncedAtMs);
  if (player.durationMs > 0 && p > player.durationMs) p = player.durationMs;
  return p;
}

void setBrightness(uint8_t pct) {
  if (pct < 5) pct = 5;
  if (pct > 90) pct = 90; // dejamos margen al convertidor del backlight
  cfg.brightness = pct;
  analogWrite(PIN_TFT_BL, 1023 - (pct * 1023 / 100));
}

void setBacklightOff() {
  // Backlight active-low en el SmallTV-Ultra.
  analogWrite(PIN_TFT_BL, 1023);
  backlightSleeping = true;
}

void notePlaybackStopped() {
  if (playbackStoppedAtMs == 0) {
    playbackStoppedAtMs = millis();
  }
  player.isPlaying = false;
}

void wakeForPlayback() {
  bool wasIdle = idleClockActive || backlightSleeping;

  playbackStoppedAtMs = 0;
  idleClockActive = false;

  if (backlightSleeping) {
    setBrightness(cfg.brightness);
    backlightSleeping = false;
  }

  // Si estábamos mostrando reloj o con la luz apagada, hay que reconstruir
  // la pantalla de canción aunque siga siendo la misma pista.
  if (wasIdle) forceFullRedraw = true;
}

void updateIdleState() {
  if (player.isPlaying || playbackStoppedAtMs == 0) return;

  uint32_t elapsed = millis() - playbackStoppedAtMs;

  if (!idleClockActive && elapsed >= PAUSE_TO_CLOCK_MS) {
    idleClockActive = true;
    statusLine = "Spotify en espera";
    forceFullRedraw = true;
  }

  if (!backlightSleeping && elapsed >= PAUSE_TO_BACKLIGHT_OFF_MS) {
    setBacklightOff();
  }
}

// -----------------------------------------------------------------------------
// Configuración persistente
// -----------------------------------------------------------------------------

bool loadConfig() {
  if (!LittleFS.exists(CONFIG_FILE)) return false;
  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) return false;

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  cfg.clientId = String(doc["client_id"] | "");
  cfg.refreshToken = String(doc["refresh_token"] | "");
  cfg.adminPass = String(doc["admin_pass"] | "geekmagic");
  cfg.brightness = doc["brightness"] | 82;
  cfg.authorizedAt = doc["authorized_at"] | 0;
  if (cfg.adminPass.length() < 4) cfg.adminPass = "geekmagic";
  return true;
}

bool saveConfig() {
  DynamicJsonDocument doc(2048);
  doc["client_id"] = cfg.clientId;
  doc["refresh_token"] = cfg.refreshToken;
  doc["admin_pass"] = cfg.adminPass;
  doc["brightness"] = cfg.brightness;
  doc["authorized_at"] = cfg.authorizedAt;

  File f = LittleFS.open(CONFIG_FILE, "w");
  if (!f) return false;
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

// -----------------------------------------------------------------------------
// Pantalla
// -----------------------------------------------------------------------------

void drawSpotifyMark() {
  const uint16_t green = tft.color565(29, 185, 84);
  tft.fillCircle(10, 10, 7, green);
  // Tres pequeñas líneas curvas simplificadas dentro del círculo.
  tft.drawLine(6, 7, 14, 8, TFT_BLACK);
  tft.drawLine(6, 10, 14, 11, TFT_BLACK);
  tft.drawLine(7, 13, 13, 14, TFT_BLACK);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Spotify", 22, 2, 2);
}

void drawHeader(bool clearRight = true) {
  if (clearRight) tft.fillRect(178, 0, 62, 20, TFT_BLACK);
  drawSpotifyMark();
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(clockHHMM(), 234, 2, 2);
}

bool tftJpegOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= tft.height()) return false;
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

void drawAlbumPlaceholder() {
  const uint16_t green = tft.color565(29, 185, 84);
  tft.fillRoundRect(50, 26, 140, 140, 8, tft.color565(28, 31, 33));
  tft.fillCircle(120, 96, 32, green);
  // Nota musical simple dibujada con primitivas: evita depender de glifos Unicode.
  tft.fillRect(126, 72, 5, 34, TFT_BLACK);
  tft.drawLine(130, 73, 145, 69, TFT_BLACK);
  tft.drawLine(130, 74, 145, 70, TFT_BLACK);
  tft.fillCircle(120, 107, 8, TFT_BLACK);
}

bool drawAlbumFromFile() {
  if (!LittleFS.exists(ALBUM_FILE)) {
    drawAlbumPlaceholder();
    return false;
  }

  uint16_t w = 0, h = 0;
  if (TJpgDec.getFsJpgSize(&w, &h, ALBUM_FILE, LittleFS) != JDR_OK || w == 0 || h == 0) {
    drawAlbumPlaceholder();
    return false;
  }

  uint8_t scale = 1;
  if (w >= 500) scale = 4;
  else if (w >= 250) scale = 2;

  uint16_t rw = w / scale;
  uint16_t rh = h / scale;

  // Spotify suele entregar 300x300; con escala 2 queda en 150x150.
  // Mostramos 140x140 visibles, dejando espacio para título y artista.
  int16_t x = (240 - (int16_t)rw) / 2;
  int16_t y = 21;
  if (rw > 170 || rh > 170) {
    drawAlbumPlaceholder();
    return false;
  }

  File jpgFile = LittleFS.open(ALBUM_FILE, "r");
  if (!jpgFile) {
    drawAlbumPlaceholder();
    return false;
  }

  TJpgDec.setJpgScale(scale);

  // En esta placa el ST7789 no usa CS dedicado. Mantener una sola capa de
  // byte-swap y una transacción continua evita los artefactos de bloques.
  tft.startWrite();
  JRESULT r = TJpgDec.drawFsJpg(x, y, jpgFile);
  tft.endWrite();
  jpgFile.close();

  if (r != JDR_OK) {
    drawAlbumPlaceholder();
    return false;
  }

  if (rw >= 145 && rh >= 145) {
    const int crop = 5;
    tft.fillRect(x, y, rw, crop, TFT_BLACK);
    tft.fillRect(x, y + rh - crop, rw, crop, TFT_BLACK);
    tft.fillRect(x, y, crop, rh, TFT_BLACK);
    tft.fillRect(x + rw - crop, y, crop, rh, TFT_BLACK);
  }

  return true;
}

void drawProgressOnly() {
  if (!player.hasItem) return;
  const uint16_t green = tft.color565(29, 185, 84);
  const uint16_t gray = tft.color565(70, 74, 76);
  const int x = 8, y = 223, w = 224, h = 6;

  tft.fillRect(x, y, w, h, gray);
  uint32_t p = currentProgressMs();
  int fill = 0;
  if (player.durationMs > 0) {
    fill = (int)((uint64_t)p * w / player.durationMs);
    if (fill < 0) fill = 0;
    if (fill > w) fill = w;
  }
  if (fill > 0) tft.fillRect(x, y, fill, h, green);
}

void resetTitleScroll() {
  String title = asciiForTft(player.title);
  titlePixelWidth = tft.textWidth(title, TITLE_FONT);
  titleScrollEnabled = titlePixelWidth > TITLE_W;
  titleScrollOffset = 0;
  titleScrollPauseUntil = millis() + TITLE_SCROLL_PAUSE_MS;
  nextTitleScrollAt = titleScrollPauseUntil;
}

void drawTitleOnly(bool fullClear = false) {
  String title = asciiForTft(player.title);

  // v0.1.6: componemos TODA la franja del título fuera de pantalla y luego
  // la enviamos al TFT de una sola vez. Así nunca se ve el paso "borrar -> dibujar".
  // Sprite de 1 bit = consumo mínimo de RAM (~730 bytes).
  titleSprite.fillSprite(TFT_BLACK);
  titleSprite.setTextDatum(TL_DATUM);
  titleSprite.setTextColor(TFT_WHITE, TFT_BLACK);
  titleSprite.drawString(title, -titleScrollOffset, 0, TITLE_FONT);
  titleSprite.pushSprite(TITLE_X, TITLE_Y);
}

void updateTitleScroll() {
  if (idleClockActive || backlightSleeping ||
      !player.hasItem || albumDownloadPending || !titleScrollEnabled) return;

  unsigned long now = millis();
  if ((int32_t)(now - titleScrollPauseUntil) < 0) return;
  if ((int32_t)(now - nextTitleScrollAt) < 0) return;

  nextTitleScrollAt = now + TITLE_SCROLL_TICK_MS;
  titleScrollOffset += TITLE_SCROLL_STEP_PX;

  // Cuando ha pasado todo el texto, vuelve al principio y descansa un poco.
  if (titleScrollOffset > titlePixelWidth + 18) {
    titleScrollOffset = 0;
    titleScrollPauseUntil = now + TITLE_SCROLL_PAUSE_MS;
    nextTitleScrollAt = titleScrollPauseUntil;
  }

  drawTitleOnly(false);
}

void drawNowPlayingFull() {
  tft.fillScreen(TFT_BLACK);
  drawHeader(false);
  drawAlbumFromFile();

  // Zona inferior: título grande con scroll, artista y barra sin cifras.
  tft.fillRect(0, 166, 240, 74, TFT_BLACK);

  resetTitleScroll();
  drawTitleOnly(true);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(tft.color565(196, 200, 202), TFT_BLACK);
  tft.drawString(fitText(player.artist, 224, 2), 8, 196, 2);

  drawProgressOnly();
  forceFullRedraw = false;
}

void drawIdleScreen(const String &message = "Spotify en espera") {
  tft.fillScreen(TFT_BLACK);
  drawHeader(false);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(clockHHMM(), 120, 92, 4);

  tft.setTextColor(tft.color565(170, 176, 180), TFT_BLACK);
  tft.drawString(asciiForTft(message), 120, 132, 2);

  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(tft.color565(95, 104, 108), TFT_BLACK);
    tft.drawString(WiFi.localIP().toString(), 120, 154, 1);
  }
  forceFullRedraw = false;
}

void drawSetupMode() {
  tft.fillScreen(TFT_BLACK);
  const uint16_t green = tft.color565(29, 185, 84);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(green, TFT_BLACK);
  tft.drawString("GeekMagic Spotify", 120, 54, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("CONFIGURAR WIFI", 120, 91, 2);
  tft.drawString("Red:", 120, 122, 1);
  tft.setTextColor(green, TFT_BLACK);
  tft.drawString("GeekMagic-Spotify", 120, 139, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("192.168.4.1", 120, 174, 2);
}

void drawBoot(const String &line) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(tft.color565(29,185,84), TFT_BLACK);
  tft.drawString("GeekMagic Spotify", 120, 84, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(asciiForTft(line), 120, 120, 1);
}

// -----------------------------------------------------------------------------
// HTTPS / Spotify
// Nota v0.1: TLS cifrado pero sin validación de CA en el ESP8266 para ahorrar RAM.
// El token solo se aprovisiona desde la LAN y nunca se expone en la web.
// -----------------------------------------------------------------------------

bool refreshAccessToken() {
  if (cfg.clientId.length() < 10 || cfg.refreshToken.length() < 10) {
    statusLine = "Falta autorizar Spotify";
    return false;
  }

  BearSSL::WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.setTimeout(10000);
  if (!https.begin(client, "https://accounts.spotify.com/api/token")) {
    statusLine = "Error TLS token";
    return false;
  }

  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "grant_type=refresh_token&refresh_token=" + urlEncode(cfg.refreshToken) +
                "&client_id=" + urlEncode(cfg.clientId);

  int code = https.POST(body);
  if (code != HTTP_CODE_OK) {
    statusLine = (code == 400) ? "Reautoriza Spotify" : ("Token HTTP " + String(code));
    https.end();
    return false;
  }

  DynamicJsonDocument doc(1536);
  DeserializationError err = deserializeJson(doc, https.getStream());
  https.end();
  if (err) {
    statusLine = "Token JSON invalido";
    return false;
  }

  accessToken = String(doc["access_token"] | "");
  uint32_t expiresIn = doc["expires_in"] | 3600;
  const char *newRefresh = doc["refresh_token"] | nullptr;
  if (newRefresh && strlen(newRefresh) > 10) {
    cfg.refreshToken = String(newRefresh);
    saveConfig();
  }

  if (accessToken.length() < 20) {
    statusLine = "Token vacio";
    return false;
  }

  uint32_t safeSecs = expiresIn > 90 ? expiresIn - 60 : expiresIn;
  accessTokenExpiryAtMs = millis() + safeSecs * 1000UL;
  statusLine = "Spotify autorizado";
  return true;
}

bool hasValidAccessToken() {
  return accessToken.length() > 0 &&
         (int32_t)(accessTokenExpiryAtMs - millis()) > 0;
}

bool downloadAlbumArt(const String &url) {
  if (url.length() < 8) return false;

  BearSSL::WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  int len = http.getSize();
  if (len > 180000) {
    http.end();
    return false;
  }

  File f = LittleFS.open(ALBUM_FILE, "w");
  if (!f) {
    http.end();
    return false;
  }

  int written = http.writeToStream(&f);
  f.close();
  http.end();

  if (written <= 0) {
    LittleFS.remove(ALBUM_FILE);
    return false;
  }
  return true;
}

String chooseAlbumUrl(JsonArray images) {
  String best;
  int bestDiff = 100000;
  for (JsonVariant v : images) {
    JsonObject img = v.as<JsonObject>();
    int width = img["width"] | 0;
    const char *url = img["url"] | "";
    if (!url || !*url) continue;
    int diff = abs(width - 300);
    if (diff < bestDiff) {
      bestDiff = diff;
      best = String(url);
    }
  }
  return best;
}

void setNoPlayback(const String &why) {
  statusLine = why;
  notePlaybackStopped();

  // Si nunca hemos tenido una canción desde el arranque, mostramos directamente
  // el reloj. Si sí había una canción, conservamos portada/título durante 30 s.
  if (!player.hasItem) {
    if (!idleClockActive) {
      idleClockActive = true;
      forceFullRedraw = true;
    }
  }
}

bool pollSpotifyMetadata() {
  if (!hasValidAccessToken()) return false;

  BearSSL::WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.setTimeout(10000);
  String endpoint = "https://api.spotify.com/v1/me/player/currently-playing?market=ES";
  if (!https.begin(client, endpoint)) {
    statusLine = "Error TLS Spotify";
    return false;
  }

  const char *headerKeys[] = {"Retry-After"};
  https.collectHeaders(headerKeys, 1);

  // v0.1.7: ArduinoJson lee directamente el stream para no guardar la respuesta
  // completa en RAM. Forzamos una respuesta simple (sin chunked/gzip), porque
  // los bloques HTTP pueden confundirse con JSON cuando se usa getStream().
  https.useHTTP10(true);
  https.addHeader("Authorization", "Bearer " + accessToken);
  https.addHeader("Accept", "application/json");
  https.addHeader("Accept-Encoding", "identity");
  https.addHeader("Cache-Control", "no-cache");
  https.addHeader("Pragma", "no-cache");

  int code = https.GET();
  lastSpotifyHttpCode = code;
  lastSpotifyContentLength = https.getSize();

  if (code == HTTP_CODE_NO_CONTENT) {
    https.end();
    setNoPlayback("Spotify en espera");
    return true;
  }

  if (code == HTTP_CODE_UNAUTHORIZED) {
    https.end();
    accessToken = "";
    accessTokenExpiryAtMs = 0;
    statusLine = "Renovando Spotify";
    return false;
  }

  if (code == 429) {
    String retry = https.header("Retry-After");
    uint32_t sec = retry.toInt();
    if (sec < 5) sec = 30;

    spotifyBackoffUntil = millis() + sec * 1000UL;
    nextSpotifyPollAt = spotifyBackoffUntil;
    statusLine = "Spotify limitado " + String(sec) + "s";
    https.end();
    return false;
  }

  // Cualquier respuesta distinta de 429 limpia un backoff anterior.
  spotifyBackoffUntil = 0;

  if (code != HTTP_CODE_OK) {
    statusLine = "Spotify HTTP " + String(code);
    https.end();
    return false;
  }

  StaticJsonDocument<768> filter;
  filter["is_playing"] = true;
  filter["progress_ms"] = true;
  filter["item"]["id"] = true;
  filter["item"]["name"] = true;
  filter["item"]["duration_ms"] = true;
  filter["item"]["artists"][0]["name"] = true;
  for (int i = 0; i < 3; ++i) {
    filter["item"]["album"]["images"][i]["url"] = true;
    filter["item"]["album"]["images"][i]["width"] = true;
    filter["item"]["album"]["images"][i]["height"] = true;
  }

  DynamicJsonDocument doc(2816);
  heapBeforeSpotifyJson = ESP.getFreeHeap();
  lastSpotifyJsonError = "";

  DeserializationError err = deserializeJson(
      doc, https.getStream(), DeserializationOption::Filter(filter));
  https.end();

  if (err) {
    lastSpotifyJsonError = String(err.c_str());
    statusLine = "Spotify JSON: " + lastSpotifyJsonError;
    return false;
  }

  lastSpotifyJsonError = "";

  if (doc["item"].isNull()) {
    setNoPlayback("Spotify en espera");
    return true;
  }

  String newId = String(doc["item"]["id"] | "");
  String newTitle = String(doc["item"]["name"] | "Sin titulo");
  String newArtist = String(doc["item"]["artists"][0]["name"] | "");
  uint32_t newDuration = doc["item"]["duration_ms"] | 0;
  uint32_t newProgress = doc["progress_ms"] | 0;
  bool newPlaying = doc["is_playing"] | false;
  String newArt = chooseAlbumUrl(doc["item"]["album"]["images"].as<JsonArray>());

  if (newPlaying) {
    wakeForPlayback();
  } else {
    notePlaybackStopped();
  }

  bool trackChanged =
      !player.hasItem ||
      newId != player.id ||
      newTitle != player.title ||
      newArtist != player.artist ||
      newDuration != player.durationMs;

  player.hasItem = true;
  player.id = newId;
  player.title = newTitle;
  player.artist = newArtist;
  player.durationMs = newDuration;
  player.progressMs = newProgress;
  player.progressSyncedAtMs = millis();
  player.isPlaying = newPlaying;

  if (trackChanged) {
    titleScrollEnabled = false;
    titleScrollOffset = 0;
    player.artUrl = newArt;
    pendingAlbumUrl = newArt;
    albumDownloadPending = newArt.length() > 0;

    statusLine = albumDownloadPending ? "Portada pendiente" :
                                        (newPlaying ? "Reproduciendo" : "Pausado");

    // Si hay portada nueva, mantenemos la pantalla anterior estable durante
    // los pocos segundos de descarga. El redibujado completo se hace UNA vez
    // cuando la portada ya está lista.
    if (!albumDownloadPending) forceFullRedraw = true;
  } else {
    statusLine = newPlaying ? "Reproduciendo" : "Pausado";
  }
  return true;
}

// -----------------------------------------------------------------------------
// Web UI local + aprovisionamiento
// -----------------------------------------------------------------------------

String rootPage() {
  String ip = WiFi.localIP().toString();
  String configured = (cfg.clientId.length() > 10 && cfg.refreshToken.length() > 10) ? "SI" : "NO";

  String s;
  s.reserve(5000);
  s += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  s += F("<title>GeekMagic Spotify</title><style>");
  s += F("body{font-family:system-ui;background:#111;color:#eee;max-width:760px;margin:auto;padding:24px}");
  s += F(".card{background:#1c1c1c;border-radius:16px;padding:20px;margin:16px 0}h1{color:#1db954}");
  s += F("input{width:100%;box-sizing:border-box;padding:10px;margin:7px 0;background:#292929;color:#fff;border:1px solid #555;border-radius:8px}");
  s += F("button,a.btn{background:#1db954;color:#07140b;border:0;border-radius:999px;padding:10px 18px;font-weight:700;text-decoration:none;display:inline-block}");
  s += F(".muted{color:#aaa}.ok{color:#1db954}</style></head><body>");
  s += F("<h1>GeekMagic Spotify</h1>");
  s += F("<div class='card'><b>Firmware:</b> "); s += FW_VERSION;
  s += F("<br><b>IP:</b> "); s += ip;
  s += F("<br><b>Spotify configurado:</b> <span class='ok'>"); s += configured;
  s += F("</span><br><b>Estado:</b> "); s += htmlEscape(statusLine);
  s += F("<br><b>Spotify HTTP:</b> "); s += String(lastSpotifyHttpCode);
  s += F("<br><b>Modo pantalla:</b> ");
  if (backlightSleeping) s += F("APAGADA");
  else if (idleClockActive) s += F("RELOJ");
  else s += F("NOW PLAYING");
  s += F("<br><b>Spotify bytes:</b> "); s += String(lastSpotifyContentLength);
  if (lastSpotifyJsonError.length()) {
    s += F("<br><b>Error JSON:</b> "); s += htmlEscape(lastSpotifyJsonError);
    s += F("<br><b>Heap antes JSON:</b> "); s += String(heapBeforeSpotifyJson); s += F(" bytes");
  }
  if ((int32_t)(spotifyBackoffUntil - millis()) > 0) {
    s += F("<br><b>Espera API:</b> ");
    s += String((spotifyBackoffUntil - millis() + 999) / 1000);
    s += F(" s");
  }
  s += F("<br><b>Heap libre:</b> "); s += String(ESP.getFreeHeap()); s += F(" bytes");
  s += F("</div>");

  s += F("<div class='card'><h2>Spotify</h2><p class='muted'>El refresh token nunca se muestra. Lo más cómodo es usar tools/provision_spotify.py desde el PC.</p>");
  s += F("<form method='post' action='/settings'>");
  s += F("<label>Client ID</label><input name='client_id' value='"); s += htmlEscape(cfg.clientId); s += F("'>");
  s += F("<label>Refresh token (dejar vacío para conservar el actual)</label><input type='password' name='refresh_token' value=''>");
  s += F("<button type='submit'>Guardar Spotify</button></form></div>");

  s += F("<div class='card'><h2>Pantalla</h2><form method='post' action='/display'>");
  s += F("<label>Brillo 5-90%</label><input type='number' min='5' max='90' name='brightness' value='");
  s += String(cfg.brightness);
  s += F("'><button type='submit'>Guardar brillo</button></form></div>");

  s += F("<div class='card'><h2>Sistema</h2><p><a class='btn' href='/update'>Actualizar firmware OTA</a></p>");
  s += F("<p class='muted'>Usuario OTA: admin. La contraseña inicial es geekmagic.</p>");
  s += F("<form method='post' action='/reboot'><button type='submit'>Reiniciar</button></form></div>");
  s += F("</body></html>");
  return s;
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html; charset=utf-8", rootPage());
  });

  server.on("/api/status", HTTP_GET, []() {
    DynamicJsonDocument doc(1024);
    doc["firmware"] = FW_VERSION;
    doc["ip"] = WiFi.localIP().toString();
    doc["spotify_configured"] = (cfg.clientId.length() > 10 && cfg.refreshToken.length() > 10);
    doc["status"] = statusLine;
    doc["spotify_http"] = lastSpotifyHttpCode;
    doc["idle_clock"] = idleClockActive;
    doc["backlight_sleep"] = backlightSleeping;
    doc["spotify_bytes"] = lastSpotifyContentLength;
    doc["spotify_json_error"] = lastSpotifyJsonError;
    doc["heap_before_json"] = heapBeforeSpotifyJson;
    doc["spotify_backoff_ms"] =
        ((int32_t)(spotifyBackoffUntil - millis()) > 0)
            ? (spotifyBackoffUntil - millis())
            : 0;
    doc["free_heap"] = ESP.getFreeHeap();
    doc["track"] = player.title;
    doc["artist"] = player.artist;
    doc["playing"] = player.isPlaying;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/api/spotify", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
      return;
    }
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
      return;
    }
    String cid = String(doc["client_id"] | "");
    String rt = String(doc["refresh_token"] | "");
    uint32_t authAt = doc["authorized_at"] | 0;

    if (cid.length() < 10 || rt.length() < 10) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"client_id/refresh_token invalid\"}");
      return;
    }

    cfg.clientId = cid;
    cfg.refreshToken = rt;
    cfg.authorizedAt = authAt;
    saveConfig();

    accessToken = "";
    accessTokenExpiryAtMs = 0;
    nextSpotifyPollAt = millis() + 5000;
    statusLine = "Spotify recibido";
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/settings", HTTP_POST, []() {
    if (server.hasArg("client_id") && server.arg("client_id").length() >= 10)
      cfg.clientId = server.arg("client_id");
    if (server.hasArg("refresh_token") && server.arg("refresh_token").length() >= 10)
      cfg.refreshToken = server.arg("refresh_token");
    saveConfig();
    accessToken = "";
    accessTokenExpiryAtMs = 0;
    nextSpotifyPollAt = millis() + 5000;
    server.sendHeader("Location", "/");
    server.send(303, "text/plain", "");
  });

  server.on("/display", HTTP_POST, []() {
    if (server.hasArg("brightness")) {
      int b = server.arg("brightness").toInt();
      if (b < 5) b = 5;
      if (b > 90) b = 90;
      setBrightness((uint8_t)b);
      saveConfig();
    }
    server.sendHeader("Location", "/");
    server.send(303, "text/plain", "");
  });

  server.on("/reboot", HTTP_POST, []() {
    server.send(200, "text/plain", "Reiniciando...");
    delay(250);
    ESP.restart();
  });

  server.on("/v.json", HTTP_GET, []() {
    String v = String("{\"model\":\"SmallTV-Ultra\",\"firmware\":\"") + FW_NAME +
               "\",\"version\":\"" + FW_VERSION + "\"}";
    server.send(200, "application/json", v);
  });

  httpUpdater.setup(&server, "/update", "admin", cfg.adminPass.c_str());
  server.begin();
}

// -----------------------------------------------------------------------------
// Setup / Loop
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(50);

  pinMode(PIN_TFT_BL, OUTPUT);
  analogWriteRange(1023);
  analogWriteFreq(22000);
  setBrightness(82);

  tft.init();
  tft.setRotation(0);
  tft.invertDisplay(true);
  tft.fillScreen(TFT_BLACK);

  // Buffer off-screen del título. 1 bit: blanco/negro y RAM mínima.
  titleSprite.setColorDepth(1);
  titleSprite.createSprite(TITLE_W, TITLE_H);
  titleSprite.setBitmapColor(TFT_WHITE, TFT_BLACK);
  titleSprite.fillSprite(TFT_BLACK);

  if (!LittleFS.begin()) {
    drawBoot("Error LittleFS");
    delay(1500);
  } else {
    loadConfig();
    setBrightness(cfg.brightness);
  }

  drawBoot("Conectando WiFi");

  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);

  WiFiManager wm;
  wm.setConfigPortalTimeout(0); // esperar hasta que el usuario configure
  wm.setAPCallback([](WiFiManager*) {
    drawSetupMode();
  });

  bool wifiOk = wm.autoConnect("GeekMagic-Spotify");
  if (!wifiOk) {
    drawBoot("Error WiFi");
    delay(1500);
    ESP.restart();
  }

  drawBoot("WiFi OK " + WiFi.localIP().toString());

  configTime(TZ_MADRID, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
  }

  setupWebServer();

  // El JPEG se dibuja por bloques directamente en el TFT.
  TJpgDec.setCallback(tftJpegOutput);
  TJpgDec.setSwapBytes(true);

  if (cfg.clientId.length() > 10 && cfg.refreshToken.length() > 10) {
    statusLine = "Spotify inicia en 20s";
  } else {
    statusLine = "Configura Spotify";
  }

  drawIdleScreen(statusLine);
  nextSpotifyPollAt = millis() + SPOTIFY_BOOT_DELAY_MS;
  nextUiTickAt = millis() + 1000;
}

void loop() {
  server.handleClient();
  MDNS.update();
  yield();

  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED &&
      cfg.clientId.length() > 10 && cfg.refreshToken.length() > 10 &&
      (int32_t)(now - nextSpotifyPollAt) >= 0 &&
      (spotifyBackoffUntil == 0 ||
       (int32_t)(now - spotifyBackoffUntil) >= 0)) {

    // IMPORTANTE v0.1.1:
    // Cada ciclo realiza UNA sola operación HTTPS. En v0.1.0 se podían encadenar
    // refresh token -> currently-playing -> portada en la misma vuelta, demasiado
    // agresivo para el heap del ESP8266.
    if (!hasValidAccessToken()) {
      statusLine = "Autorizando Spotify";
      forceFullRedraw = true;
      if (!player.hasItem) drawIdleScreen(statusLine);
      delay(20);
      yield();

      bool ok = refreshAccessToken();
      // Renovar token no cambia el contenido visible.
      nextSpotifyPollAt = millis() + (ok ? SPOTIFY_STAGE_DELAY_MS : 15000);

    } else if (albumDownloadPending) {
      statusLine = "Descargando portada";
      delay(20);
      yield();

      // Guardamos la nueva portada y solo entonces sustituimos la anterior.
      // Si falla, eliminamos el archivo para mostrar el placeholder limpio.
      bool ok = downloadAlbumArt(pendingAlbumUrl);
      albumDownloadPending = false;
      pendingAlbumUrl = "";

      if (!ok && LittleFS.exists(ALBUM_FILE)) LittleFS.remove(ALBUM_FILE);

      statusLine = player.isPlaying ? "Reproduciendo" : "Pausado";
      if (!ok) statusLine = "Sin portada";
      forceFullRedraw = true; // exactamente una vez por cambio de canción
      nextSpotifyPollAt = millis() + SPOTIFY_STAGE_DELAY_MS;

    } else {
      statusLine = player.hasItem ? statusLine : "Consultando Spotify";
      bool ok = pollSpotifyMetadata();

      // pollSpotifyMetadata() decide si ha cambiado algo que requiera
      // redibujado completo. El 429 deja programado su propio Retry-After.
      if (spotifyBackoffUntil != 0 &&
          (int32_t)(spotifyBackoffUntil - millis()) > 0) {
        nextSpotifyPollAt = spotifyBackoffUntil;

      } else if (!ok && !hasValidAccessToken()) {
        // 401: en la próxima etapa se renovará el token.
        nextSpotifyPollAt = millis() + SPOTIFY_STAGE_DELAY_MS;

      } else if (albumDownloadPending) {
        nextSpotifyPollAt = millis() + SPOTIFY_STAGE_DELAY_MS;

      } else {
        // Cerca del final de la canción hacemos una comprobación rápida para
        // que el cambio natural de pista aparezca prácticamente al momento.
        uint32_t p = currentProgressMs();
        uint32_t remaining =
            (player.durationMs > p) ? (player.durationMs - p) : 0;

        if (player.hasItem && player.isPlaying &&
            player.durationMs > 0 && remaining <= 2500) {
          nextSpotifyPollAt = millis() + 900;
        } else {
          nextSpotifyPollAt = millis() + POLL_NORMAL_MS;
        }
      }
    }
  }

  // Gestiona portada -> reloj -> retroiluminación apagada.
  updateIdleState();

  // El marquee del título se actualiza independientemente del reloj/barra.
  updateTitleScroll();

  if ((int32_t)(now - nextUiTickAt) >= 0) {
    nextUiTickAt = now + UI_TICK_MS;

    if (forceFullRedraw) {
      if (idleClockActive || !player.hasItem) {
        drawIdleScreen("Spotify en espera");
      } else {
        drawNowPlayingFull();
      }

    } else if (idleClockActive || !player.hasItem) {
      // En reposo actualizamos el reloj solo mientras la retroiluminación
      // siga encendida. Con la luz apagada no gastamos ciclos de dibujo.
      if (!backlightSleeping) {
        tft.fillRect(48, 72, 144, 42, TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(clockHHMM(), 120, 92, 4);
        drawHeader(true);
      }

    } else if (player.hasItem) {
      drawHeader(true);
      // Si está llegando una portada de una canción nueva, mantenemos el resto
      // de la pantalla intacto hasta poder hacer un único redibujado completo.
      if (!albumDownloadPending) drawProgressOnly();
    }
  }
}
