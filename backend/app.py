
from flask import Flask, Response, jsonify, redirect, request, send_from_directory, session, stream_with_context
from werkzeug.security import check_password_hash
from functools import wraps
from pathlib import Path
from datetime import datetime
from zoneinfo import ZoneInfo
from queue import Queue, Empty
from threading import Lock
import hmac
import json
import os
import secrets
import time

BASE_DIR = Path(__file__).resolve().parent.parent
BACKEND_DIR = Path(__file__).resolve().parent
FRONTEND_DIR = BASE_DIR / "frontend"
USERS_FILE = BACKEND_DIR / "users.json"
LOGIN_HISTORY_FILE = BACKEND_DIR / "login_history.txt"
SECRETS_FILE = BACKEND_DIR / "local_secrets.json"


def load_local_secrets():
    """Environment variables win; otherwise read the gitignored local_secrets.json."""
    stored = {}
    if SECRETS_FILE.exists():
        stored = json.loads(SECRETS_FILE.read_text(encoding="utf-8"))
    return (
        os.environ.get("FLASK_SECRET_KEY") or stored.get("flask_secret_key") or secrets.token_hex(32),
        os.environ.get("DEVICE_API_KEY") or stored.get("device_api_key"),
    )


FLASK_SECRET_KEY, DEVICE_API_KEY = load_local_secrets()
if not DEVICE_API_KEY:
    print("WARNING: no DEVICE_API_KEY configured; ESP32 endpoints are unauthenticated.")

app = Flask(__name__, static_folder=str(FRONTEND_DIR))
app.secret_key = FLASK_SECRET_KEY
app.config.update(SESSION_COOKIE_HTTPONLY=True, SESSION_COOKIE_SAMESITE="Lax")

# Role permissions. Unlocking during a latched alarm is owner-only.
CAN_CLEAR_ALARM = ("owner", "admin")
CAN_UNLOCK_DURING_ALARM = ("owner",)

state_lock = Lock()

locker_state = {
    "locked": True,
    "movement": "UNKNOWN",
    "alarm": False,
    "security": "STARTING",
    "esp32": "OFFLINE",
    "ir": "UNKNOWN",
    "sensor_fault": None,
}

desired_locked = True
last_seen = 0.0
activity_log = []
seen_event_ids = set()

# Remote clear command lifecycle.
next_command_id = 1
pending_clear_command = None
last_clear_result = None

subscribers = set()
subscribers_lock = Lock()
SSE_QUEUE_SIZE = 10

SECURITY_EVENT_PREFIXES = (
    "TAMPER", "ALARM", "ARMED", "REARM", "STABLE",
    "SUSPECT", "SECURITY", "SENSOR_BASELINE", "OBJECT"
)
ACCESS_EVENT_PREFIXES = ("LOCK", "UNLOCK", "ACCESS", "AUTH", "CLEAR")


def local_time():
    return datetime.now(ZoneInfo("Asia/Kolkata")).strftime("%H:%M:%S")


def event_category(event_type):
    name = str(event_type or "SYSTEM_EVENT").upper()
    if any(name.startswith(p) for p in SECURITY_EVENT_PREFIXES):
        return "SECURITY"
    if any(name.startswith(p) for p in ACCESS_EVENT_PREFIXES):
        return "ACCESS"
    return "HARDWARE"


def status_payload():
    with state_lock:
        if not last_seen or time.time() - last_seen > 8:
            locker_state["esp32"] = "OFFLINE"
        return {
            **locker_state,
            "activity": list(activity_log),
            "last_seen": last_seen or None,
            "clear_command": (
                {
                    "command_id": pending_clear_command["command_id"],
                    "status": "PENDING",
                }
                if pending_clear_command
                else last_clear_result
            ),
        }


def broadcast_update():
    payload = json.dumps(status_payload(), separators=(",", ":"))
    with subscribers_lock:
        clients = list(subscribers)

    for client in clients:
        try:
            client.put_nowait(payload)
        except Exception:
            try:
                client.get_nowait()
            except Empty:
                pass
            try:
                client.put_nowait(payload)
            except Exception:
                pass


def update_state_from_payload(data):
    with state_lock:
        for key in ("locked", "movement", "security", "ir", "sensor_fault"):
            if key in data:
                locker_state[key] = data[key]
        if "alarm" in data:
            locker_state["alarm"] = data["alarm"]


def add_activity(event_type, message, event_id=None, source="ESP32"):
    if not event_type:
        return False

    event_type = str(event_type)[:80]
    with state_lock:
        if event_id is not None:
            key = str(event_id)
            if key in seen_event_ids:
                return False
            seen_event_ids.add(key)
            if len(seen_event_ids) > 500:
                seen_event_ids.clear()
                seen_event_ids.add(key)

        activity_log.insert(0, {
            "event_id": str(event_id) if event_id is not None else None,
            "type": event_type,
            "category": event_category(event_type),
            "message": str(message or event_type.replace("_", " "))[:300],
            "time": local_time(),
            "source": source,
        })
        del activity_log[100:]
    return True


