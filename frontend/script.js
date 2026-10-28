const API = "/api";
let activeFilter = "ALL";
let latestEvents = [];
let previousLatestEventKey = null;
let lastUpdatedAt = null;
let liveStream = null;
let statusRequestInFlight = false;
let currentUser = null;
let lastDashboardData = null;

const el = id => document.getElementById(id);
const securityCard = el("security-card");
const securityIcon = el("security-icon");
const securityStatus = el("security-status");
const securityMessage = el("security-message");
const securityPill = el("security-pill");
const incidentStatus = el("incident-status");
const lastUpdated = el("last-updated");
const lockStatus = el("lock-status");
const lockMessage = el("lock-message");
const unlockButton = el("unlock-btn");
const lockButton = el("lock-btn");
const clearAlarmButton = el("clear-alarm-btn");
const commandFeedback = el("command-feedback");
const clearAlarmContainer = el("clear-alarm-container");
const clearAlarmWarning = clearAlarmContainer.querySelector(".clear-alarm-warning");
const userChip = el("user-chip");
const userName = el("user-name");
const userRole = el("user-role");
const logoutButton = el("logout-btn");
const alarmCard = el("alarm-card");
const alarmStatus = el("alarm-status");
const alarmMessage = el("alarm-message");
const alarmIncidentNote = el("alarm-incident-note");
const movementCard = el("movement-card");
const movementStatus = el("movement-status");
const movementMessage = el("movement-message");
const contentsCard = el("contents-card");
const contentsIcon = el("contents-icon");
const contentsStatus = el("contents-status");
const contentsMessage = el("contents-message");
const espStatus = el("esp-status");
const sensorEsp32 = el("sensor-esp32");
const sensorAdxl = el("sensor-adxl");
const sensorIr = el("sensor-ir");
const irDescription = el("ir-description");
const sensorFault = el("sensor-fault");
const activityLog = el("activity-log");

const SECURITY_STATES = {
  ARMED: { label: "ARMED", icon: "◈", className: "state-armed", pill: "MONITORING", message: "The locker is locked and monitoring for suspicious movement." },
  SUSPECTED: { label: "SUSPECTED", icon: "↗", className: "state-suspected", pill: "VERIFYING MOVEMENT", message: "Movement has been detected; the system is checking for confirmation." },
  TAMPERING: { label: "TAMPERING DETECTED", icon: "⚠", className: "state-tampering", pill: "THREAT CONFIRMED", message: "Suspicious movement has been confirmed while the locker is secured." },
  ALARM_LATCHED: { label: "ALARM LATCHED", icon: "!", className: "state-alarm-latched", pill: "INCIDENT ACTIVE", message: "A security incident remains active until it is explicitly cleared." },
  REARMING: { label: "REARMING", icon: "↻", className: "state-rearming", pill: "STABILIZING", message: "Waiting for stable sensor readings before monitoring resumes." },
  AUTHORIZED_ACCESS: { label: "AUTHORIZED ACCESS", icon: "↗", className: "state-access", pill: "ACCESS MODE", message: "Movement monitoring is suppressed during the configured access period." },
  STARTING: { label: "STARTING", icon: "…", className: "state-unknown", pill: "INITIALIZING", message: "The controller is initializing its sensors and security state." },
  SENSOR_FAULT: { label: "SENSOR FAULT", icon: "!", className: "state-fault", pill: "CHECK SENSOR", message: "A sensor fault was reported. Movement decisions may be paused." },
  UNKNOWN: { label: "STATUS UNKNOWN", icon: "?", className: "state-unknown", pill: "WAITING FOR DATA", message: "No recognized security state has been received yet." }
};

