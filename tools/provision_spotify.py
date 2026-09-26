#!/usr/bin/env python3
import json
import os
import sys
import urllib.request
import urllib.error
from pathlib import Path

def find_auth_file():
    candidates = [
        Path.cwd() / "spotify_auth.json",
        Path.home() / "Downloads" / "spotify_auth.json",
        Path(__file__).resolve().parent.parent / "spotify_auth.json",
    ]
    for p in candidates:
        if p.exists():
            return p
    return None

def main():
    print("=== GeekMagic Spotify - aprovisionamiento ===")
    auth_path = find_auth_file()
    if not auth_path:
        typed = input("Ruta de spotify_auth.json: ").strip().strip('"')
        auth_path = Path(typed)

    if not auth_path.exists():
        print("No encuentro spotify_auth.json")
        sys.exit(1)

    data = json.loads(auth_path.read_text(encoding="utf-8"))
    client_id = data.get("client_id", "")
    refresh_token = data.get("refresh_token", "")
    authorized_at = int(data.get("obtained_at", 0) or 0)

    if not client_id or not refresh_token:
        print("El archivo no contiene client_id/refresh_token.")
        sys.exit(1)

    default_host = "geekspotify.local"
    ip = input(f"IP o hostname actual del GeekMagic [{default_host}]: ").strip() or default_host
    base = f"http://{ip}"

    try:
        with urllib.request.urlopen(base + "/api/status", timeout=5) as r:
            status = json.loads(r.read().decode("utf-8"))
        print("Dispositivo encontrado:", status.get("firmware", "?"))
    except Exception as e:
        print("No puedo contactar con el GeekMagic:", e)
        print("Comprueba su IP y que el PC esté en la misma WiFi.")
        sys.exit(1)

    payload = json.dumps({
        "client_id": client_id,
        "refresh_token": refresh_token,
        "authorized_at": authorized_at,
    }).encode("utf-8")

    req = urllib.request.Request(
        base + "/api/spotify",
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=8) as r:
            response = r.read().decode("utf-8")
        print("OK. Spotify enviado al cubo.")
        print("Respuesta:", response)
        print()
        print("Ya puedes cerrar el PC. El GeekMagic renovará sus access tokens por sí mismo.")
        print("El refresh token caduca a los 180 días y habrá que autorizar de nuevo.")
    except urllib.error.HTTPError as e:
        print("HTTP", e.code, e.read().decode("utf-8", errors="replace"))
        sys.exit(1)
    except Exception as e:
        print("Error:", e)
        sys.exit(1)

if __name__ == "__main__":
    main()
