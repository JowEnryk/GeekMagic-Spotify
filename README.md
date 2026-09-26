# GeekMagic Spotify

Autonomous Spotify **Now Playing** firmware for the **GeekMagic SmallTV-Ultra**.

The display connects directly to Spotify over Wi‑Fi, so after the initial authorization it does **not** need a PC to stay on.

## Features

- Album artwork
- Track title with smooth horizontal marquee when needed
- Artist name
- Progress bar
- `HH:MM` clock in the header
- Automatic track-change detection
- Local Wi‑Fi setup portal
- Local status/configuration web page
- OTA firmware updates
- Automatic idle mode:
  - first 30 s stopped: keep the last artwork on screen
  - after 30 s: large clock + `Spotify en espera`
  - after 10 min: backlight off
  - playback resumes: display wakes automatically

## Supported hardware

This project currently targets **only**:

- GeekMagic **SmallTV-Ultra**
- ESP8266 / ESP-12F
- ST7789 240×240 TFT
- 4 MB flash

It is **not** intended for SmallTV Pro ESP32, ESP32-C2 or other variants.

The firmware was developed and tested from a SmallTV-Ultra originally running `Ultra-V9.0.39`.

## Important before flashing

Keep a copy of your original GeekMagic firmware if possible.

The stock SmallTV-Ultra firmware has a small OTA slot, so the first installation of this project cannot normally be uploaded directly. The tested route is:

1. Install `smalltv-mod-loader.bin` from the `giovi321/smalltv-mod` project.
2. Connect to the temporary `SmallTV-Loader` Wi‑Fi network.
3. Open `http://192.168.4.1/update`.
4. Upload the compiled GeekMagic Spotify firmware as **Firmware**.

After GeekMagic Spotify is installed, future versions can be updated directly from its own `/update` page.

## Build

### Windows

Run:

```text
BUILD_WINDOWS.bat
```

The resulting binary is written to:

```text
dist\GeekMagicSpotify-v0.1.8.bin
```

### PlatformIO

You can also build directly with PlatformIO:

```bash
pio run -e ultra
```

## First boot / Wi‑Fi

On first boot the device creates the access point:

```text
GeekMagic-Spotify
```

Connect to it and choose your 2.4 GHz Wi‑Fi network. If the captive portal does not open automatically, browse to:

```text
http://192.168.4.1
```

Once connected to your network, the device shows its local IP. You can normally reach the status page at:

```text
http://geekspotify.local/
```

or by using the IP displayed on screen.

## Spotify setup

You need your own Spotify Developer application and its **Client ID**.

Recommended redirect URI:

```text
http://127.0.0.1:8888/callback
```

Required scopes:

```text
user-read-currently-playing
user-read-playback-state
```

Run the authorization helper:

```bash
python tools/geekmagic_spotify_auth.py
```

It creates a local file named:

```text
spotify_auth.json
```

**Do not share or commit that file.** It contains your Spotify refresh token and is ignored by this repository's `.gitignore`.

Then provision the device:

```bash
python tools/provision_spotify.py
```

Enter the device IP/hostname when requested. The script sends the Client ID and refresh token only over your local network and does not print the token.

## Local web interface

Open the device hostname or IP in a browser. The page shows status and diagnostics and lets you:

- adjust brightness
- configure Spotify credentials manually
- reboot the device
- perform OTA updates

OTA path:

```text
http://DEVICE-IP/update
```

Default OTA credentials:

```text
user: admin
password: geekmagic
```

Change the password from the local configuration page if the device is used on a network you do not fully trust.

## Security note

The ESP8266 connects to Spotify using HTTPS, but the current firmware uses `setInsecure()` to reduce TLS memory pressure and therefore does **not** validate the remote certificate chain.

That limitation is documented intentionally. A future version should use CA validation if memory constraints allow it.

## Spotify token lifetime

Spotify's Development Mode policies can change. At the time this version was built, refresh tokens for Development Mode apps may require reauthorization periodically. If authorization expires, rerun the authorization and provisioning helpers.

## Privacy / secrets

This repository does **not** contain:

- Wi‑Fi credentials
- Spotify access tokens
- Spotify refresh tokens
- private `spotify_auth.json` files

Never commit those values to GitHub.

## License

MIT License. See [LICENSE](LICENSE).

## Disclaimer

This is an independent community project and is not affiliated with, endorsed by, or sponsored by GeekMagic or Spotify. Product and service names may be trademarks of their respective owners.
