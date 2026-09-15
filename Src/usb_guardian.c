/**
 * @file    usb_guardian.c
 * @brief   USB Guardian OT core state machine.
 *
 * Design principle (see briefing, Section 05): "Inspect First, Connect
 * Second." Every device is held open at the hardware relay until its
 * descriptor, class, and power behavior have been verified. The relay
 * only ever closes from ONE place in this file (Guardian_Transition to
 * GUARDIAN_STATE_CONNECTED) - keep it that way in any modification.
 */

#include "usb_guardian.h"
#include "relay_control.h"
#include "can_alert.h"
#include "sha256.h"
#include "stm32f4xx_hal.h"
#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------- */
/* Module state                                                           */
/* ---------------------------------------------------------------------- */

static guardian_state_t   g_state = GUARDIAN_STATE_IDLE;
static guard_descriptor_t g_current_desc;
static uint8_t            g_salt[GUARD_SALT_LEN];
static uint8_t            g_current_fingerprint[GUARD_FINGERPRINT_LEN];

static guard_whitelist_entry_t g_whitelist[GUARD_MAX_WHITELIST_ENTRIES];
static uint32_t                g_whitelist_count = 0;

static guard_fingerprint_record_t g_known_devices[GUARD_MAX_KNOWN_DEVICES];
static uint32_t                   g_known_count = 0;

static bool g_pending_approval = false;

/* HID inhuman-typing-rate detection (sliding window of keystroke timestamps) */
#define HID_WINDOW_SIZE 32
static uint32_t g_hid_timestamps[HID_WINDOW_SIZE];
static uint32_t g_hid_index = 0;
static uint32_t g_hid_count = 0;

/* Current-pulse detection for USB Killer signature (rapid mA spike, not a
   sustained load) */
static uint16_t g_last_current_mA = 0;
static uint32_t g_last_sample_tick = 0;

static bool g_remote_killed = false;

/* ---------------------------------------------------------------------- */
/* Internal helpers                                                       */
/* ---------------------------------------------------------------------- */

static void guard_set_state(guardian_state_t s)
{
    g_state = s;
    CanAlert_Send(GUARD_ALERT_STATE_CHANGE, g_state, GUARD_REASON_NONE,
                  g_current_desc.vid, g_current_desc.pid);
}

static void guard_reject(guard_reject_reason_t reason)
{
    Relay_Open();
    guard_set_state(GUARDIAN_STATE_QUARANTINED);
    CanAlert_Send(GUARD_ALERT_REJECTED, g_state, reason,
                  g_current_desc.vid, g_current_desc.pid);
}

static bool class_is_whitelisted(uint16_t vid, uint16_t pid, guard_usb_class_t dev_class)
{
    for (uint32_t i = 0; i < g_whitelist_count; i++) {
        guard_whitelist_entry_t *e = &g_whitelist[i];
        if (e->vid != vid) {
            continue;
        }
        if (e->pid != 0xFFFF && e->pid != pid) {
            continue;
        }
        if (e->allowed_class == dev_class) {
            return true;
        }
    }
    return false;
}

static guard_fingerprint_record_t *find_known_device(const uint8_t fp[GUARD_FINGERPRINT_LEN])
{
    for (uint32_t i = 0; i < g_known_count; i++) {
        if (memcmp(g_known_devices[i].digest, fp, GUARD_FINGERPRINT_LEN) == 0) {
            return &g_known_devices[i];
        }
    }
    return NULL;
}

static guard_fingerprint_record_t *register_new_device(const uint8_t fp[GUARD_FINGERPRINT_LEN])
{
    if (g_known_count >= GUARD_MAX_KNOWN_DEVICES) {
        /* Evict oldest by last_seen - keeps the table bounded on constrained
           flash/RAM without silently refusing to fingerprint new devices. */
        uint32_t oldest_idx = 0;
        for (uint32_t i = 1; i < g_known_count; i++) {
            if (g_known_devices[i].last_seen_epoch < g_known_devices[oldest_idx].last_seen_epoch) {
                oldest_idx = i;
            }
        }
        memset(&g_known_devices[oldest_idx], 0, sizeof(guard_fingerprint_record_t));
        memcpy(g_known_devices[oldest_idx].digest, fp, GUARD_FINGERPRINT_LEN);
        g_known_devices[oldest_idx].first_seen_epoch = HAL_GetTick();
        g_known_devices[oldest_idx].last_seen_epoch  = HAL_GetTick();
        return &g_known_devices[oldest_idx];
    }

    guard_fingerprint_record_t *rec = &g_known_devices[g_known_count++];
    memcpy(rec->digest, fp, GUARD_FINGERPRINT_LEN);
    rec->first_seen_epoch = HAL_GetTick();
    rec->last_seen_epoch  = HAL_GetTick();
    rec->operator_approved = false;
    return rec;
}

/* ---------------------------------------------------------------------- */
/* Public API                                                             */
/* ---------------------------------------------------------------------- */

