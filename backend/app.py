
from flask import Flask, Response, jsonify, request, send_from_directory, stream_with_context
from pathlib import Path
from datetime import datetime
from zoneinfo import ZoneInfo
from queue import Queue, Empty
from threading import Lock
import json
import time

BASE_DIR = Path(__file__).resolve().parent.parent
FRONTEND_DIR = BASE_DIR / "frontend"
app = Flask(__name__, static_folder=str(FRONTEND_DIR))

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


@app.route("/")
def home():
    return send_from_directory(FRONTEND_DIR, "index.html")


@app.route("/<path:filename>")
def frontend_files(filename):
    return send_from_directory(FRONTEND_DIR, filename)


@app.route("/api/status", methods=["GET"])
def get_status():
    return jsonify(status_payload())


@app.route("/api/stream", methods=["GET"])
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
def lock():
    global desired_locked
    with state_lock:
        desired_locked = True
    broadcast_update()
    return jsonify({
        "success": True,
        "message": "Lock command queued; waiting for ESP32 confirmation.",
    })


@app.route("/api/unlock", methods=["POST"])
def unlock():
    global desired_locked
    with state_lock:
        desired_locked = False
    broadcast_update()
    return jsonify({
        "success": True,
        "message": "Unlock command queued; waiting for ESP32 confirmation.",
    })


@app.route("/api/clear-alarm", methods=["POST"])
def request_clear_alarm():
    global pending_clear_command, next_command_id, last_clear_result

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
        }
        last_clear_result = {
            "command_id": command_id,
            "status": "PENDING",
        }

    broadcast_update()
    return jsonify({
        "success": True,
        "message": "Clear request queued; waiting for ESP32 execution.",
        "command_id": command_id,
    }), 202


@app.route("/api/command", methods=["GET"])
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

        pending_clear_command = None
        last_clear_result = {
            "command_id": command_id,
            "status": "CLEARED" if success else "FAILED",
        }

    if success:
        add_activity(
            "CLEAR_ALARM",
            "ESP32 confirmed execution of the alarm-clear command.",
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