function normalizeSecurity(raw, movement, alarm, locked) {
  const value = String(raw ?? "").trim().toUpperCase();
  const move = String(movement ?? "").trim().toUpperCase();
  const alarmOn = alarm === true || ["ON", "TRUE", "1"].includes(String(alarm).toUpperCase());
  if (alarmOn || ["ALARM_LATCHED", "SEC_ALARM_LATCHED", "ALARM", "LATCHED"].includes(value)) return "ALARM_LATCHED";
  if (["SENSOR_FAULT", "SEC_SENSOR_FAULT", "FAULT", "ERROR"].includes(value)) return "SENSOR_FAULT";
  if (["TAMPERING", "MOVE_TAMPERING", "SEC_TAMPERING"].includes(value) || move.includes("TAMPERING")) return "TAMPERING";
  if (["SUSPECTED", "MOVE_SUSPECTED", "SUSPECT", "WAITING", "WAITING_FOR_STABILITY", "CONFIRMING"].includes(value) || ["SUSPECTED", "WAITING"].includes(move)) return "SUSPECTED";
  if (["REARMING", "SEC_REARMING", "WAITING_FOR_STABILITY", "STABILIZING"].includes(value)) return "REARMING";
  if (["AUTHORIZED_ACCESS", "SEC_AUTHORIZED_ACCESS", "UNLOCKED", "ACCESS"].includes(value) || locked === false) return "AUTHORIZED_ACCESS";
  if (["ARMED", "SEC_ARMED", "SECURE", "NORMAL", "LOW"].includes(value)) return "ARMED";
  if (["STARTING", "SEC_STARTING", "BOOTING"].includes(value)) return "STARTING";
  return "UNKNOWN";
}