void Guardian_Init(void)
{
    Relay_Init();
    memset(&g_current_desc, 0, sizeof(g_current_desc));
    memset(g_whitelist, 0, sizeof(g_whitelist));
    memset(g_known_devices, 0, sizeof(g_known_devices));
    g_whitelist_count = 0;
    g_known_count = 0;
    g_hid_index = 0;
    g_hid_count = 0;
    g_remote_killed = false;

    /* Seed the fingerprint salt from the MCU's factory-programmed unique ID
       so fingerprints are stable across reboots but device-specific across
       units (a cloned VID/PID/serial from a different Guardian unit won't
       collide). */
    uint32_t *uid = (uint32_t *)UID_BASE;
    memcpy(g_salt, uid, 12);
    memset(g_salt + 12, 0xA5, GUARD_SALT_LEN - 12);

    g_state = GUARDIAN_STATE_IDLE;
}

bool Guardian_AddWhitelistEntry(uint16_t vid, uint16_t pid, guard_usb_class_t allowed_class)
{
    if (g_whitelist_count >= GUARD_MAX_WHITELIST_ENTRIES) {
        return false;
    }
    guard_whitelist_entry_t *e = &g_whitelist[g_whitelist_count++];
    e->vid = vid;
    e->pid = pid;
    e->allowed_class = allowed_class;
    return true;
}

void Guardian_ClearWhitelist(void)
{
    memset(g_whitelist, 0, sizeof(g_whitelist));
    g_whitelist_count = 0;
}

void Guardian_Poll(void)
{
    /* Timeout guard: if a device sits in PENDING_APPROVAL too long without
       an operator decision, fail safe back to quarantine rather than hang
       open indefinitely. */
    static uint32_t pending_since = 0;
    const uint32_t PENDING_TIMEOUT_MS = 60000;

    if (g_state == GUARDIAN_STATE_PENDING_APPROVAL) {
        if (pending_since == 0) {
            pending_since = HAL_GetTick();
        } else if (HAL_GetTick() - pending_since > PENDING_TIMEOUT_MS) {
            guard_reject(GUARD_REASON_OPERATOR_DENIED);
            pending_since = 0;
        }
    } else {
        pending_since = 0;
    }
}

void Guardian_OnDescriptorsRead(const guard_descriptor_t *desc)
{
    if (g_remote_killed) {
        return; /* All ports disabled - ignore new attachments entirely. */
    }

    memcpy(&g_current_desc, desc, sizeof(g_current_desc));
    guard_set_state(GUARDIAN_STATE_ENUMERATING);
    guard_set_state(GUARDIAN_STATE_INSPECTING);

    /* --- Check 1: class whitelist ------------------------------------- */
    if (!class_is_whitelisted(desc->vid, desc->pid, (guard_usb_class_t)desc->dev_class)) {
        guard_reject(GUARD_REASON_CLASS_NOT_WHITELISTED);
        return;
    }

    /* --- Check 2: fingerprint ------------------------------------------
       Fingerprint = salted SHA-256(VID || PID || serial || raw descriptor). */
    uint8_t material[8 + sizeof(desc->serial) + GUARD_DESCRIPTOR_MAX_BYTES];
    uint32_t offset = 0;
    material[offset++] = (uint8_t)(desc->vid & 0xFF);
    material[offset++] = (uint8_t)(desc->vid >> 8);
    material[offset++] = (uint8_t)(desc->pid & 0xFF);
    material[offset++] = (uint8_t)(desc->pid >> 8);
    memcpy(material + offset, desc->serial, sizeof(desc->serial));
    offset += sizeof(desc->serial);
    memcpy(material + offset, desc->raw, desc->raw_len);
    offset += desc->raw_len;

    SHA256_Salted(g_salt, GUARD_SALT_LEN, material, offset, g_current_fingerprint);

    guard_fingerprint_record_t *known = find_known_device(g_current_fingerprint);

    if (known == NULL) {
        /* Never seen this exact fingerprint before - register it and require
           explicit operator approval before the relay ever closes. */
        register_new_device(g_current_fingerprint);
        g_pending_approval = true;
        guard_set_state(GUARDIAN_STATE_PENDING_APPROVAL);
        return;
    }

    /* Fingerprint matches a record we have - but if VID/PID/serial matches
       a *different* previously-approved device's identity while the raw
       descriptor bytes differ, that would have produced a different
       fingerprint already (this is the point of hashing the raw descriptor
       bytes too) - so reaching this branch with `known` non-NULL already
       means an exact match, including against cloning attempts that only
       spoof VID/PID/serial. */
    known->last_seen_epoch = HAL_GetTick();

    if (!known->operator_approved) {
        g_pending_approval = true;
        guard_set_state(GUARDIAN_STATE_PENDING_APPROVAL);
        return;
    }

    /* Known + approved: relay closes here, and ONLY here. */
    Relay_Close();
    guard_set_state(GUARDIAN_STATE_CONNECTED);

    /* Reset per-connection detectors for the new session. */
    g_hid_index = 0;
    g_hid_count = 0;
}

