/**
 * @file    relay_control.h
 * @brief   Hardware relay (Omron G5V-1) + SCR crowbar control on VBUS.
 *
 * The relay is the trust boundary: while open, the downstream (OT host)
 * side of VBUS/D+/D- is physically disconnected from the upstream
 * (untrusted device) side. Nothing in software can "logically" grant
 * access - only Relay_Close() can, and only Guardian core calls it.
 */

#ifndef RELAY_CONTROL_H
#define RELAY_CONTROL_H

#include <stdint.h>
#include <stdbool.h>

/* GPIO mapping - adjust to your board's CubeMX pinout */
#define RELAY_CTRL_GPIO_PORT     GPIOB
#define RELAY_CTRL_GPIO_PIN      GPIO_PIN_0   /* Drives G5V-1 coil via transistor */

#define CROWBAR_SCR_GPIO_PORT    GPIOB
#define CROWBAR_SCR_GPIO_PIN     GPIO_PIN_1   /* Gate drive for crowbar SCR */

void Relay_Init(void);

/** Physically connect upstream device to the OT host. Only call after full inspection passes. */
void Relay_Close(void);

/** Physically disconnect. Safe to call at any time, including from an ISR. */
void Relay_Open(void);

bool Relay_IsClosed(void);

/**
 * Fire the SCR crowbar: clamps VBUS to ground in hardware, independent of
 * any further software execution. Call this the instant an overvoltage
 * sample is seen - do not wait for the main loop.
 */
void Crowbar_Fire(void);

/** Release the crowbar once VBUS has been confirmed safe by Guardian_OnCurrentSample(). */
void Crowbar_Reset(void);

bool Crowbar_IsActive(void);

#endif /* RELAY_CONTROL_H */
