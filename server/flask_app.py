"""
ColdTrace backend: receives readings and finished hours from the device and
serves the live dashboard, per-day history graphs and the /admin settings page.

Run locally:
    pip install flask
    COLDTRACE_ADMIN_PASSWORD=choose-one python flask_app.py
Then visit http://localhost:5000/dashboard

On PythonAnywhere, paste this file in as flask_app.py and set the admin
password as described in server/README.md.
"""

from flask import Flask, request, jsonify, render_template_string, Response
from datetime import datetime, timezone
from functools import wraps
import hmac
import json
import os

app = Flask(__name__)

# ---------------------------------------------------------------------------
# Config — tune these for your install
# ---------------------------------------------------------------------------
LOG_FILE = "readings.jsonl"
SUMMARY_LOG_FILE = "summaries.jsonl"
SUMMARY_HISTORY_SIZE = 90    # 3x/day * 30 days
HISTORY_SIZE = 50            # readings kept in memory / shown on the trace
LOG_TRIM_AT = HISTORY_SIZE * 4  # readings.jsonl is cut back to HISTORY_SIZE lines at this length
STALE_AFTER_SECONDS = 600    # no reading in this long -> "stale" indicator
SAFE_TEMP_LOW = -22.0        # °C — freezer safe band
SAFE_TEMP_HIGH = -15.0
WARN_MARGIN = 2.0            # °C inside the high bound counts as "warning"
DEVICE_NAME = "Walk-in Freezer"
DEVICE_LOCATION = "Elora, ON — back kitchen"
ADMIN_USERNAME = "admin"
# fallback if the env var is not set; looked up next to this file, whatever the working directory
ADMIN_PASSWORD_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "admin_password.txt")
TZ_OFFSET_FILE = "tz_offset.json"
DEFAULT_TZ_OFFSET_HOURS = -4  # EDT; -5 for EST
HOURLY_LOG_FILE = "hourly.jsonl"   # one line per finished hour, sent by the device (live or catch-up)
ALARM_THRESHOLD_FILE = "alarm_threshold.json"
DEFAULT_ALARM_THRESHOLD_C = -15.0  # freezer - alarm if it gets warmer than this
ALARM_DELAY_FILE = "alarm_delay.json"
DEFAULT_ALARM_DELAY_MIN = 15  # must stay over the threshold this long before alarming
ALARM_DELAY_RANGE = (1, 240)

# In-memory ring buffer so /dashboard and /api/data don't re-read the file
# on every request. Reloaded from disk on startup.
_history = []
_summary_history = []


def _load_jsonl(path, limit):
    if not os.path.exists(path):
        return []
    with open(path, "r") as f:
        lines = f.readlines()[-limit:]
    out = []
    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return out


_history = _load_jsonl(LOG_FILE, HISTORY_SIZE)


def _count_lines(path):
    if not os.path.exists(path):
        return 0
    with open(path, "r") as f:
        return sum(1 for _ in f)


# readings.jsonl only restores the live graph after a restart; the permanent
# record is hourly.jsonl, so it is kept short instead of growing forever.
def _trim_log():
    tmp = LOG_FILE + ".tmp"
    with open(tmp, "w") as f:
        for entry in _history:
            f.write(json.dumps(entry) + "\n")
    os.replace(tmp, LOG_FILE)


_log_lines = _count_lines(LOG_FILE)
if _log_lines > LOG_TRIM_AT:
    _trim_log()
    _log_lines = len(_history)
_summary_history = _load_jsonl(SUMMARY_LOG_FILE, SUMMARY_HISTORY_SIZE)

# Hourly history, keyed by the device's hour key: yymmddhh in the device's
# LOCAL time (26100914 = 2026-10-09 14:00). Re-sent hours just overwrite, so
# uploads are safe to repeat; the newest line for a key wins on reload.
_hourly = {}


def _valid_hour_key(key):
    yy, mm, dd, hh = key // 1000000, key // 10000 % 100, key // 100 % 100, key % 100
    return 20 <= yy <= 99 and 1 <= mm <= 12 and 1 <= dd <= 31 and 0 <= hh <= 23


def _load_hourly():
    if not os.path.exists(HOURLY_LOG_FILE):
        return
    with open(HOURLY_LOG_FILE, "r") as f:
        for line in f:
            try:
                e = json.loads(line)
                _hourly[int(e["key"])] = e
            except (json.JSONDecodeError, KeyError, ValueError, TypeError):
                continue


_load_hourly()


def get_tz_offset():
    if not os.path.exists(TZ_OFFSET_FILE):
        return DEFAULT_TZ_OFFSET_HOURS
    try:
        with open(TZ_OFFSET_FILE, "r") as f:
            return int(json.load(f)["hours"])
    except (json.JSONDecodeError, KeyError, ValueError, OSError):
        return DEFAULT_TZ_OFFSET_HOURS


def set_tz_offset(hours):
    with open(TZ_OFFSET_FILE, "w") as f:
        json.dump({"hours": hours}, f)


def get_alarm_threshold():
    if not os.path.exists(ALARM_THRESHOLD_FILE):
        return DEFAULT_ALARM_THRESHOLD_C
    try:
        with open(ALARM_THRESHOLD_FILE, "r") as f:
            return float(json.load(f)["celsius"])
    except (json.JSONDecodeError, KeyError, ValueError, OSError):
        return DEFAULT_ALARM_THRESHOLD_C