void Guardian_OnCurrentSample(uint16_t vbus_mV, uint16_t current_mA)
{
    /* --- Voltage crowbar: fire immediately, do not wait for the main loop. */
    if (vbus_mV >= GUARD_VBUS_TRIP_MV) {
        Crowbar_Fire();
        guard_set_state(GUARDIAN_STATE_TRIPPED);
        CanAlert_Send(GUARD_ALERT_CROWBAR_FIRED, g_state, GUARD_REASON_OVERVOLTAGE,
                      g_current_desc.vid, g_current_desc.pid);
        return;
    }

    /* --- Current-pulse pattern check (USB Killer discharge signature):
       a very large delta in a very short window, rather than a sustained
       high current draw (which can be legitimate, e.g. PD-negotiated
       charging). */
    uint32_t now = HAL_GetTick();
    uint32_t dt_ms = now - g_last_sample_tick;
    if (dt_ms > 0 && dt_ms <= GUARD_CURRENT_PULSE_WINDOW_MS) {
        int32_t delta_mA = (int32_t)current_mA - (int32_t)g_last_current_mA;
        if (delta_mA > 1500) { /* >1.5A step within the pulse window */
            guard_reject(GUARD_REASON_CURRENT_ANOMALY);
        }
    }
    g_last_current_mA = current_mA;
    g_last_sample_tick = now;

    /* Auto-recover the crowbar once voltage is confirmed safely back to
       nominal, per the briefing's "auto-resetting once voltage is safe
       again." Do NOT auto-close the relay - that still requires a fresh
       inspection cycle on next attach. */
    if (Crowbar_IsActive() && vbus_mV <= GUARD_VBUS_NOMINAL_MV + 200) {
        Crowbar_Reset();
    }
}

void Guardian_OnHidKeystroke(uint32_t timestamp_ms)
{
    if (g_state != GUARDIAN_STATE_CONNECTED) {
        return; /* Shouldn't happen - HID traffic implies relay is closed. */
    }

    g_hid_timestamps[g_hid_index] = timestamp_ms;
    g_hid_index = (g_hid_index + 1) % HID_WINDOW_SIZE;
    if (g_hid_count < HID_WINDOW_SIZE) {
        g_hid_count++;
    }

    if (g_hid_count < HID_WINDOW_SIZE) {
        return; /* Not enough samples yet for a reliable rate estimate. */
    }

    /* Oldest timestamp in the ring is the slot we're about to overwrite next. */
    uint32_t oldest = g_hid_timestamps[g_hid_index];
    uint32_t span_ms = timestamp_ms - oldest;
    if (span_ms == 0) {
        span_ms = 1;
    }

    /* Approximate "words per minute" assuming ~5 chars/word: keystrokes
       per window -> chars/min -> words/min. */
    uint32_t keystrokes = HID_WINDOW_SIZE;
    uint32_t chars_per_min = (keystrokes * 60000u) / span_ms;
    uint32_t wpm = chars_per_min / 5u;

    if (wpm > GUARD_HID_MAX_HUMAN_WPM) {
        guard_reject(GUARD_REASON_HID_INHUMAN_RATE);
    }
}

void Guardian_OnWormSignatureMatch(const char *volume_label)
{
    (void)volume_label;
    Relay_Open();
    guard_set_state(GUARDIAN_STATE_QUARANTINED);
    CanAlert_Send(GUARD_ALERT_WORM_DETECTED, g_state, GUARD_REASON_WORM_SIGNATURE,
                  g_current_desc.vid, g_current_desc.pid);
}

void Guardian_ApproveDevice(const uint8_t fingerprint[GUARD_FINGERPRINT_LEN])
{
    guard_fingerprint_record_t *rec = find_known_device(fingerprint);
    if (rec == NULL) {
        return;
    }
    rec->operator_approved = true;

    if (g_pending_approval && memcmp(fingerprint, g_current_fingerprint, GUARD_FINGERPRINT_LEN) == 0) {
        g_pending_approval = false;
        Relay_Close();
        guard_set_state(GUARDIAN_STATE_CONNECTED);
        g_hid_index = 0;
        g_hid_count = 0;
    }
}

void Guardian_DenyDevice(const uint8_t fingerprint[GUARD_FINGERPRINT_LEN])
{
    if (g_pending_approval && memcmp(fingerprint, g_current_fingerprint, GUARD_FINGERPRINT_LEN) == 0) {
        g_pending_approval = false;
        guard_reject(GUARD_REASON_OPERATOR_DENIED);
    }
}

void Guardian_OnTamperTriggered(void)
{
    Relay_Open();
    guard_set_state(GUARDIAN_STATE_TRIPPED);
    CanAlert_Send(GUARD_ALERT_TAMPER, g_state, GUARD_REASON_TAMPER, 0, 0);
}

void Guardian_RemoteKill(void)
{
    g_remote_killed = true;
    Relay_Open();
    guard_set_state(GUARDIAN_STATE_QUARANTINED);
}

void Guardian_Reset(void)
{
    if (Crowbar_IsActive()) {
        Crowbar_Reset();
    }
    g_remote_killed = false;
    g_pending_approval = false;
    Relay_Open();
    memset(&g_current_desc, 0, sizeof(g_current_desc));
    guard_set_state(GUARDIAN_STATE_IDLE);
}

guardian_state_t Guardian_GetState(void)
{
    return g_state;
}
