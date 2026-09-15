/**
 * @file    usb_guardian.h
 * @brief   USB Guardian OT - Firmware core: "Inspect First, Connect Second"
 *
 * Target: STM32F407VGT6 (STM32F4 HAL / USB Host Library / I2C / CAN)
 *
 * This header defines the state machine that gates every USB device behind
 * a hardware relay until its descriptor, class, and power behavior have
 * been verified. Nothing downstream (the OT host) sees a device until
 * GUARDIAN_STATE_CONNECTED is reached.
 */

#ifndef USB_GUARDIAN_H
#define USB_GUARDIAN_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------- */
/* Configuration                                                          */
/* ---------------------------------------------------------------------- */

#define GUARD_MAX_WHITELIST_ENTRIES     32
#define GUARD_MAX_KNOWN_DEVICES         64
#define GUARD_DESCRIPTOR_MAX_BYTES      256
#define GUARD_FINGERPRINT_LEN           32   /* SHA-256 digest */
#define GUARD_SALT_LEN                  16

/* Voltage crowbar threshold (see slide 20: "Voltage Crowbar Protection") */
#define GUARD_VBUS_TRIP_MV              6000u   /* 6.0V absolute trip point */
#define GUARD_VBUS_NOMINAL_MV           5000u
#define GUARD_VBUS_SAMPLE_PERIOD_US     50u     /* ADC sampled every 50us  */

/* HID "typing speed" heuristic (Rubber Ducky detection, slide 18) */
#define GUARD_HID_MAX_HUMAN_WPM         600u

/* Current-pulse pattern window for USB Killer detection */
#define GUARD_CURRENT_PULSE_WINDOW_MS   5u

/* ---------------------------------------------------------------------- */
/* USB device classes (subset of USB-IF base class codes we care about)   */
/* ---------------------------------------------------------------------- */

typedef enum {
    GUARD_CLASS_UNKNOWN        = 0x00,
    GUARD_CLASS_HID            = 0x03,
    GUARD_CLASS_MASS_STORAGE   = 0x08,
    GUARD_CLASS_HUB            = 0x09,
    GUARD_CLASS_VENDOR_SPECIFIC= 0xFF
} guard_usb_class_t;

/* ---------------------------------------------------------------------- */
/* Guardian state machine                                                 */
/* ---------------------------------------------------------------------- */

typedef enum {
    GUARDIAN_STATE_IDLE = 0,        /* No device present, relay open       */
    GUARDIAN_STATE_DETECTED,        /* D+/D- pull-up seen, relay STILL open*/
    GUARDIAN_STATE_ENUMERATING,     /* Reading descriptors via isolated
                                        host-side sniff, relay STILL open  */
    GUARDIAN_STATE_INSPECTING,      /* Fingerprint + whitelist + current
                                        checks running                    */
    GUARDIAN_STATE_PENDING_APPROVAL,/* Unknown device - needs operator OK */
    GUARDIAN_STATE_CONNECTED,       /* Relay CLOSED - device reaches host */
    GUARDIAN_STATE_QUARANTINED,     /* Failed inspection - relay stays open,
                                        alert raised                      */
    GUARDIAN_STATE_TRIPPED          /* Crowbar fired - hard fault latch,
                                        requires manual reset             */
} guardian_state_t;

typedef enum {
    GUARD_REASON_NONE = 0,
    GUARD_REASON_CLASS_NOT_WHITELISTED,
    GUARD_REASON_FINGERPRINT_MISMATCH,
    GUARD_REASON_CURRENT_ANOMALY,
    GUARD_REASON_OVERVOLTAGE,
    GUARD_REASON_HID_INHUMAN_RATE,
    GUARD_REASON_WORM_SIGNATURE,
    GUARD_REASON_OPERATOR_DENIED,
    GUARD_REASON_TAMPER
} guard_reject_reason_t;

/* ---------------------------------------------------------------------- */
/* Descriptor / fingerprint model                                         */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint16_t vid;
    uint16_t pid;
    uint8_t  dev_class;
    uint8_t  dev_subclass;
    uint8_t  dev_protocol;
    char     serial[64];
    uint8_t  raw[GUARD_DESCRIPTOR_MAX_BYTES];
    uint16_t raw_len;
} guard_descriptor_t;

typedef struct {
    uint8_t  digest[GUARD_FINGERPRINT_LEN]; /* salted SHA-256 */
    uint32_t first_seen_epoch;
    uint32_t last_seen_epoch;
    bool     operator_approved;
} guard_fingerprint_record_t;

typedef struct {
    uint16_t vid;
    uint16_t pid;                 /* 0xFFFF = wildcard (any PID for this VID) */
    guard_usb_class_t allowed_class;
} guard_whitelist_entry_t;

/* ---------------------------------------------------------------------- */
/* Public API                                                             */
/* ---------------------------------------------------------------------- */

/** One-time init: seeds RNG/salt, clears state, opens relay. Call after HAL_Init(). */
void Guardian_Init(void);

/**
 * Add one entry to the device-class whitelist (e.g. "VID 0x1234, any PID,
 * class HID"). Call during boot policy load, or when the software daemon
 * pushes a policy update over the serial/USB-CDC link. Returns false if
 * the table is full.
 */
bool Guardian_AddWhitelistEntry(uint16_t vid, uint16_t pid, guard_usb_class_t allowed_class);

/** Clear the whitelist (e.g. before applying a freshly synced policy). */
void Guardian_ClearWhitelist(void);

/** Call from the main loop / a periodic task (recommended >= 1kHz). */
void Guardian_Poll(void);

/**
 * Called by the isolated USB host stack once full descriptors have been
 * read from a newly attached (but still electrically disconnected-from-host)
 * device. Guardian owns the decision of whether the relay ever closes.
 */
void Guardian_OnDescriptorsRead(const guard_descriptor_t *desc);

/** Called by the current-sense ISR/DMA callback with the latest VBUS mA and mV reading. */
void Guardian_OnCurrentSample(uint16_t vbus_mV, uint16_t current_mA);

/** Called by the HID class parser whenever a keystroke event is decoded pre-relay. */
void Guardian_OnHidKeystroke(uint32_t timestamp_ms);

/** Called by the mass-storage pre-scan if a worm signature is matched on a mounted volume. */
void Guardian_OnWormSignatureMatch(const char *volume_label);

/** Operator approval path (from the dashboard, over the software daemon's serial/USB-CDC link). */
void Guardian_ApproveDevice(const uint8_t fingerprint[GUARD_FINGERPRINT_LEN]);
void Guardian_DenyDevice(const uint8_t fingerprint[GUARD_FINGERPRINT_LEN]);

/** Physical housing tamper switch ISR calls this directly. */
void Guardian_OnTamperTriggered(void);

/** Remote SOC kill-switch: disable all USB ports immediately (idempotent). */
void Guardian_RemoteKill(void);

/** Manual reset after a TRIPPED or QUARANTINED latch (physical button or authenticated command). */
void Guardian_Reset(void);

/** Current state, for the software daemon / CAN alert payloads. */
guardian_state_t Guardian_GetState(void);

#endif /* USB_GUARDIAN_H */
