/**
 * @file    can_alert.h
 * @brief   CAN bus alerting (SN65HVD230 transceiver) for immediate
 *          OT-network-side notification, independent of the software
 *          daemon / Ethernet path (defense in depth: CAN keeps working
 *          even if the gateway's IP stack is the thing under attack).
 */

#ifndef CAN_ALERT_H
#define CAN_ALERT_H

#include <stdint.h>
#include "usb_guardian.h"

/* Base CAN ID for Guardian alert frames; adjust to your OT network's
   ID allocation plan to avoid collisions with SCADA/PLC traffic. */
#define GUARD_CAN_BASE_ID          0x700

typedef enum {
    GUARD_ALERT_STATE_CHANGE   = 0x00,
    GUARD_ALERT_REJECTED       = 0x01,
    GUARD_ALERT_CROWBAR_FIRED  = 0x02,
    GUARD_ALERT_TAMPER         = 0x03,
    GUARD_ALERT_WORM_DETECTED  = 0x04,
} guard_can_alert_type_t;

void CanAlert_Init(void *hcan_handle);

/**
 * Send an 8-byte alert frame:
 *   byte0    = alert type (guard_can_alert_type_t)
 *   byte1    = guardian_state_t at time of alert
 *   byte2    = guard_reject_reason_t (0 if not applicable)
 *   byte3-4  = VID (little-endian), if applicable
 *   byte5-6  = PID (little-endian), if applicable
 *   byte7    = reserved / sequence counter
 */
void CanAlert_Send(guard_can_alert_type_t type, guardian_state_t state,
                    guard_reject_reason_t reason, uint16_t vid, uint16_t pid);

#endif /* CAN_ALERT_H */