def get_alarm_delay():
    if not os.path.exists(ALARM_DELAY_FILE):
        return DEFAULT_ALARM_DELAY_MIN
    try:
        with open(ALARM_DELAY_FILE, "r") as f:
            return int(json.load(f)["minutes"])
    except (json.JSONDecodeError, KeyError, ValueError, OSError):
        return DEFAULT_ALARM_DELAY_MIN


def set_alarm_delay(minutes):
    with open(ALARM_DELAY_FILE, "w") as f:
        json.dump({"minutes": minutes}, f)


def set_alarm_threshold(celsius):
    with open(ALARM_THRESHOLD_FILE, "w") as f:
        json.dump({"celsius": celsius}, f)


def get_admin_password():
    """Env var first, then admin_password.txt next to the app; None if neither is set."""
    password = os.environ.get("COLDTRACE_ADMIN_PASSWORD", "")
    if not password and os.path.exists(ADMIN_PASSWORD_FILE):
        with open(ADMIN_PASSWORD_FILE, "r") as f:
            password = f.read().strip()
    return password or None


def require_admin(view):
    @wraps(view)
    def wrapped(*args, **kwargs):
        expected = get_admin_password()
        if expected is None:
            return Response("Admin disabled: no admin password is configured on the server", 503)
        auth = request.authorization
        if (
            not auth
            or not hmac.compare_digest(auth.username.encode(), ADMIN_USERNAME.encode())
            or not hmac.compare_digest((auth.password or "").encode(), expected.encode())
        ):
            return Response(
                "Admin login required", 401, {"WWW-Authenticate": 'Basic realm="ColdTrace Admin"'}
            )
        return view(*args, **kwargs)
    return wrapped


# Added to every response (not just /api/data) so the firmware's plain HEAD
# request for time sync always carries the current settings, regardless of
# which route/branch actually handled it.
#
# X-Last-Hour is the newest hourly record this server has (0 = none). The
# device uploads every saved hour newer than that, which is how it catches
# the server up after a WiFi outage or a reboot.
@app.after_request
def add_device_headers(response):
    response.headers["X-Tz-Offset"] = str(get_tz_offset())
    response.headers["X-Alarm-Threshold"] = f"{get_alarm_threshold():.1f}"
    response.headers["X-Alarm-Delay"] = str(get_alarm_delay())
    response.headers["X-Last-Hour"] = str(max(_hourly) if _hourly else 0)
    return response


# ---------------------------------------------------------------------------
# Ingestion
# ---------------------------------------------------------------------------
@app.route("/api/reading", methods=["POST"])
def receive_reading():
    data = request.get_json(silent=True)

    if not data or "temp" not in data:
        return jsonify({"error": "missing data"}), 400

    try:
        temp = float(data.get("temp"))
    except (TypeError, ValueError):
        return jsonify({"error": "temp must be a number"}), 400

    entry = {
        "received_at": datetime.now(timezone.utc).isoformat(),
        "temp": temp,
        "door": bool(data.get("door")),
        "device_timestamp": data.get("timestamp"),
    }

    global _log_lines
    with open(LOG_FILE, "a") as f:
        f.write(json.dumps(entry) + "\n")
    _log_lines += 1

    _history.append(entry)
    if len(_history) > HISTORY_SIZE:
        del _history[0 : len(_history) - HISTORY_SIZE]

    if _log_lines > LOG_TRIM_AT:
        _trim_log()
        _log_lines = len(_history)

    return jsonify({"status": "ok"}), 200


# 3x/day avg/min/max summary, instead of a reading every ~1s
@app.route("/api/summary", methods=["POST"])
def receive_summary():
    data = request.get_json(silent=True)

    if not data or "avg" not in data or "min" not in data or "max" not in data:
        return jsonify({"error": "missing data"}), 400

    try:
        avg_temp = float(data.get("avg"))
        min_temp = float(data.get("min"))
        max_temp = float(data.get("max"))
    except (TypeError, ValueError):
        return jsonify({"error": "avg/min/max must be numbers"}), 400

    entry = {
        "received_at": datetime.now(timezone.utc).isoformat(),
        "avg": avg_temp,
        "min": min_temp,
        "max": max_temp,
        "door": bool(data.get("door")),
        "device_timestamp": data.get("timestamp"),
    }

    with open(SUMMARY_LOG_FILE, "a") as f:
        f.write(json.dumps(entry) + "\n")

    _summary_history.append(entry)
    if len(_summary_history) > SUMMARY_HISTORY_SIZE:
        del _summary_history[0 : len(_summary_history) - SUMMARY_HISTORY_SIZE]

    return jsonify({"status": "ok"}), 200


@app.route("/api/hourly", methods=["POST"])
def receive_hourly():
    data = request.get_json(silent=True) or {}
    try:
        key = int(data["key"])
        entry = {
            "key": key,
            "min": int(data["min_x100"]) / 100.0,
            "max": int(data["max_x100"]) / 100.0,
            "avg": int(data["avg_x100"]) / 100.0,
            "door_samples": int(data.get("door", 0)),
            "samples": int(data["n"]),
        }
    except (KeyError, TypeError, ValueError):
        return jsonify({"error": "missing or invalid fields"}), 400

    if not _valid_hour_key(key) or entry["samples"] <= 0:
        return jsonify({"error": "bad key or sample count"}), 400

    entry["received_at"] = datetime.now(timezone.utc).isoformat()
    with open(HOURLY_LOG_FILE, "a") as f:
        f.write(json.dumps(entry) + "\n")
    _hourly[key] = entry

    return jsonify({"status": "ok"}), 200


