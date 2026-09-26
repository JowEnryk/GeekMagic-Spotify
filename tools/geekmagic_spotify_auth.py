import base64
import hashlib
import json
import secrets
import time
import urllib.parse
import urllib.request
import urllib.error
import webbrowser
from http.server import BaseHTTPRequestHandler, HTTPServer

REDIRECT_URI = "http://127.0.0.1:8888/callback"
SCOPES = "user-read-currently-playing user-read-playback-state"

def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")

def post_form(url, data):
    body = urllib.parse.urlencode(data).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=body,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.loads(r.read().decode("utf-8"))

def api_get(url, access_token):
    req = urllib.request.Request(
        url,
        headers={"Authorization": f"Bearer {access_token}"},
        method="GET",
    )
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            if r.status == 204:
                return None
            raw = r.read()
            return json.loads(raw.decode("utf-8")) if raw else None
    except urllib.error.HTTPError as e:
        if e.code == 204:
            return None
        raise

def main():
    print("=== GeekMagic Spotify - autorización inicial ===")
    client_id = input("Pega aquí el Client ID de tu app de Spotify: ").strip()
    if not client_id:
        print("Client ID vacío. Cancelado.")
        return

    verifier = b64url(secrets.token_bytes(64))
    challenge = b64url(hashlib.sha256(verifier.encode("ascii")).digest())
    state = secrets.token_urlsafe(24)

    result = {}

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            parsed = urllib.parse.urlparse(self.path)
            if parsed.path != "/callback":
                self.send_response(404)
                self.end_headers()
                return
            qs = urllib.parse.parse_qs(parsed.query)
            result["code"] = qs.get("code", [None])[0]
            result["state"] = qs.get("state", [None])[0]
            result["error"] = qs.get("error", [None])[0]

            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write(
                "<html><body style='font-family:sans-serif'>"
                "<h2>Spotify autorizado</h2>"
                "<p>Ya puedes cerrar esta ventana y volver a PowerShell.</p>"
                "</body></html>".encode("utf-8")
            )

        def log_message(self, format, *args):
            pass

    params = {
        "client_id": client_id,
        "response_type": "code",
        "redirect_uri": REDIRECT_URI,
        "scope": SCOPES,
        "code_challenge_method": "S256",
        "code_challenge": challenge,
        "state": state,
    }
    auth_url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode(params)

    server = HTTPServer(("127.0.0.1", 8888), Handler)
    print("\nSe abrirá Spotify en el navegador.")
    print("Autoriza la app y espera a que vuelva a esta ventana.\n")
    webbrowser.open(auth_url)
    server.handle_request()
    server.server_close()

    if result.get("error"):
        print("Spotify devolvió un error:", result["error"])
        return
    if result.get("state") != state:
        print("ERROR: state no coincide. Cancelado por seguridad.")
        return
    code = result.get("code")
    if not code:
        print("No se recibió código de autorización.")
        return

    token = post_form(
        "https://accounts.spotify.com/api/token",
        {
            "grant_type": "authorization_code",
            "code": code,
            "redirect_uri": REDIRECT_URI,
            "client_id": client_id,
            "code_verifier": verifier,
        },
    )

    token["client_id"] = client_id
    token["obtained_at"] = int(time.time())
    token["expires_at"] = int(time.time()) + int(token.get("expires_in", 3600))

    out = "spotify_auth.json"
    with open(out, "w", encoding="utf-8") as f:
        json.dump(token, f, indent=2)

    print("\nOK: autorización completada.")
    print(f"Credenciales guardadas localmente en: {out}")
    print("IMPORTANTE: NO compartas ese archivo; contiene tu refresh token.\n")

    current = api_get(
        "https://api.spotify.com/v1/me/player/currently-playing",
        token["access_token"],
    )
    if not current:
        print("Spotify responde correctamente, pero ahora mismo no detecta reproducción activa.")
        print("Pon una canción en tu Google Home y después podremos probarla.")
        return

    item = current.get("item") or {}
    name = item.get("name", "(sin título)")
    artists = ", ".join(a.get("name", "") for a in item.get("artists", []))
    device = "(el endpoint currently-playing no siempre devuelve el nombre del dispositivo)"
    progress_ms = current.get("progress_ms") or 0
    duration_ms = item.get("duration_ms") or 0

    def mmss(ms):
        s = max(0, int(ms // 1000))
        return f"{s//60}:{s%60:02d}"

    print("REPRODUCIENDO:")
    print("  Canción :", name)
    print("  Artista :", artists)
    print("  Progreso:", f"{mmss(progress_ms)} / {mmss(duration_ms)}")
    print("  Estado  :", "PLAY" if current.get("is_playing") else "PAUSA")

if __name__ == "__main__":
    main()
