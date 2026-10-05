const API = "/api";


// ========================================
// ELEMENTS
// ========================================

const securityCard =
    document.getElementById("security-card");

const securityIcon =
    document.getElementById("security-icon");

const securityStatus =
    document.getElementById("security-status");

const securityMessage =
    document.getElementById("security-message");


const lockIcon =
    document.getElementById("lock-icon");

const lockStatus =
    document.getElementById("lock-status");

const lockMessage =
    document.getElementById("lock-message");


const movementCard =
    document.getElementById("movement-card");

const movementIcon =
    document.getElementById("movement-icon");

const movementStatus =
    document.getElementById("movement-status");

const movementMessage =
    document.getElementById("movement-message");


const alarmCard =
    document.getElementById("alarm-card");

const alarmIcon =
    document.getElementById("alarm-icon");

const alarmStatus =
    document.getElementById("alarm-status");

const alarmMessage =
    document.getElementById("alarm-message");


const espStatus =
    document.getElementById("esp-status");

const sensorEsp32 =
    document.getElementById("sensor-esp32");


const activityLog =
    document.getElementById("activity-log");


const lockButton =
    document.getElementById("lock-btn");

const unlockButton =
    document.getElementById("unlock-btn");


// ========================================
// LOCK
// ========================================

lockButton.addEventListener("click", async () => {

    lockButton.disabled = true;

    try {

        await fetch(`${API}/lock`, {
            method: "POST"
        });

        await updateDashboard();

    } catch (error) {

        console.error(error);

    }

    lockButton.disabled = false;

});


// ========================================
// UNLOCK
// ========================================

unlockButton.addEventListener("click", async () => {

    unlockButton.disabled = true;

    try {

        await fetch(`${API}/unlock`, {
            method: "POST"
        });

        await updateDashboard();

    } catch (error) {

        console.error(error);

    }

    unlockButton.disabled = false;

});


// ========================================
// GET STATUS
// ========================================

async function updateDashboard() {

    try {

        const response =
            await fetch(`${API}/status`);

        if (!response.ok) {
            throw new Error("Server unavailable");
        }

        const data =
            await response.json();


        updateESP32(data.esp32);

        updateLock(data.locked);

        updateMovement(data.movement);

        updateAlarm(data.alarm);

        updateSecurity(data.security);

        updateActivity(data.activity);

    }

    catch (error) {

        console.error(
            "Dashboard error:",
            error
        );

        updateESP32("OFFLINE");

    }
}


// ========================================
// ESP32
// ========================================

function updateESP32(status) {

    if (status === "ONLINE") {

        espStatus.textContent =
            "● ONLINE";

        espStatus.className =
            "connection online";


        sensorEsp32.textContent =
            "ONLINE";

        sensorEsp32.className =
            "badge online-badge";

    }

    else {

        espStatus.textContent =
            "● OFFLINE";

        espStatus.className =
            "connection offline";


        sensorEsp32.textContent =
            "OFFLINE";

        sensorEsp32.className =
            "badge offline-badge";

    }
}


// ========================================
// LOCK STATUS
// ========================================

function updateLock(locked) {

    if (locked) {

        lockIcon.textContent = "🔒";

        lockStatus.textContent =
            "LOCKED";

        lockMessage.textContent =
            "Locker is secured";

    }

    else {

        lockIcon.textContent = "🔓";

        lockStatus.textContent =
            "UNLOCKED";

        lockMessage.textContent =
            "Authorized access mode";

    }
}


// ========================================
// MOVEMENT
// ========================================

function updateMovement(movement) {

    movementCard.classList.remove(
        "tampering",
        "waiting"
    );


    if (movement === "TAMPERING") {

        movementIcon.textContent =
            "🚨";

        movementStatus.textContent =
            "TAMPERING DETECTED";

        movementMessage.textContent =
            "Locker is being disturbed";

        movementCard.classList.add(
            "tampering"
        );

    }

    else if (movement === "WAITING") {

        movementIcon.textContent =
            "⚠️";

        movementStatus.textContent =
            "WAITING FOR STABILITY";

        movementMessage.textContent =
            "Waiting for movement to stop";

        movementCard.classList.add(
            "waiting"
        );

    }

    else {

        movementIcon.textContent =
            "📡";

        movementStatus.textContent =
            "LOW";

        movementMessage.textContent =
            "Locker is stable";

    }
}


// ========================================
// ALARM
// ========================================

function updateAlarm(alarm) {

    if (alarm === "ON") {

        alarmIcon.textContent =
            "🚨";

        alarmStatus.textContent =
            "ACTIVE";

        alarmMessage.textContent =
            "Tampering detected";

        alarmCard.classList.add(
            "danger"
        );

    }

    else {

        alarmIcon.textContent =
            "🔔";

        alarmStatus.textContent =
            "OFF";

        alarmMessage.textContent =
            "No active alarm";

        alarmCard.classList.remove(
            "danger"
        );

    }
}


// ========================================
// SECURITY
// ========================================

function updateSecurity(security) {

    securityCard.classList.remove(
        "tampering",
        "waiting",
        "unlocked"
    );


    if (security === "TAMPERING") {

        securityIcon.textContent =
            "🚨";

        securityStatus.textContent =
            "TAMPERING DETECTED";

        securityMessage.textContent =
            "Suspicious movement detected";

        securityCard.classList.add(
            "tampering"
        );

    }

    else if (
        security ===
        "WAITING_FOR_STABILITY"
    ) {

        securityIcon.textContent =
            "⚠️";

        securityStatus.textContent =
            "WAITING FOR STABILIZATION";

        securityMessage.textContent =
            "Movement must remain stable";

        securityCard.classList.add(
            "waiting"
        );

    }

    else if (
        security === "UNLOCKED"
    ) {

        securityIcon.textContent =
            "🔓";

        securityStatus.textContent =
            "UNLOCKED";

        securityMessage.textContent =
            "Authorized access mode";

        securityCard.classList.add(
            "unlocked"
        );

    }

    else {

        securityIcon.textContent =
            "🛡️";

        securityStatus.textContent =
            "SECURE";

        securityMessage.textContent =
            "Locker is secure and stable";

    }
}


// ========================================
// ACTIVITY
// ========================================

function updateActivity(events) {

    if (!events ||
        events.length === 0) {

        activityLog.innerHTML = `
            <div class="empty">
                Waiting for activity...
            </div>
        `;

        return;
    }


    activityLog.innerHTML =
        events.map(event => {

            let icon = "ℹ️";


            if (event.type === "TAMPER") {
                icon = "🚨";
            }

            else if (event.type === "STABLE") {
                icon = "🛡️";
            }

            else if (event.type === "LOCK") {
                icon = "🔒";
            }

            else if (event.type === "UNLOCK") {
                icon = "🔓";
            }


            return `
                <div class="log-item">

                    <span class="log-time">
                        ${event.time}
                    </span>

                    <span class="log-icon">
                        ${icon}
                    </span>

                    <span class="log-message">
                        ${event.message}
                    </span>

                </div>
            `;

        }).join("");
}


// ========================================
// START
// ========================================

updateDashboard();

setInterval(
    updateDashboard,
    1000
);