@app.route("/api/summaries", methods=["GET"])
def api_summaries():
    return jsonify({"summaries": list(reversed(_summary_history))})


# ---------------------------------------------------------------------------
# JSON API for the dashboard's polling — keeps /dashboard from needing a
# full page reload every few seconds
# ---------------------------------------------------------------------------
@app.route("/api/data", methods=["GET"])
def api_data():
    if not _history:
        return jsonify({"has_data": False})

    latest = _history[-1]
    received_at = datetime.fromisoformat(latest["received_at"])
    if received_at.tzinfo is None:
        received_at = received_at.replace(tzinfo=timezone.utc)
    age_seconds = (datetime.now(timezone.utc) - received_at).total_seconds()

    temps = [r["temp"] for r in _history]

    # Time since the door was last open, for the "last opened Xm ago" line
    last_open_at = None
    for r in reversed(_history[:-1]):
        if r.get("door"):
            last_open_at = r["received_at"]
            break

    return jsonify(
        {
            "has_data": True,
            "temp": latest["temp"],
            "door": bool(latest.get("door")),
            "received_at": latest["received_at"],
            "age_seconds": age_seconds,
            "is_stale": age_seconds > STALE_AFTER_SECONDS,
            "history": temps,
            "history_min": min(temps),
            "history_max": max(temps),
            "safe_low": SAFE_TEMP_LOW,
            "safe_high": SAFE_TEMP_HIGH,
            "warn_at": SAFE_TEMP_HIGH - WARN_MARGIN,
            "last_open_at": last_open_at,
            "device_name": DEVICE_NAME,
            "device_location": DEVICE_LOCATION,
        }
    )


# ---------------------------------------------------------------------------
# Admin - lets the UTC offset be changed without reflashing the firmware.
# The firmware picks it up automatically via the X-Tz-Offset header on its
# next sync (on boot, or daily at local noon).
# ---------------------------------------------------------------------------
@app.route("/admin", methods=["GET", "POST"])
@require_admin
def admin():
    saved = False
    if request.method == "POST":
        try:
            hours = int(request.form.get("tz_offset", ""))
            if -12 <= hours <= 14:
                set_tz_offset(hours)
                saved = True
        except ValueError:
            pass
        try:
            threshold = float(request.form.get("alarm_threshold", ""))
            if -40 <= threshold <= 40:
                set_alarm_threshold(threshold)
                saved = True
        except ValueError:
            pass
        try:
            delay = int(request.form.get("alarm_delay", ""))
            if ALARM_DELAY_RANGE[0] <= delay <= ALARM_DELAY_RANGE[1]:
                set_alarm_delay(delay)
                saved = True
        except ValueError:
            pass
    return render_template_string(
        ADMIN_TEMPLATE,
        tz_offset=get_tz_offset(),
        alarm_threshold=get_alarm_threshold(),
        alarm_delay=get_alarm_delay(),
        saved=saved,
    )


