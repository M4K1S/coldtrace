# ColdTrace server

One Flask file, [`flask_app.py`](flask_app.py), that the device talks to. It stores readings and hourly history on disk, serves the live dashboard and the per-day graphs, and holds the settings the device picks up (timezone, alarm threshold, alarm delay). It runs on PythonAnywhere at `coldtrace.pythonanywhere.com`.

> **AI assistance:** this server was written and revised by Claude (Anthropic's AI model, via Claude Code), working from requirements and decisions given by Maxim Kisselev, who set up the deployment and tested it against the real device.

## Routes

| Route | Purpose |
|---|---|
| `POST /api/reading` | live reading from the device (every 5 s) |
| `POST /api/hourly` | one finished hour (live or catch-up after an outage); the latest line per hour wins, so repeats are harmless |
| `GET /api/data` | latest data for the dashboard. Every response also carries `X-Tz-Offset`, `X-Alarm-Threshold`, `X-Alarm-Delay` and `X-Last-Hour`, which is how the device gets its settings and learns what it needs to re-send |
| `GET /dashboard` | live view |
| `GET /history?day=YYMMDD` | per-day graph of hourly min/avg/max with a door strip |
| `/admin` | set timezone offset, alarm threshold and alarm delay (password) |

## Files it creates (next to `flask_app.py`)

| File | Contents |
|---|---|
| `readings.jsonl` | recent live readings, cut back automatically so it can't grow without limit |
| `hourly.jsonl` | one line per finished hour, kept permanently |
| `summaries.jsonl` | older summary records |
| `tz_offset.json`, `alarm_threshold.json`, `alarm_delay.json` | settings saved from `/admin` |

None of these belong in git.

## Admin password

The password is **not** in the code. `/admin` stays disabled (HTTP 503) until you set one, either way:

- **Environment variable:** `COLDTRACE_ADMIN_PASSWORD`
- **File:** a one-line `admin_password.txt` next to `flask_app.py` (ignored by git)

The username is `admin`.

## Deploy on PythonAnywhere

1. Open `flask_app.py` in the **Files** tab (in the web app's source folder) and paste in the contents of this file.
2. Set the password. Easiest: create `admin_password.txt` in that same folder with just the password in it. Alternatively, add `os.environ["COLDTRACE_ADMIN_PASSWORD"] = "..."` near the top of the web app's WSGI file, above the line that imports the app.
3. **Web** tab, then **Reload**.
4. Check `/dashboard` loads, and that `/admin` asks for the password. Use `https://` when you open `/admin` so the password isn't sent in the clear.

## Run locally

```bash
pip install flask
COLDTRACE_ADMIN_PASSWORD=choose-one python flask_app.py
```

Then open `http://localhost:5000/dashboard`. Data files are created in the folder you run it from.

## Known limits

- The device does not authenticate itself, so anyone who knows the URL could post readings. A shared device key is a planned improvement.
- The device speaks plain HTTP (no TLS); see the main README's roadmap.
- Data lives in files on the web host's disk. Back it up if the history matters, and remember PythonAnywhere's free tier needs a periodic "keep alive" click.
