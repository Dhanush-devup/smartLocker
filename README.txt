# Smart Locker live-update code

This bundle contains the updated ESP32 firmware, Flask backend, and frontend JavaScript.
Keep your existing frontend design, but add the clear button snippet and danger-button CSS supplied in frontend/.

## 1. Configure the ESP32 firmware
Open `esp32/SmartLocker.ino` and set these three constants near the top:
- `WIFI_SSID`
- `WIFI_PASSWORD`
- `SERVER_BASE_URL`, using the laptop's current Wi-Fi IPv4 address, for example `http://192.168.1.25:5000`

Do not use `127.0.0.1` or `localhost` for `SERVER_BASE_URL`; those refer to the ESP32 itself, not your laptop.
The laptop and ESP32 should be on the same Wi-Fi. Keep the laptop firewall configured to allow Python/Flask on private networks.

Required Arduino libraries:
- Adafruit ADXL345
- Adafruit Unified Sensor
- ESP32Servo

## 2. Replace the backend
Put `backend/app.py` in the same backend folder as your existing `app.py` (keep the project layout where `backend/` and `frontend/` are sibling folders).
This version adds `/api/stream` using Server-Sent Events (SSE), sends dashboard snapshots to connected browsers when state/events change, keeps `/api/status` as a fallback endpoint, and runs Flask threaded with the debug reloader disabled.

## 3. Replace the frontend JavaScript
Copy `frontend/script.js` over your existing project's `frontend/script.js`.
Keep your current `index.html` and `style.css`.

## 4. Add the dashboard button
1. In `frontend/index.html`, inside the LOCKER card, insert the contents of `clear_alarm_button_snippet.html` immediately after the existing `.card-actions` element.
2. Append `clear_alarm_button.css` to the end of your existing `frontend/style.css` (or copy its rules).
3. The button is hidden unless the ESP32 reports a latched incident. Clicking it asks for confirmation, then queues a clear command until the ESP32 acknowledges receipt.

## 5. Run and test
1. Start Flask using your normal virtual environment command, e.g. `python app.py`.
2. Open the dashboard from the Flask laptop URL, e.g. `http://127.0.0.1:5000` on the laptop.
3. Upload the updated firmware to the ESP32 after setting Wi-Fi credentials and the laptop IP.
4. Open the website and test Lock/Unlock, movement events, and the activity timeline.
5. Flask should show a browser connection to `/api/stream`; the page should reconnect automatically if the stream drops.

## Design notes / limitations
- Sensor detection and local buzzer activation remain local to the ESP32 and do not depend on Flask.
- The network task attempts a pending event before routine command/heartbeat requests. HTTP calls still use their existing timeouts; this improves prioritization but does not make a blocked HTTP request impossible.
- SSE provides near-immediate browser updates after Flask receives a state/event update. It does not guarantee zero latency.
- This remains a local demo prototype. Wi-Fi credentials are placeholders in the firmware; enter them locally and do not share the completed file publicly.
- Alarm clearing can be requested from the dashboard or Serial Monitor `CLEAR`. The dashboard request remains pending until the ESP32 acknowledges receiving it; the website waits for the ESP32 state update before showing the incident as cleared. This is a local-network demo control, not authenticated remote access.