ADMIN_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>ColdTrace Admin</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  :root{ --steel-900:#0D1416; --steel-800:#161F22; --steel-700:#212C2F;
    --frost:#7FE0D6; --ink:#E9F1F0; --muted:#8A9598; --hairline:rgba(233,241,240,0.08); }
  *{ box-sizing:border-box; }
  html,body{ margin:0; padding:0; min-height:100vh; background:var(--steel-900);
    color:var(--ink); font-family:system-ui, sans-serif; }
  body{ display:flex; justify-content:center; padding:28px 16px; }
  .panel{ width:100%; max-width:380px; background:var(--steel-800); border-radius:12px;
    padding:20px; border:1px solid var(--hairline); }
  h1{ font-size:16px; margin:0 0 16px; }
  label{ font-size:12px; color:var(--muted); display:block; margin-bottom:6px; }
  input[type=number]{ width:100%; padding:9px 10px; border-radius:8px; border:1px solid var(--hairline);
    background:var(--steel-700); color:var(--ink); font-size:14px; margin-bottom:12px; }
  button{ width:100%; padding:10px; border-radius:8px; border:none; background:var(--frost);
    color:#08201d; font-weight:600; font-size:14px; cursor:pointer; }
  .saved{ font-size:12px; color:var(--frost); margin-bottom:12px; }
  .hint{ font-size:11px; color:var(--muted); margin-top:12px; line-height:1.5; }
</style>
</head>
<body>
  <div class="panel">
    <h1>ColdTrace Settings</h1>
    {% if saved %}<div class="saved">Saved - the device will pick this up on its next sync.</div>{% endif %}
    <form method="POST">
      <label for="tz_offset">Hours offset from UTC (e.g. -4 for EDT, -5 for EST)</label>
      <input type="number" id="tz_offset" name="tz_offset" value="{{ tz_offset }}" min="-12" max="14" step="1">
      <label for="alarm_threshold">Alarm threshold (&deg;C) - alarms if temp rises above this</label>
      <input type="number" id="alarm_threshold" name="alarm_threshold" value="{{ alarm_threshold }}" min="-40" max="40" step="0.5">
      <label for="alarm_delay">Alarm delay (minutes) - how long it must stay over the threshold before alarming</label>
      <input type="number" id="alarm_delay" name="alarm_delay" value="{{ alarm_delay }}" min="1" max="240" step="1">
      <button type="submit">Save</button>
    </form>
    <div class="hint">The firmware reads these settings from the same request it already makes for time sync (on boot and daily at 12:01 local) - no reflash needed, just wait for the next sync.</div>
  </div>
</body>
</html>
"""


# ---------------------------------------------------------------------------
# Dashboard
# ---------------------------------------------------------------------------
@app.route("/dashboard", methods=["GET"])
def dashboard():
    return render_template_string(DASHBOARD_TEMPLATE)


# ---------------------------------------------------------------------------
# History - one day at a time: hourly min / avg / max on a graph, a door
# strip under it, and the numbers in a table.
# ---------------------------------------------------------------------------
CHART_W, CHART_H = 600, 220
CHART_L, CHART_R, CHART_T, CHART_B = 46, 14, 14, 26


def _runs(slots):
    """Split 24 hour slots into runs of consecutive hours that have data, so a
    gap (device off, outage not yet caught up) breaks the line instead of
    drawing a misleading straight segment across it."""
    run, out = [], []
    for h, e in enumerate(slots):
        if e is None:
            if run:
                out.append(run)
            run = []
        else:
            run.append((h, e))
    if run:
        out.append(run)
    return out


def _build_chart(slots):
    present = [e for e in slots if e is not None]
    if not present:
        return ""

    lo = min(e["min"] for e in present)
    hi = max(e["max"] for e in present)
    if hi - lo < 1.0:                       # a flat day still gets a readable scale
        mid = (hi + lo) / 2.0
        lo, hi = mid - 0.5, mid + 0.5
    pad = (hi - lo) * 0.08
    lo, hi = lo - pad, hi + pad

    plot_w = CHART_W - CHART_L - CHART_R
    plot_h = CHART_H - CHART_T - CHART_B

    def x(h):
        # centered in its column, so each point sits right above its cell in the door strip
        return CHART_L + (h + 0.5) * plot_w / 24.0

    def y(v):
        return CHART_T + (1 - (v - lo) / (hi - lo)) * plot_h

    parts = []

    # horizontal grid + temperature labels
    for i in range(5):
        v = lo + (hi - lo) * i / 4.0
        parts.append(f'<line class="grid" x1="{CHART_L}" y1="{y(v):.1f}" x2="{CHART_W - CHART_R}" y2="{y(v):.1f}"/>')
        parts.append(f'<text class="axis" x="{CHART_L - 6}" y="{y(v) + 3:.1f}" text-anchor="end">{v:.1f}</text>')

    # hour labels every 3h
    for h in range(0, 24, 3):
        parts.append(f'<text class="axis" x="{x(h):.1f}" y="{CHART_H - 8}" text-anchor="middle">{h:02d}:00</text>')

    # alarm threshold, when it falls inside the visible range
    thr = get_alarm_threshold()
    if lo <= thr <= hi:
        parts.append(f'<line class="thr" x1="{CHART_L}" y1="{y(thr):.1f}" x2="{CHART_W - CHART_R}" y2="{y(thr):.1f}"/>')
        parts.append(f'<text class="axis thr-label" x="{CHART_W - CHART_R}" y="{y(thr) - 4:.1f}" text-anchor="end">ALARM {thr:.1f}</text>')

    for run in _runs(slots):
        top = " ".join(f"{x(h):.1f},{y(e['max']):.1f}" for h, e in run)
        bot = " ".join(f"{x(h):.1f},{y(e['min']):.1f}" for h, e in reversed(run))
        if len(run) > 1:
            parts.append(f'<polygon class="band" points="{top} {bot}"/>')
            for cls, key in (("l-max", "max"), ("l-avg", "avg"), ("l-min", "min")):
                pts = " ".join(f"{x(h):.1f},{y(e[key]):.1f}" for h, e in run)
                parts.append(f'<polyline class="{cls}" points="{pts}"/>')
        else:                               # lone hour: draw it as a vertical min-max tick + avg dot
            h, e = run[0]
            parts.append(f'<line class="l-avg" x1="{x(h):.1f}" y1="{y(e["max"]):.1f}" x2="{x(h):.1f}" y2="{y(e["min"]):.1f}"/>')
            parts.append(f'<circle class="dot" cx="{x(h):.1f}" cy="{y(e["avg"]):.1f}" r="3"/>')

    # invisible per-hour hit areas carrying the hover text
    col_w = plot_w / 24.0
    for h, e in enumerate(slots):
        if e is None:
            continue
        door_min = round(60.0 * e["door_samples"] / e["samples"])
        tip = f'{h:02d}:00  min {e["min"]:.1f}  avg {e["avg"]:.1f}  max {e["max"]:.1f}  door open ~{door_min} min'
        parts.append(f'<rect class="hit" x="{x(h) - col_w / 2:.1f}" y="{CHART_T}" width="{col_w:.1f}" height="{plot_h}"><title>{tip}</title></rect>')

    return (f'<svg class="chart" viewBox="0 0 {CHART_W} {CHART_H}" preserveAspectRatio="xMidYMid meet">'
            + "".join(parts) + "</svg>")


def _door_strip(slots):
    cells = []
    for h, e in enumerate(slots):
        if e is None:
            cells.append('<div class="cell none" title="%02d:00 no data"></div>' % h)
            continue
        frac = e["door_samples"] / e["samples"]
        mins = round(60.0 * frac)
        if mins <= 0:
            cells.append('<div class="cell closed" title="%02d:00 door stayed closed"></div>' % h)
        else:
            alpha = min(1.0, 0.35 + frac * 4)
            cells.append(f'<div class="cell open" style="opacity:{alpha:.2f}" title="{h:02d}:00 door open ~{mins} min"></div>')
    return "".join(cells)


@app.route("/history", methods=["GET"])
def history():
    days = sorted({k // 100 for k in _hourly})          # yymmdd
    day = request.args.get("day", type=int)
    if day not in days:
        day = days[-1] if days else None

    slots = [_hourly.get(day * 100 + h) for h in range(24)] if day else [None] * 24
    present = [e for e in slots if e is not None]

    prev_day = next_day = None
    label = ""
    if day:
        i = days.index(day)
        prev_day = days[i - 1] if i > 0 else None
        next_day = days[i + 1] if i < len(days) - 1 else None
        label = datetime(2000 + day // 10000, day // 100 % 100, day % 100).strftime("%a %b %-d, %Y")

    rows = [
        {
            "hour": f"{h:02d}:00",
            "min": f"{e['min']:.1f}", "avg": f"{e['avg']:.1f}", "max": f"{e['max']:.1f}",
            "door": round(60.0 * e["door_samples"] / e["samples"]),
        }
        for h, e in enumerate(slots) if e is not None
    ]

    stats = None
    if present:
        stats = {
            "lo": f"{min(e['min'] for e in present):.1f}",
            "hi": f"{max(e['max'] for e in present):.1f}",
            "avg": f"{sum(e['avg'] * e['samples'] for e in present) / sum(e['samples'] for e in present):.1f}",
            "hours": len(present),
        }

    return render_template_string(
        HISTORY_TEMPLATE,
        device_name=DEVICE_NAME, label=label, chart=_build_chart(slots), door_strip=_door_strip(slots),
        rows=rows, stats=stats, prev_day=prev_day, next_day=next_day,
    )


HISTORY_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>ColdTrace History</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500;600&family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
<style>
  :root{
    --steel-900:#0D1416; --steel-800:#161F22; --steel-700:#212C2F;
    --frost:#7FE0D6; --frost-dim:#3E7A73; --amber:#F2A93B; --red:#E14B4B; --blue:#6FA8FF;
    --ink:#E9F1F0; --muted:#8A9598; --hairline:rgba(233,241,240,0.08);
    --mono:'IBM Plex Mono', ui-monospace, monospace; --sans:'Inter', system-ui, sans-serif;
  }
  *{ box-sizing:border-box; }
  html,body{ margin:0; padding:0; min-height:100vh;
    background:radial-gradient(ellipse at 50% -10%, #1a2427 0%, var(--steel-900) 55%);
    color:var(--ink); font-family:var(--sans); -webkit-font-smoothing:antialiased; }
  body{ display:flex; justify-content:center; padding:28px 16px 60px; }
  .panel{ width:100%; max-width:640px; }
  .top{ display:flex; justify-content:space-between; align-items:baseline; margin-bottom:14px; }
  .title{ font-size:17px; font-weight:600; }
  .back{ font-family:var(--mono); font-size:10.5px; color:var(--frost-dim); text-decoration:none; }
  .daynav{ display:flex; align-items:center; justify-content:space-between; margin-bottom:12px;
    font-family:var(--mono); font-size:12px; }
  .daynav a{ color:var(--frost); text-decoration:none; padding:4px 10px; border:1px solid var(--hairline);
    border-radius:20px; background:var(--steel-700); }
  .daynav a.off{ color:var(--muted); opacity:.35; pointer-events:none; }
  .card{ background:var(--steel-700); border-radius:9px; padding:14px 14px 12px;
    border:1px solid var(--hairline); margin-top:14px; }
  .card-head{ display:flex; justify-content:space-between; font-size:10px; color:var(--muted);
    margin-bottom:8px; font-family:var(--mono); }
  .legend{ display:flex; gap:12px; font-family:var(--mono); font-size:10px; color:var(--muted); margin-top:8px; }
  .legend i{ display:inline-block; width:14px; height:2px; vertical-align:middle; margin-right:5px; }
  svg.chart{ width:100%; height:auto; display:block; }
  .grid{ stroke:var(--hairline); stroke-width:1; }
  .axis{ fill:var(--muted); font-family:var(--mono); font-size:9.5px; }
  .thr{ stroke:var(--amber); stroke-width:1; stroke-dasharray:4 4; opacity:.7; }
  .thr-label{ fill:var(--amber); opacity:.8; }
  .band{ fill:rgba(127,224,214,0.10); stroke:none; }
  polyline, .l-avg{ fill:none; stroke-linejoin:round; stroke-linecap:round; }
  .l-max{ stroke:var(--red); stroke-width:1.3; }
  .l-min{ stroke:var(--blue); stroke-width:1.3; }
  .l-avg{ stroke:var(--frost); stroke-width:2; filter:drop-shadow(0 0 3px rgba(127,224,214,0.6)); }
  .dot{ fill:var(--frost); }
  .hit{ fill:transparent; }
  .hit:hover{ fill:rgba(255,255,255,0.05); }
  .strip{ display:grid; grid-template-columns:repeat(24,1fr); gap:2px; margin-left:calc(46 / 600 * 100%); margin-right:calc(14 / 600 * 100%); }
  .cell{ height:14px; border-radius:2px; }
  .cell.none{ background:transparent; border:1px dashed var(--hairline); }
  .cell.closed{ background:rgba(127,224,214,0.18); }
  .cell.open{ background:var(--red); }
  .stats{ display:flex; gap:10px; margin-top:14px; }
  .stat{ flex:1; background:var(--steel-700); border:1px solid var(--hairline); border-radius:9px; padding:10px 12px; }
  .stat .k{ font-size:10px; color:var(--muted); margin-bottom:4px; }
  .stat .v{ font-family:var(--mono); font-size:16px; font-weight:500; }
  table{ width:100%; border-collapse:collapse; margin-top:14px; background:var(--steel-800); border-radius:10px; overflow:hidden; }
  th, td{ font-family:var(--mono); font-size:12px; padding:8px 10px; text-align:right; border-bottom:1px solid var(--hairline); }
  th:first-child, td:first-child{ text-align:left; }
  th{ color:var(--muted); font-weight:500; font-size:10.5px; background:var(--steel-700); }
  tr:last-child td{ border-bottom:none; }
  td.door{ color:var(--red); }
  .empty{ text-align:center; padding:60px 20px; color:var(--muted); font-family:var(--mono); font-size:13px; }
</style>
</head>
<body>
  <div class="panel">
    <div class="top">
      <div class="title">{{ device_name }} &mdash; History</div>
      <a class="back" href="/dashboard">&larr; LIVE</a>
    </div>

    {% if label %}
    <div class="daynav">
      <a class="{{ '' if prev_day else 'off' }}" href="/history?day={{ prev_day }}">&larr; PREV</a>
      <span>{{ label }}</span>
      <a class="{{ '' if next_day else 'off' }}" href="/history?day={{ next_day }}">NEXT &rarr;</a>
    </div>

    <div class="card">
      <div class="card-head"><span>HOURLY MIN / AVG / MAX</span><span>{{ stats.lo }} to {{ stats.hi }}&deg;C</span></div>
      {{ chart|safe }}
      <div class="legend">
        <span><i style="background:var(--red)"></i>MAX</span>
        <span><i style="background:var(--frost)"></i>AVG</span>
        <span><i style="background:var(--blue)"></i>MIN</span>
      </div>
    </div>

    <div class="card">
      <div class="card-head"><span>DOOR OPEN, BY HOUR</span><span>brighter = longer</span></div>
      <div class="strip">{{ door_strip|safe }}</div>
    </div>

    <div class="stats">
      <div class="stat"><div class="k">Day low</div><div class="v">{{ stats.lo }}&deg;C</div></div>
      <div class="stat"><div class="k">Day average</div><div class="v">{{ stats.avg }}&deg;C</div></div>
      <div class="stat"><div class="k">Day high</div><div class="v">{{ stats.hi }}&deg;C</div></div>
    </div>

    <table>
      <tr><th>Hour</th><th>Min</th><th>Avg</th><th>Max</th><th>Door open</th></tr>
      {% for r in rows %}
      <tr>
        <td>{{ r.hour }}</td><td>{{ r.min }}</td><td>{{ r.avg }}</td><td>{{ r.max }}</td>
        <td class="{{ 'door' if r.door > 0 else '' }}">{{ r.door }} min</td>
      </tr>
      {% endfor %}
    </table>
    {% else %}
    <div class="empty">No hourly history received yet.<br>The first hour is recorded once the clock rolls over.</div>
    {% endif %}
  </div>
</body>
</html>
"""


DASHBOARD_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>ColdTrace</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500;600&family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
<style>
  :root{
    --steel-900:#0D1416; --steel-800:#161F22; --steel-700:#212C2F; --well:#080B0C;
    --frost:#7FE0D6; --frost-dim:#3E7A73; --amber:#F2A93B; --red:#E14B4B;
    --ink:#E9F1F0; --muted:#8A9598; --hairline:rgba(233,241,240,0.08);
    --mono:'IBM Plex Mono', ui-monospace, monospace; --sans:'Inter', system-ui, sans-serif;
  }
  *{ box-sizing:border-box; }
  html,body{ margin:0; padding:0; min-height:100vh;
    background:radial-gradient(ellipse at 50% -10%, #1a2427 0%, var(--steel-900) 55%);
    color:var(--ink); font-family:var(--sans); -webkit-font-smoothing:antialiased; }
  body{ display:flex; justify-content:center; align-items:flex-start; padding:28px 16px 60px; }
  .panel{ width:100%; max-width:420px; background:linear-gradient(180deg, var(--steel-800), var(--steel-900));
    border-radius:14px; border:1px solid rgba(255,255,255,0.06);
    box-shadow:0 30px 60px -20px rgba(0,0,0,0.6), inset 0 1px 0 rgba(255,255,255,0.04);
    padding:22px 20px 18px; position:relative; }
  .screw{ position:absolute; width:7px; height:7px; border-radius:50%;
    background:radial-gradient(circle at 35% 35%, #4a5658, #10171a 70%);
    box-shadow:0 1px 1px rgba(0,0,0,0.6), inset 0 0 1px rgba(255,255,255,0.3); }
  .screw::after{ content:""; position:absolute; top:50%; left:50%; width:5px; height:1px;
    background:rgba(0,0,0,0.5); transform:translate(-50%,-50%) rotate(35deg); }
  .screw.tl{ top:10px; left:10px; } .screw.tr{ top:10px; right:10px; }
  .screw.bl{ bottom:10px; left:10px; } .screw.br{ bottom:10px; right:10px; }
  .header{ display:flex; justify-content:space-between; align-items:flex-start;
    padding:2px 6px 18px; border-bottom:1px solid var(--hairline); margin-bottom:18px; }
  .device-id{ font-size:11px; letter-spacing:0.02em; color:var(--muted); margin-bottom:3px; }
  .device-name{ font-size:17px; font-weight:600; }
  .location{ font-size:12px; color:var(--muted); margin-top:2px; }
  .conn{ display:flex; align-items:center; gap:6px; font-family:var(--mono); font-size:10.5px;
    color:var(--muted); padding-top:3px; }
  .dot{ width:6px; height:6px; border-radius:50%; background:var(--frost); box-shadow:0 0 6px var(--frost); }
  .dot.stale{ background:var(--amber); box-shadow:0 0 6px var(--amber); animation:pulse 1.6s ease-in-out infinite; }
  @keyframes pulse{ 0%,100%{opacity:1;} 50%{opacity:0.35;} }
  .screen{ background:var(--well); border-radius:9px; padding:22px 18px 18px;
    border:1px solid rgba(0,0,0,0.6);
    box-shadow:inset 0 2px 10px rgba(0,0,0,0.7), inset 0 0 40px rgba(127,224,214,0.03);
    text-align:center; position:relative; overflow:hidden; }
  .screen::before{ content:""; position:absolute; inset:0;
    background:repeating-linear-gradient(180deg, rgba(255,255,255,0.015) 0px, rgba(255,255,255,0.015) 1px, transparent 1px, transparent 3px);
    pointer-events:none; }
  .temp-label{ font-family:var(--mono); font-size:10.5px; color:var(--frost-dim); margin-bottom:6px; }
  .temp-value{ font-family:var(--mono); font-weight:500; font-size:64px; line-height:1; color:var(--frost);
    text-shadow:0 0 18px rgba(127,224,214,0.5), 0 0 2px rgba(127,224,214,0.8); letter-spacing:-1px; }
  .temp-value.warn{ color:var(--amber); text-shadow:0 0 18px rgba(242,169,59,0.5), 0 0 2px rgba(242,169,59,0.8); }
  .temp-value.alert{ color:var(--red); text-shadow:0 0 18px rgba(225,75,75,0.55), 0 0 2px rgba(225,75,75,0.85); }
  .temp-unit{ font-size:24px; vertical-align:top; margin-left:2px; opacity:0.7; }
  .range-note{ font-family:var(--mono); font-size:10px; color:var(--muted); margin-top:8px; }
  .status-row{ display:flex; gap:10px; margin-top:16px; }
  .status-card{ flex:1; background:var(--steel-700); border-radius:9px; padding:12px 12px 11px;
    border:1px solid var(--hairline); }
  .status-card .k{ font-size:10px; color:var(--muted); margin-bottom:6px; }
  .door-state{ display:flex; align-items:center; gap:8px; }
  .contact{ width:11px; height:11px; border-radius:3px; background:var(--frost);
    box-shadow:0 0 8px rgba(127,224,214,0.7); flex-shrink:0; }
  .contact.open{ background:var(--red); box-shadow:0 0 8px rgba(225,75,75,0.75); animation:pulse 1.3s ease-in-out infinite; }
  .door-text{ font-size:15px; font-weight:600; }
  .door-sub{ font-family:var(--mono); font-size:10px; color:var(--muted); margin-top:5px; }
  .updated .v{ font-family:var(--mono); font-size:15px; font-weight:500; }
  .updated .sub{ font-family:var(--mono); font-size:10px; color:var(--muted); margin-top:5px; }
  .trace-wrap{ margin-top:16px; background:var(--steel-700); border-radius:9px; padding:14px 14px 10px;
    border:1px solid var(--hairline); }
  .trace-head{ display:flex; justify-content:space-between; font-size:10px; color:var(--muted);
    margin-bottom:6px; font-family:var(--mono); }
  svg.trace{ width:100%; height:56px; display:block; }
  .trace-line{ fill:none; stroke:var(--frost); stroke-width:1.6; filter:drop-shadow(0 0 3px rgba(127,224,214,0.6)); }
  .trace-fill{ fill:url(#tg); opacity:0.5; }
  .footer{ display:flex; justify-content:space-between; align-items:center; margin-top:16px;
    padding:0 6px; font-family:var(--mono); font-size:9.5px; color:var(--muted); }
  .badge{ display:inline-flex; align-items:center; gap:5px; padding:3px 8px; border-radius:20px;
    background:var(--steel-700); border:1px solid var(--hairline); }
  .empty{ text-align:center; padding:60px 20px; color:var(--muted); font-family:var(--mono); font-size:13px; }
  @media (max-width:360px){ .temp-value{ font-size:52px; } }
</style>
</head>
<body>
  <div class="panel" id="panel">
    <div class="screw tl"></div><div class="screw tr"></div>
    <div class="screw bl"></div><div class="screw br"></div>

    <div class="header">
      <div>
        <div class="device-id">COLDTRACE · UNIT 01</div>
        <div class="device-name" id="deviceName">—</div>
        <div class="location" id="deviceLocation">—</div>
      </div>
      <div class="conn">
        <span class="dot" id="connDot"></span>
        <span id="connText">—</span>
      </div>
    </div>

    <div style="text-align:right; margin:-10px 6px 12px;">
      <a href="/history" style="font-family:var(--mono); font-size:10.5px; color:var(--frost-dim); text-decoration:none;">HISTORY &rarr;</a>
    </div>

    <div id="content">
      <div class="empty">Waiting for first reading&hellip;</div>
    </div>
  </div>

<script>
const REFRESH_MS = 5000;

function fmtAge(sec){
  if (sec < 60) return Math.floor(sec) + "s ago";
  if (sec < 3600) return Math.floor(sec/60) + "m ago";
  return Math.floor(sec/3600) + "h " + Math.floor((sec%3600)/60) + "m ago";
}

function toPoints(vals, w, h, pad){
  const lo = Math.min(...vals), hi = Math.max(...vals);
  const range = (hi - lo) || 1;
  return vals.map((v,i)=>{
    const x = vals.length > 1 ? (i/(vals.length-1)) * w : w/2;
    const y = pad + (1 - (v-lo)/range) * (h - pad*2);
    return x.toFixed(1) + "," + y.toFixed(1);
  }).join(" ");
}

function render(d){
  document.getElementById('deviceName').textContent = d.device_name;
  document.getElementById('deviceLocation').textContent = d.device_location;

  const connDot = document.getElementById('connDot');
  const connText = document.getElementById('connText');
  connDot.classList.toggle('stale', d.is_stale);
  connText.textContent = d.is_stale ? 'STALE' : 'LIVE';

  if (!d.has_data){
    document.getElementById('content').innerHTML = '<div class="empty">Waiting for first reading&hellip;</div>';
    return;
  }

  let tempClass = '';
  if (d.temp > d.safe_high) tempClass = 'alert';
  else if (d.temp > d.warn_at) tempClass = 'warn';

  const doorSubText = d.door
    ? 'currently open'
    : (d.last_open_at ? 'last opened ' + fmtAge((Date.now() - new Date(d.last_open_at)) / 1000) : 'no activity logged');

  const pts = d.history.length ? toPoints(d.history, 300, 56, 6) : '';
  const fillPts = pts ? ('0,56 ' + pts + ' 300,56') : '';

  document.getElementById('content').innerHTML = `
    <div class="screen">
      <div class="temp-label">TEMPERATURE</div>
      <div class="temp-value ${tempClass}">${d.temp.toFixed(1)}<span class="temp-unit">&deg;C</span></div>
      <div class="range-note">SAFE RANGE ${d.safe_low.toFixed(0)} TO ${d.safe_high.toFixed(0)}</div>
    </div>
    <div class="status-row">
      <div class="status-card">
        <div class="k">Door</div>
        <div class="door-state">
          <span class="contact ${d.door ? 'open' : ''}"></span>
          <span class="door-text">${d.door ? 'Open' : 'Closed'}</span>
        </div>
        <div class="door-sub">${doorSubText}</div>
      </div>
      <div class="status-card updated">
        <div class="k">Last reading</div>
        <div class="v">${fmtAge(d.age_seconds)}</div>
        <div class="sub">${new Date(d.received_at).toLocaleTimeString()}</div>
      </div>
    </div>
    <div class="trace-wrap">
      <div class="trace-head"><span>LAST ${d.history.length} READINGS</span><span>${d.history_min.toFixed(1)} to ${d.history_max.toFixed(1)}&deg;C</span></div>
      <svg class="trace" viewBox="0 0 300 56" preserveAspectRatio="none">
        <defs><linearGradient id="tg" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="#7FE0D6" stop-opacity="0.45"/>
          <stop offset="100%" stop-color="#7FE0D6" stop-opacity="0"/>
        </linearGradient></defs>
        <polygon class="trace-fill" points="${fillPts}"></polygon>
        <polyline class="trace-line" points="${pts}"></polyline>
      </svg>
    </div>
    <div class="footer">
      <span>${d.history.length} readings buffered</span>
      <span class="badge"><span class="dot" style="width:5px;height:5px;"></span>${d.is_stale ? 'NO SIGNAL' : 'CONNECTED'}</span>
    </div>
  `;
}

async function poll(){
  try{
    const res = await fetch('/api/data', { cache: 'no-store' });
    const d = await res.json();
    render(d);
  }catch(e){
    document.getElementById('connText').textContent = 'ERROR';
  }
}

poll();
setInterval(poll, REFRESH_MS);
</script>
</body>
</html>
"""


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