function isAlarmOn(value) {
  return value === true || ["ON", "TRUE", "1"].includes(String(value).toUpperCase());
}
function setBadge(node, label, state) {
  node.textContent = label;
  node.className = `badge ${state || "neutral"}`;
}
function updateSecurity(rawSecurity, movement, alarm, locked) {
  const key = normalizeSecurity(rawSecurity, movement, alarm, locked);
  const state = SECURITY_STATES[key];
  securityCard.className = `security-panel ${state.className}`;
  securityIcon.textContent = state.icon;
  securityStatus.textContent = state.label;
  securityMessage.textContent = state.message;
  securityPill.textContent = state.pill;
  const incident = key === "ALARM_LATCHED" || key === "TAMPERING" || isAlarmOn(alarm);
  incidentStatus.innerHTML = `<span class="status-dot"></span>${incident ? "Security incident active" : key === "UNKNOWN" || key === "STARTING" ? "Incident status unknown" : "No active security incident"}`;
  alarmIncidentNote.textContent = incident ? "Incident remains latched independently of the buzzer output." : "No latched incident reported by the controller.";
}
function updateLock(locked) {
  if (locked === true || String(locked).toLowerCase() === "true" || locked === 1) {
    lockStatus.textContent = "LOCKED"; lockMessage.textContent = "Lock command state: locked";
    lockButton.disabled = true; unlockButton.disabled = false;
  } else if (locked === false || String(locked).toLowerCase() === "false" || locked === 0) {
    lockStatus.textContent = "UNLOCKED"; lockMessage.textContent = "Access mode reported by controller";
    lockButton.disabled = false; unlockButton.disabled = true;
  } else {
    lockStatus.textContent = "UNKNOWN"; lockMessage.textContent = "Waiting for controller status";
    lockButton.disabled = false; unlockButton.disabled = false;
  }
}
function updateAlarm(alarm, security) {
  const latched = isAlarmOn(alarm) || normalizeSecurity(security, null, alarm, null) === "ALARM_LATCHED";
  const known = alarm !== null && alarm !== undefined && String(alarm).toUpperCase() !== "UNKNOWN";
  alarmCard.classList.toggle("is-alarming", latched);
  alarmStatus.textContent = latched ? "INCIDENT LATCHED" : known ? "NO ACTIVE INCIDENT" : "UNKNOWN";
  alarmMessage.textContent = latched ? "Incident remains active until explicitly cleared" : known ? "No latched incident reported by controller" : "Waiting for incident status";
  alarmIncidentNote.textContent = "Buzzer output is not reported separately by the current firmware.";
  const canClear = Boolean(currentUser?.can_clear_alarm);
  clearAlarmContainer.hidden = !latched;
  clearAlarmButton.hidden = !canClear;
  clearAlarmWarning.textContent = canClear
    ? "A security incident is latched."
    : "A security incident is latched. An owner or admin must clear it.";
  // Unlocking during an active alarm is owner-only; the backend enforces this too.
  if (latched && !currentUser?.can_unlock_during_alarm) {
    unlockButton.disabled = true;
    lockMessage.textContent = "Unlock is restricted to the owner during an alarm";
  }
}
function updateMovement(movement) {
  const value = String(movement ?? "").toUpperCase();
  if (value.includes("TAMPERING")) { movementStatus.textContent = "TAMPERING"; movementMessage.textContent = "Confirmed movement event"; movementCard.classList.add("movement-danger"); }
  else if (value.includes("SUSPECT") || value.includes("WAITING")) { movementStatus.textContent = "SUSPECTED"; movementMessage.textContent = "Movement is awaiting confirmation"; movementCard.classList.remove("movement-danger"); }
  else if (value && !["UNKNOWN", "NULL"].includes(value)) { movementStatus.textContent = value === "MOVE_NORMAL" ? "NORMAL" : value; movementMessage.textContent = value.includes("NORMAL") || value === "LOW" ? "No confirmed suspicious movement" : "Latest movement state reported by controller"; movementCard.classList.remove("movement-danger"); }
  else { movementStatus.textContent = "UNKNOWN"; movementMessage.textContent = "Waiting for sensor state"; movementCard.classList.remove("movement-danger"); }
}
function updateContents(ir, alarm, events) {
  const value = String(ir ?? "").toUpperCase();
  // A latched incident produces exactly one TAMPER_* event, so the latest one is its cause.
  const latestTamper = (events || []).find(event => String(event.type || "").toUpperCase().startsWith("TAMPER"));
  const cause = isAlarmOn(alarm) && latestTamper ? String(latestTamper.type).toUpperCase() : "";
  let status, message, icon, tone;
  if (cause === "TAMPER_OBJECT_REMOVED") {
    status = "REMOVED WHILE LOCKED"; message = "Item was taken out while the locker was secured"; icon = "!"; tone = "red";
  } else if (cause === "TAMPER_OBJECT_INSERTED") {
    status = "CHANGED WHILE LOCKED"; message = "An item appeared while the locker was secured"; icon = "!"; tone = "red";
  } else if (value === "PRESENT") {
    status = "ITEM PRESENT"; message = "An item is detected inside the locker"; icon = "▣"; tone = "green";
  } else if (value === "ABSENT") {
    status = "EMPTY"; message = "No item detected inside the locker"; icon = "□"; tone = "muted";
  } else {
    status = "UNKNOWN"; message = "Waiting for IR sensor state"; icon = "?"; tone = "muted";
  }
  contentsStatus.textContent = status;
  contentsMessage.textContent = message;
  contentsIcon.textContent = icon;
  contentsIcon.className = `card-icon ${tone}`;
  contentsCard.classList.toggle("is-alarming", tone === "red");
}
function updateESP32(status) {
  const online = String(status ?? "").toUpperCase() === "ONLINE";
  espStatus.className = `connection ${online ? "online" : "offline"}`;
  espStatus.innerHTML = `<span class="status-dot"></span>${online ? "ESP32 ONLINE" : "ESP32 OFFLINE"}`;
  setBadge(sensorEsp32, online ? "ONLINE" : "OFFLINE", online ? "online" : "offline");
}
function updateSensors(data) {
  if (data.sensor_fault === true || data.sensor_fault === "true") {
    setBadge(sensorAdxl, "FAULT", "offline"); sensorFault.hidden = false; sensorFault.textContent = "ADXL345 sensor fault reported. Movement decisions may be paused.";
  } else if (data.sensor_fault === false || data.sensor_fault === "false") {
    setBadge(sensorAdxl, "READING", "online"); sensorFault.hidden = true;
  } else {
    setBadge(sensorAdxl, "NOT REPORTED", "neutral"); sensorFault.hidden = true;
  }
  const ir = String(data.ir ?? "").toUpperCase();
  if (ir && !["UNKNOWN", "IR_UNKNOWN", "NULL"].includes(ir)) {
    setBadge(sensorIr, ir.replaceAll("_", " "), ir.includes("FAULT") ? "offline" : "online");
    irDescription.textContent = ir.includes("DETECTED") || ir === "OBJECT" || ir === "PRESENT" ? "Object detected" : ir.includes("CLEAR") || ir === "NO_OBJECT" || ir === "ABSENT" ? "No object detected" : "Latest IR state reported by controller";
  } else { setBadge(sensorIr, "UNKNOWN", "neutral"); irDescription.textContent = "Object state not reported"; }
}
function categoryForEvent(event) {
  const type = String(event.type || event.event || "EVENT").toUpperCase();
  if (/TAMPER|ALARM|ARMED|REARM|STABLE|SENSOR_BASELINE|SECURITY|SUSPECT|OBJECT/.test(type)) return "SECURITY";
  if (/LOCK|UNLOCK|ACCESS|AUTH|CLEAR/.test(type)) return "ACCESS";
  return "HARDWARE";
}
function iconForEvent(type, category) {
  if (/TAMPER|ALARM|FAULT|SUSPICIOUS/.test(type)) return "!";
  if (/UNLOCK|ACCESS/.test(type)) return "↗";
  if (/LOCK/.test(type)) return "⌑";
  if (/IR/.test(type)) return "IR";
  if (/ONLINE|RECOVER/.test(type)) return "✓";
  if (/OFFLINE/.test(type)) return "×";
  return category === "SECURITY" ? "◈" : category === "ACCESS" ? "⌑" : "•";
}
function safeText(value) { return String(value ?? ""); }
function eventKey(event) { return String(event.event_id ?? `${event.time ?? ""}|${event.type ?? event.event ?? ""}|${event.message ?? ""}`); }
function renderActivity() {
  const events = latestEvents.filter(event => activeFilter === "ALL" || (event.category || categoryForEvent(event)) === activeFilter);
  if (!events.length) { activityLog.innerHTML = `<div class="empty-state">${latestEvents.length ? "No events in this category." : "Waiting for locker events…"}</div>`; return; }
  activityLog.replaceChildren();
  for (const event of events) {
    const type = safeText(event.type || event.event || "SYSTEM_EVENT").toUpperCase();
    const category = event.category || categoryForEvent(event);
    const item = document.createElement("div"); item.className = "activity-item";
    const icon = document.createElement("div"); icon.className = `event-icon ${category.toLowerCase()}`; icon.textContent = iconForEvent(type, category); icon.setAttribute("aria-hidden", "true");
    const copy = document.createElement("div"); copy.className = "event-copy";
    const title = document.createElement("strong"); title.textContent = type.replaceAll("_", " ");
    const message = document.createElement("p"); message.textContent = safeText(event.message || type.replaceAll("_", " "));
    const cat = document.createElement("span"); cat.className = "event-category"; cat.textContent = category;
    copy.append(title, message, cat);
    const time = document.createElement("time"); time.className = "event-time"; time.textContent = safeText(event.time || "");
    item.append(icon, copy, time); activityLog.append(item);
  }
}
function maybeNotify(events) {
  if (!Array.isArray(events) || !events.length || !("Notification" in window) || Notification.permission !== "granted") return;
  const event = events[0]; const type = String(event.type || event.event || "").toUpperCase();
  if (!/TAMPER|ALARM_LATCHED|IR_SUSPICIOUS_CHANGE/.test(type)) return;
  const key = eventKey(event); if (key === previousLatestEventKey) return; previousLatestEventKey = key;
  try { new Notification("Smart Locker security alert", { body: event.message || type.replaceAll("_", " ") }); } catch (error) { console.warn("Browser notification could not be shown", error); }
}