def load_users():
    if USERS_FILE.exists():
        return json.loads(USERS_FILE.read_text(encoding="utf-8"))
    return {}


def current_user():
    """Return (username, role) for the logged-in user, or None."""
    username = session.get("username")
    if not username:
        return None
    user = load_users().get(username)
    if user is None:
        # Account was removed while the session was still open.
        session.clear()
        return None
    return username, user["role"]


def login_required(view):
    @wraps(view)
    def wrapper(*args, **kwargs):
        if current_user() is None:
            return jsonify({"success": False, "message": "Please log in."}), 401
        return view(*args, **kwargs)
    return wrapper


def device_key_required(view):
    @wraps(view)
    def wrapper(*args, **kwargs):
        if DEVICE_API_KEY:
            supplied = request.headers.get("X-Device-Key", "")
            if not hmac.compare_digest(supplied, DEVICE_API_KEY):
                return jsonify({"success": False, "message": "Invalid device key."}), 401
        return view(*args, **kwargs)
    return wrapper


login_history_lock = Lock()


def record_login_history(action, username, role="-"):
    """Append one line per login/logout/failure; kept across backend restarts."""
    stamp = datetime.now(ZoneInfo("Asia/Kolkata")).strftime("%Y-%m-%d %H:%M:%S")
    line = f"{stamp} | {action:<12} | {username[:40]:<20} | {role:<6} | {request.remote_addr}\n"
    with login_history_lock:
        new_file = not LOGIN_HISTORY_FILE.exists()
        with LOGIN_HISTORY_FILE.open("a", encoding="utf-8") as history:
            if new_file:
                history.write("TIME (IST)          | ACTION       | USERNAME             | ROLE   | IP ADDRESS\n")
            history.write(line)


def actor_label():
    username, role = current_user()
    return f"{username} ({role})"


@app.route("/")
def home():
    if current_user() is None:
        return redirect("/login.html")
    return send_from_directory(FRONTEND_DIR, "index.html")


@app.route("/<path:filename>")
def frontend_files(filename):
    if filename == "index.html":
        return redirect("/")
    return send_from_directory(FRONTEND_DIR, filename)


@app.route("/api/login", methods=["POST"])
def login():
    data = request.get_json(silent=True) or {}
    username = str(data.get("username", "")).strip().lower()
    password = str(data.get("password", ""))
    user = load_users().get(username)

    if user is None or not check_password_hash(user["password_hash"], password):
        add_activity("AUTH_LOGIN_FAILED", f"Failed login attempt for '{username[:40]}'", source="DASHBOARD")
        record_login_history("LOGIN_FAILED", username)
        broadcast_update()
        return jsonify({"success": False, "message": "Invalid username or password."}), 401

    session.clear()
    session["username"] = username
    add_activity("AUTH_LOGIN", f"{username} ({user['role']}) logged in", source="DASHBOARD")
    record_login_history("LOGIN", username, user["role"])
    broadcast_update()
    return jsonify({"success": True, "username": username, "role": user["role"]})


@app.route("/api/logout", methods=["POST"])
def logout():
    user = current_user()
    session.clear()
    if user:
        add_activity("AUTH_LOGOUT", f"{user[0]} ({user[1]}) logged out", source="DASHBOARD")
        record_login_history("LOGOUT", user[0], user[1])
        broadcast_update()
    return jsonify({"success": True})


@app.route("/api/me", methods=["GET"])
@login_required
def me():
    username, role = current_user()
    return jsonify({
        "username": username,
        "role": role,
        "can_clear_alarm": role in CAN_CLEAR_ALARM,
        "can_unlock_during_alarm": role in CAN_UNLOCK_DURING_ALARM,
    })


@app.route("/api/status", methods=["GET"])
@login_required
def get_status():
    return jsonify(status_payload())


@app.route("/api/stream", methods=["GET"])
@login_required
def stream_status():
    client_queue = Queue(maxsize=SSE_QUEUE_SIZE)
    with subscribers_lock:
        subscribers.add(client_queue)

    def generate():
        try:
            yield "data: " + json.dumps(
                status_payload(), separators=(",", ":")
            ) + "\n\n"
            while True:
                try:
                    yield "data: " + client_queue.get(timeout=15) + "\n\n"
                except Empty:
                    yield ": keep-alive\n\n"
        finally:
            with subscribers_lock:
                subscribers.discard(client_queue)

    response = Response(
        stream_with_context(generate()),
        mimetype="text/event-stream",
    )
    response.headers["Cache-Control"] = "no-cache"
    response.headers["X-Accel-Buffering"] = "no"
    return response


@app.route("/api/lock", methods=["POST"])
@login_required
def lock():
    global desired_locked
    with state_lock:
        desired_locked = True
    add_activity("LOCK_REQUESTED", f"Lock requested by {actor_label()}", source="DASHBOARD")
    broadcast_update()
    return jsonify({
        "success": True,
        "message": "Lock command queued; waiting for ESP32 confirmation.",
    })


