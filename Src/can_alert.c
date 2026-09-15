/**
 * @file    can_alert.c
 * @brief   CAN alert transmission implementation.
 */

#include "can_alert.h"
#include "stm32f4xx_hal.h"

static CAN_HandleTypeDef *guard_hcan = NULL;
static uint8_t seq_counter = 0;

void CanAlert_Init(void *hcan_handle)
{
    guard_hcan = (CAN_HandleTypeDef *)hcan_handle;
    HAL_CAN_Start(guard_hcan);
}

void CanAlert_Send(guard_can_alert_type_t type, guardian_state_t state,
                    guard_reject_reason_t reason, uint16_t vid, uint16_t pid)
{
    if (guard_hcan == NULL) {
        return;
    }

    CAN_TxHeaderTypeDef header = {0};
    uint8_t data[8];
    uint32_t mailbox;

    header.StdId = GUARD_CAN_BASE_ID + (uint32_t)type;
    header.IDE   = CAN_ID_STD;
    header.RTR   = CAN_RTR_DATA;
    header.DLC   = 8;

    data[0] = (uint8_t)type;
    data[1] = (uint8_t)state;
    data[2] = (uint8_t)reason;
    data[3] = (uint8_t)(vid & 0xFF);
    data[4] = (uint8_t)(vid >> 8);
    data[5] = (uint8_t)(pid & 0xFF);
    data[6] = (uint8_t)(pid >> 8);
    data[7] = seq_counter++;

    /* Best-effort: if all three mailboxes are full (unlikely at Guardian's
       alert rate), drop rather than block - a stuck CAN TX must never stall
       the relay/crowbar decision path. */
    if (HAL_CAN_GetTxMailboxesFreeLevel(guard_hcan) > 0) {
        HAL_CAN_AddTxMessage(guard_hcan, &header, data, &mailbox);
    }
}
