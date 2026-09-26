# Changelog

## v0.1.8 — 2026-09-26

- Automatic idle behaviour.
- Keeps the last cover visible for 30 seconds after playback stops.
- After 30 seconds, switches to a large clock / `Spotify en espera` screen.
- After 10 minutes, turns the TFT backlight off.
- Automatically wakes and returns to Now Playing when playback resumes.
- Keeps the stable Spotify/JSON logic and smooth title marquee from v0.1.7.

## v0.1.7

- Fixed intermittent JSON parsing despite Spotify HTTP 200 responses.
- Uses HTTP/1.0 plus `Accept-Encoding: identity` for reliable streaming JSON.
- Reduced ArduinoJson memory use.
- Added useful JSON/heap diagnostics to the local web UI.
- Smooth off-screen sprite title marquee.

## v0.1.6

- Added 1-bit `TFT_eSprite` title marquee to reduce flicker.

## v0.1.5

- Correct handling of Spotify HTTP 429 / `Retry-After`.
- 5 s normal polling and faster polling near track end.
- More robust track-change detection.

## v0.1.4

- Faster Spotify polling.
- Smoother title scrolling.

## v0.1.3

- Corrected ST7789 / JPEG rendering for SmallTV-Ultra.
- `TFT_CS=-1`, `invertDisplay(true)`, and corrected byte-swap handling.
- Added automatic title marquee.
- Removed numeric elapsed/remaining times from the main UI.
- Thicker progress bar.

## v0.1.2

- Avoided periodic full-screen redraws.
- Improved UI spacing and partial redraw behaviour.

## v0.1.1

- Fixed reboot loop while connecting to Spotify by staging HTTPS work.
- Reduced peak RAM use.

## v0.1.0

- First autonomous Spotify Now Playing firmware.
