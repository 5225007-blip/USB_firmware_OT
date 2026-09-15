/**
 * @file    main.c
 * @brief   USB Guardian OT - example integration.
 *
 * This assumes a standard STM32CubeMX-generated project for the
 * STM32F407VGT6 with the following peripherals already configured by
 * CubeMX (not shown here - this file only contains the Guardian-specific
 * wiring that goes in place of/alongside the generated main()):
 *
 *   - I2C1        -> INA219 (current/voltage sense)          [PB6/PB7 typical]
 *   - CAN1        -> SN65HVD230 transceiver (OT network alerts)
 *   - USBH        -> USB Host middleware, host-side descriptor enumeration
 *                    of the UPSTREAM (untrusted) port, electrically isolated
 *                    from the downstream/host-facing port via the ADuM3160
 *                    until Relay_Close() is called
 *   - TIM (e.g. TIM6) -> periodic interrupt every GUARD_VBUS_SAMPLE_PERIOD_US
 *                    driving INA219_SampleAndDispatch()
 *   - EXTI        -> tamper switch on the housing -> Guardian_OnTamperTriggered()
 *   - USB CDC or UART -> serial link to the software daemon (Python, on the
 *                    OT gateway) for operator approve/deny + telemetry
 *
 * Wire your CubeMX-generated HAL_TIM_PeriodElapsedCallback,
 * HAL_GPIO_EXTI_Callback, and the USBH class driver's descriptor-ready
 * callback to the Guardian_On*() functions shown below.
 */

#include "stm32f4xx_hal.h"
#include "usb_guardian.h"
#include "relay_control.h"
#include "ina219.h"
#include "can_alert.h"
#include <string.h>

/* These handles are created by CubeMX-generated MX_I2C1_Init() /
   MX_CAN1_Init() / MX_TIM6_Init() - declared here for clarity only. */
extern I2C_HandleTypeDef hi2c1;
extern CAN_HandleTypeDef hcan1;
extern TIM_HandleTypeDef htim6;

static ina219_handle_t g_ina219;

/* Populate this from your organization's approved-device policy (or load
   it from flash/EEPROM - this static example matches the "Descriptor
   class inspection vs. whitelist" behavior from the briefing). */
static void Guardian_LoadWhitelist(void)
{
    /* Example: allow any HID keyboard/mouse from a specific approved
       vendor, and mass storage only from a specific VID/PID pair issued
       to field technicians. In a real deployment, replace this with a
       policy sync pulled from the software daemon over the serial/USB-CDC
       link rather than hardcoding here. */
    Guardian_AddWhitelistEntry(0x1234, 0xFFFF, GUARD_CLASS_HID);          /* any HID from vendor 0x1234 */
    Guardian_AddWhitelistEntry(0x0483, 0x5720, GUARD_CLASS_MASS_STORAGE); /* one specific tech drive */
}

void MX_GuardianApp_Init(void)
{
    Guardian_Init();
    INA219_Init(&g_ina219, &hi2c1);
    CanAlert_Init(&hcan1);
    Guardian_LoadWhitelist();

    /* Start the high-rate VBUS sampling timer (period configured in CubeMX
       to fire every GUARD_VBUS_SAMPLE_PERIOD_US). */
    HAL_TIM_Base_Start_IT(&htim6);
}

void MX_GuardianApp_Loop(void)
{
    Guardian_Poll();
}

/* ---------------------------------------------------------------------- */
/* HAL callback wiring                                                    */
/* ---------------------------------------------------------------------- */

/** Called by HAL from the TIM6 ISR every GUARD_VBUS_SAMPLE_PERIOD_US. */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM6) {
        INA219_SampleAndDispatch(&g_ina219);
    }
}

/** Called by HAL from the tamper-switch EXTI ISR. */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_13) { /* example tamper-switch pin */
        Guardian_OnTamperTriggered();
    }
}

/**
 * Example hook: call this from your USBH class driver once it has fully
 * read the device, configuration, and string descriptors from the
 * UPSTREAM port (the port is still electrically isolated from the
 * downstream host at this point - the USBH stack here is only being used
 * as an inspection tool, not a data path).
 */
void App_OnUsbDeviceEnumerated(uint16_t vid, uint16_t pid, uint8_t dev_class,
                                uint8_t dev_subclass, uint8_t dev_protocol,
                                const char *serial, const uint8_t *raw_desc, uint16_t raw_len)
{
    guard_descriptor_t desc = {0};
    desc.vid = vid;
    desc.pid = pid;
    desc.dev_class = dev_class;
    desc.dev_subclass = dev_subclass;
    desc.dev_protocol = dev_protocol;

    if (serial != NULL) {
        strncpy(desc.serial, serial, sizeof(desc.serial) - 1);
    }
    if (raw_desc != NULL && raw_len <= GUARD_DESCRIPTOR_MAX_BYTES) {
        memcpy(desc.raw, raw_desc, raw_len);
        desc.raw_len = raw_len;
    }

    Guardian_OnDescriptorsRead(&desc);
}

/**
 * Example hook: call this from your HID report parser for every decoded
 * keystroke, while the device is CONNECTED (post-relay).
 */
void App_OnHidKeystroke(void)
{
    Guardian_OnHidKeystroke(HAL_GetTick());
}

int main(void)
{
    HAL_Init();
    /* SystemClock_Config(), MX_GPIO_Init(), MX_I2C1_Init(), MX_CAN1_Init(),
       MX_TIM6_Init(), MX_USB_HOST_Init(), etc. - CubeMX-generated, omitted. */

    MX_GuardianApp_Init();

    while (1) {
        MX_GuardianApp_Loop();
        /* MX_USB_HOST_Process(); goes here in the generated main loop. */
    }
}
