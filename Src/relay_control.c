/**
 * @file    relay_control.c
 * @brief   Implementation of the hardware relay / crowbar driver.
 */

#include "relay_control.h"
#include "stm32f4xx_hal.h"

static volatile bool relay_closed  = false;
static volatile bool crowbar_active = false;

void Relay_Init(void)
{
    /* Ensure fail-safe state: relay open, crowbar released, on every boot/reset. */
    HAL_GPIO_WritePin(RELAY_CTRL_GPIO_PORT, RELAY_CTRL_GPIO_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(CROWBAR_SCR_GPIO_PORT, CROWBAR_SCR_GPIO_PIN, GPIO_PIN_RESET);
    relay_closed = false;
    crowbar_active = false;
}

void Relay_Close(void)
{
    /* Never close the relay while the crowbar is latched - that would just
       reconnect the fault straight to the host. */
    if (crowbar_active) {
        return;
    }
    HAL_GPIO_WritePin(RELAY_CTRL_GPIO_PORT, RELAY_CTRL_GPIO_PIN, GPIO_PIN_SET);
    relay_closed = true;
}

void Relay_Open(void)
{
    HAL_GPIO_WritePin(RELAY_CTRL_GPIO_PORT, RELAY_CTRL_GPIO_PIN, GPIO_PIN_RESET);
    relay_closed = false;
}

bool Relay_IsClosed(void)
{
    return relay_closed;
}

void Crowbar_Fire(void)
{
    /* Open the relay first (breaks the circuit to the host side), then latch
       the crowbar to sink any remaining energy on the upstream side to ground. */
    Relay_Open();
    HAL_GPIO_WritePin(CROWBAR_SCR_GPIO_PORT, CROWBAR_SCR_GPIO_PIN, GPIO_PIN_SET);
    crowbar_active = true;
}

void Crowbar_Reset(void)
{
    HAL_GPIO_WritePin(CROWBAR_SCR_GPIO_PORT, CROWBAR_SCR_GPIO_PIN, GPIO_PIN_RESET);
    crowbar_active = false;
}

bool Crowbar_IsActive(void)
{
    return crowbar_active;
}
