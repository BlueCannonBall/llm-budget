"use strict";

const form = document.getElementById("usage-form");
const keyInput = document.getElementById("api-key");
const settings = document.getElementById("key-settings");
const settingsLabel = document.getElementById("key-settings-label");
const remember = document.getElementById("remember-key");
const forget = document.getElementById("forget-key");
const refresh = document.getElementById("refresh-usage");
const result = document.getElementById("usage-result");
const message = document.getElementById("usage-message");
const submit = form.querySelector('button[type="submit"]');
const storageKey = "llm-budget.usage-api-key";
const formatter = new Intl.DateTimeFormat(undefined, {
    dateStyle: "medium",
    timeStyle: "short"
});
let storageAvailable = true;
let successfulKey = "";
let loading = false;

function localizeTimes() {
    for (const timestamp of result.querySelectorAll("time[datetime]")) {
        const date = new Date(timestamp.dateTime);
        if (!Number.isNaN(date.getTime())) timestamp.textContent = formatter.format(date);
        const countdown = document.createElement("span");
        countdown.className = "reset-countdown";
        timestamp.before(countdown);
    }

    const heading = result.querySelector("#reset-timezone");
    if (heading) heading.textContent = `Reset (${formatter.resolvedOptions().timeZone})`;
    updateResetCountdowns();
}

function updateResetCountdowns() {
    const now = Date.now();
    for (const timestamp of result.querySelectorAll("time[datetime]")) {
        const countdown = timestamp.previousElementSibling;
        if (!countdown?.classList.contains("reset-countdown")) continue;

        const remaining = Math.ceil((Date.parse(timestamp.dateTime) - now) / 60000);
        if (!Number.isFinite(remaining)) continue;
        if (remaining <= 0) {
            countdown.textContent = "Reset due";
            continue;
        }

        const days = Math.floor(remaining / 1440);
        const hours = Math.floor(remaining % 1440 / 60);
        const minutes = remaining % 60;
        const parts = [];
        if (days) parts.push(`${days}d`);
        if (hours) parts.push(`${hours}h`);
        if (minutes) parts.push(`${minutes}m`);
        countdown.textContent = `Resets in ${parts.join(" ")}`;
    }
}

function updateControls() {
    const hasUsage = Boolean(result.querySelector("table"));
    settingsLabel.textContent = hasUsage ? "Key settings" : "API key";
    refresh.hidden = !hasUsage || !keyInput.value;
}

function setLoading(value) {
    loading = value;
    submit.disabled = value;
    refresh.disabled = value;
    forget.disabled = value;
    keyInput.disabled = value;
    remember.disabled = value || !storageAvailable;
    result.setAttribute("aria-busy", String(value));
}

function removeSavedKey() {
    remember.checked = false;

    try {
        localStorage.removeItem(storageKey);
        return "";
    } catch {
        return "Browser storage is unavailable. Clear this site's browser data to remove any saved key.";
    }
}

function saveSuccessfulKey() {
    if (!remember.checked || !successfulKey || keyInput.value !== successfulKey) return "";

    try {
        localStorage.setItem(storageKey, successfulKey);
        return "";
    } catch {
        remember.checked = false;
        return "Usage loaded, but browser storage is unavailable. The key could not be remembered.";
    }
}

async function showUsage() {
    if (loading) return;

    const apiKey = keyInput.value;
    setLoading(true);
    message.textContent = "Loading usage...";

    try {
        const response = await fetch(form.action, {
            method: "POST",
            headers: {"Content-Type": "application/x-www-form-urlencoded"},
            body: new URLSearchParams({api_key: apiKey}),
            cache: "no-store"
        });
        const page = new DOMParser().parseFromString(await response.text(), "text/html");
        const details = page.getElementById("usage-result");

        if (response.status === 401) {
            successfulKey = "";
            const storageMessage = removeSavedKey();
            result.replaceChildren();
            if (details) result.append(...details.childNodes);
            settings.open = true;
            message.textContent = storageMessage || "The API key is invalid. Enter a valid key to view usage.";
        } else if (!details || (response.ok && !details.querySelector("table"))) {
            throw new Error("Unexpected usage response");
        } else if (response.ok) {
            result.replaceChildren(...details.childNodes);
            successfulKey = apiKey;
            message.textContent = saveSuccessfulKey();
            settings.open = false;
            localizeTimes();
        } else {
            result.replaceChildren(...details.childNodes);
            settings.open = true;
            message.textContent = "Could not load usage. Check your key and try again.";
        }
    } catch {
        settings.open = true;
        message.textContent = "Could not load usage. Please try again.";
    } finally {
        setLoading(false);
        updateControls();
    }
}

form.addEventListener("submit", event => {
    event.preventDefault();
    showUsage();
});

refresh.addEventListener("click", () => {
    if (!form.checkValidity()) {
        settings.open = true;
        form.reportValidity();
        return;
    }
    showUsage();
});

forget.addEventListener("click", () => {
    message.textContent = removeSavedKey();
    keyInput.value = "";
    successfulKey = "";
    result.replaceChildren();
    settings.open = true;
    updateControls();
    keyInput.focus();
});

remember.addEventListener("change", () => {
    message.textContent = remember.checked ? saveSuccessfulKey() : removeSavedKey();
});

keyInput.addEventListener("input", updateControls);

document.getElementById("remember-controls").hidden = false;
localizeTimes();
setInterval(updateResetCountdowns, 60000);
if (result.querySelector("table")) settings.open = false;
try {
    const savedKey = localStorage.getItem(storageKey);
    if (savedKey) {
        keyInput.value = savedKey;
        remember.checked = true;
        showUsage();
    }
} catch {
    storageAvailable = false;
    remember.disabled = true;
    message.textContent = "Browser storage is unavailable. You can still enter a key to view usage.";
}
updateControls();
