from flask import Flask, jsonify, request, send_from_directory
from pathlib import Path
import time

# --------------------------------------------------
# PATHS
# --------------------------------------------------

BASE_DIR = Path(__file__).resolve().parent.parent
FRONTEND_DIR = BASE_DIR / "frontend"

app = Flask(__name__, static_folder=str(FRONTEND_DIR))


# --------------------------------------------------
# LOCKER STATE
# --------------------------------------------------

locker_state = {
    "locked": True,
    "movement": "LOW",
    "alarm": "OFF",
    "security": "SECURE",
    "esp32": "ONLINE"
}

desired_locked = True

activity_log = []

last_seen = time.time()


# --------------------------------------------------
# FRONTEND
# --------------------------------------------------

@app.route("/")
def home():
    return send_from_directory(FRONTEND_DIR, "index.html")


@app.route("/<path:filename>")
def frontend_files(filename):
    return send_from_directory(FRONTEND_DIR, filename)


# --------------------------------------------------
# STATUS
# --------------------------------------------------

@app.route("/api/status", methods=["GET"])
def get_status():

    # ESP32 offline if no heartbeat for 8 seconds
    if time.time() - last_seen > 8:
        locker_state["esp32"] = "OFFLINE"

    return jsonify({
        **locker_state,
        "activity": activity_log
    })


# --------------------------------------------------
# LOCK / UNLOCK COMMANDS
# --------------------------------------------------

@app.route("/api/lock", methods=["POST"])
def lock():

    global desired_locked

    desired_locked = True

    return jsonify({
        "success": True,
        "message": "Lock command sent"
    })


@app.route("/api/unlock", methods=["POST"])
def unlock():

    global desired_locked

    desired_locked = False

    return jsonify({
        "success": True,
        "message": "Unlock command sent"
    })


# --------------------------------------------------
# ESP32 COMMAND POLLING
# --------------------------------------------------

@app.route("/api/command", methods=["GET"])
def get_command():

    if desired_locked:
        command = "LOCK"
    else:
        command = "UNLOCK"

    return jsonify({
        "command": command
    })


# --------------------------------------------------
# ESP32 EVENTS
# --------------------------------------------------

@app.route("/api/event", methods=["POST"])
def event():

    global last_seen

    data = request.get_json()

    last_seen = time.time()
    locker_state["esp32"] = "ONLINE"

    if data:

        # Update known state
        for key in [
            "locked",
            "movement",
            "alarm",
            "security"
        ]:
            if key in data:
                locker_state[key] = data[key]

        # Add event to activity log
        event_type = data.get("event")

        if event_type:
            activity_log.insert(0, {
                "event": event_type,
                "time": time.strftime("%H:%M:%S")
            })

            # Keep only recent events
            if len(activity_log) > 50:
                activity_log.pop()

    return jsonify({
        "success": True
    })


# --------------------------------------------------
# HEARTBEAT
# --------------------------------------------------

@app.route("/api/heartbeat", methods=["POST"])
def heartbeat():

    global last_seen

    last_seen = time.time()
    locker_state["esp32"] = "ONLINE"

    return jsonify({
        "success": True
    })


# --------------------------------------------------
# RUN SERVER
# --------------------------------------------------

if __name__ == "__main__":
    app.run(
        host="0.0.0.0",
        port=5000,
        debug=True
    )