function redirectToLogin() {
  window.location.href = "/login.html";
}

async function loadCurrentUser() {
  const response = await fetch(`${API}/me`, { cache: "no-store" });
  if (response.status === 401) { redirectToLogin(); return false; }
  currentUser = await response.json();
  userName.textContent = currentUser.username;
  userRole.textContent = currentUser.role.toUpperCase();
  userChip.hidden = false;
  return true;
}

logoutButton.addEventListener("click", async () => {
  try { await fetch(`${API}/logout`, { method: "POST" }); } finally { redirectToLogin(); }
});

function applyDashboardData(data) {
  lastDashboardData = data;
  updateESP32(data.esp32);
  updateLock(data.locked);
  updateMovement(data.movement);
  updateAlarm(data.alarm, data.security);
  updateSecurity(data.security, data.movement, data.alarm, data.locked);
  updateSensors(data);
  latestEvents = Array.isArray(data.activity) ? data.activity : [];
  updateContents(data.ir, data.alarm, latestEvents);
  maybeNotify(latestEvents);
  renderActivity();
  lastUpdatedAt = new Date();
  lastUpdated.textContent = `Updated ${lastUpdatedAt.toLocaleTimeString()}`;
}

async function updateDashboard() {
  if (statusRequestInFlight) return;
  statusRequestInFlight = true;
  try {
    const response = await fetch(`${API}/status`, { cache: "no-store" });
    if (response.status === 401) { redirectToLogin(); return; }
    if (!response.ok) throw new Error(`Status request failed (${response.status})`);
    const data = await response.json();
    applyDashboardData(data);
  } catch (error) {
    console.error("Dashboard update failed:", error);
    updateESP32("OFFLINE");
    securityCard.className = "security-panel state-unknown";
    securityIcon.textContent = "?"; securityStatus.textContent = "BACKEND UNAVAILABLE";
    securityMessage.textContent = "Cannot reach the local backend. Check that Flask is running.";
    securityPill.textContent = "NO CONNECTION";
    incidentStatus.innerHTML = `<span class="status-dot"></span>Incident status unavailable`;
    lastUpdated.textContent = "Update failed";
  } finally {
    statusRequestInFlight = false;
  }
}