@app.route("/api/unlock", methods=["POST"])
@login_required
def unlock():
    global desired_locked
    _, role = current_user()
    with state_lock:
        alarm_active = locker_state.get("alarm") is True
        if alarm_active and role not in CAN_UNLOCK_DURING_ALARM:
            denied = True
        else:
            denied = False
            desired_locked = False

    if denied:
        add_activity("UNLOCK_DENIED", f"Unlock during active alarm denied for {actor_label()}", source="DASHBOARD")
        broadcast_update()
        return jsonify({
            "success": False,
            "message": "Only the owner can unlock while a tamper alarm is active.",
        }), 403

    add_activity("UNLOCK_REQUESTED", f"Unlock requested by {actor_label()}", source="DASHBOARD")
    broadcast_update()
    return jsonify({
        "success": True,
        "message": "Unlock command queued; waiting for ESP32 confirmation.",
    })


@app.route("/api/clear-alarm", methods=["POST"])
@login_required
def request_clear_alarm():
    global pending_clear_command, next_command_id, last_clear_result

    _, role = current_user()
    if role not in CAN_CLEAR_ALARM:
        add_activity("CLEAR_ALARM_DENIED", f"Alarm clear denied for {actor_label()}", source="DASHBOARD")
        broadcast_update()
        return jsonify({
            "success": False,
            "message": "Only an owner or admin can clear a tamper alarm.",
        }), 403

    requested_by = actor_label()
    with state_lock:
        if pending_clear_command is not None:
            return jsonify({
                "success": True,
                "message": "A clear request is already pending.",
                "command_id": pending_clear_command["command_id"],
            })

        command_id = next_command_id
        next_command_id += 1
        pending_clear_command = {
            "command_id": command_id,
            "created_at": time.time(),
            "requested_by": requested_by,
        }
        last_clear_result = {
            "command_id": command_id,
            "status": "PENDING",
        }

    add_activity("CLEAR_ALARM_REQUESTED", f"Alarm clear requested by {requested_by}", source="DASHBOARD")
    broadcast_update()
    return jsonify({
        "success": True,
        "message": "Clear request queued; waiting for ESP32 execution.",
        "command_id": command_id,
    }), 202


@app.route("/api/command", methods=["GET"])
@device_key_required
def get_command():
    with state_lock:
        if pending_clear_command is not None:
            return jsonify({
                "command": "CLEAR_ALARM",
                "command_id": pending_clear_command["command_id"],
            })

        return jsonify({
            "command": "LOCK" if desired_locked else "UNLOCK"
        })


@app.route("/api/command-ack", methods=["POST"])
@device_key_required
def acknowledge_command():
    global pending_clear_command, last_clear_result

    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        return jsonify({
            "success": False,
            "message": "Expected a JSON object.",
        }), 400

    command_id = data.get("command_id")
    executed = data.get("executed") is True
    success = data.get("success") is True

    with state_lock:
        pending = pending_clear_command

        if pending is None or command_id != pending["command_id"]:
            return jsonify({
                "success": False,
                "message": "No matching pending command.",
            }), 409

        # Receiving or queueing a command is not execution.
        if not executed:
            return jsonify({
                "success": False,
                "message": "Execution confirmation is required.",
            }), 400

        requested_by = pending.get("requested_by", "unknown user")
        pending_clear_command = None
        last_clear_result = {
            "command_id": command_id,
            "status": "CLEARED" if success else "FAILED",
        }

    if success:
        add_activity(
            "CLEAR_ALARM",
            f"ESP32 confirmed the alarm clear requested by {requested_by}.",
            event_id=f"clear-command-{command_id}",
            source="ESP32",
        )

    broadcast_update()
    return jsonify({
        "success": True,
        "executed": True,
        "status": last_clear_result["status"],
    })


@app.route("/api/event", methods=["POST"])
@device_key_required
def event():
    global last_seen

    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        return jsonify({
            "success": False,
            "message": "Expected a JSON object.",
        }), 400

    with state_lock:
        last_seen = time.time()
        locker_state["esp32"] = "ONLINE"

    update_state_from_payload(data)

    event_type = data.get("type") or data.get("event")
    if event_type:
        add_activity(
            event_type,
            data.get("message"),
            data.get("event_id"),
            "ESP32",
        )

    broadcast_update()
    return jsonify({"success": True})


@app.route("/api/heartbeat", methods=["POST"])
@device_key_required
def heartbeat():
    global last_seen

    data = request.get_json(silent=True)
    if data is None:
        data = {}
    if not isinstance(data, dict):
        return jsonify({
            "success": False,
            "message": "Expected a JSON object.",
        }), 400

    previous = status_payload()

    with state_lock:
        last_seen = time.time()
        locker_state["esp32"] = "ONLINE"

    update_state_from_payload(data)

    current = status_payload()
    visible_keys = (
        "locked", "movement", "alarm", "security", "esp32",
        "ir", "sensor_fault"
    )
    if any(previous.get(k) != current.get(k) for k in visible_keys):
        broadcast_update()

    return jsonify({"success": True})


if __name__ == "__main__":
    app.run(
        host="0.0.0.0",
        port=5000,
        debug=False,
        threaded=True,
        use_reloader=False,
    )