async function sendCommand(action, button) {
  button.disabled = true; commandFeedback.className = "feedback"; commandFeedback.textContent = `Sending ${action.toLowerCase()} command…`;
  try {
    const response = await fetch(`${API}/${action.toLowerCase()}`, { method: "POST" });
    if (response.status === 401) { redirectToLogin(); return; }
    const result = await response.json().catch(() => ({}));
    if (!response.ok || result.success === false) throw new Error(result.message || "Command request failed");
    commandFeedback.className = "feedback success"; commandFeedback.textContent = `${action} command accepted by backend; waiting for controller status.`;
    // A live SSE update will normally update the dashboard; fetch once for confirmation.
    await updateDashboard();
  } catch (error) {
    commandFeedback.className = "feedback error"; commandFeedback.textContent = `Could not send command: ${error.message}`;
  } finally {
    button.disabled = false;
    // Re-apply role and lock-state rules to the buttons.
    if (lastDashboardData) applyDashboardData(lastDashboardData);
  }
}

unlockButton.addEventListener("click", () => sendCommand("UNLOCK", unlockButton));
lockButton.addEventListener("click", () => sendCommand("LOCK", lockButton));




async function clearAlarmIncident() {
  if (!clearAlarmButton) return;
  const confirmed = window.confirm("Clear the latched security incident and start re-arming? Check the locker first.");
  if (!confirmed) return;
  clearAlarmButton.disabled = true;
  commandFeedback.className = "feedback";
  commandFeedback.textContent = "Sending alarm-clear request…";
  try {
    const response = await fetch(`${API}/clear-alarm`, { method: "POST" });
    if (response.status === 401) { redirectToLogin(); return; }
    const result = await response.json().catch(() => ({}));
    if (!response.ok || result.success === false) throw new Error(result.message || "Clear request failed");
    commandFeedback.className = "feedback success";
    commandFeedback.textContent = "Clear request queued; waiting for ESP32 to clear the incident and confirm re-arming.";
  } catch (error) {
    commandFeedback.className = "feedback error";
    commandFeedback.textContent = `Could not request alarm clear: ${error.message}`;
  } finally {
    clearAlarmButton.disabled = false;
  }
}
if (clearAlarmButton) clearAlarmButton.addEventListener("click", clearAlarmIncident);

document.querySelectorAll(".filter-button").forEach(button => button.addEventListener("click", () => {
  activeFilter = button.dataset.filter;
  document.querySelectorAll(".filter-button").forEach(item => item.classList.toggle("active", item === button));
  renderActivity();
}));

function connectLiveUpdates() {
  if (!("EventSource" in window)) {
    console.warn("EventSource is not supported; using periodic status refresh.");
    setInterval(updateDashboard, 3000);
    return;
  }

  if (liveStream) liveStream.close();
  liveStream = new EventSource(`${API}/stream`);

  liveStream.onmessage = event => {
    try {
      applyDashboardData(JSON.parse(event.data));
    } catch (error) {
      console.error("Could not process live dashboard update:", error);
    }
  };

  liveStream.onerror = () => {
    // EventSource reconnects automatically. Periodic status polling remains as fallback.
    console.warn("Live update stream interrupted; browser will attempt to reconnect.");
  };
}

if ("Notification" in window && Notification.permission === "default") {
  console.info("Browser notifications can be enabled from the browser's site settings when needed.");
}

// Identify the user first so role rules apply, then load state, live stream,
// and low-frequency fallback/recovery refresh.
loadCurrentUser().then(loggedIn => {
  if (!loggedIn) return;
  updateDashboard();
  connectLiveUpdates();
  setInterval(updateDashboard, 15000);
}).catch(error => {
  console.error("Could not load current user:", error);
  updateDashboard();
});
