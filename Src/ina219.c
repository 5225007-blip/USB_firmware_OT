/**
 * @file    ina219.c
 * @brief   INA219 driver implementation (STM32 HAL I2C).
 */

#include "ina219.h"
#include "usb_guardian.h"
#include "stm32f4xx_hal.h"
#include <string.h>

/* Calibration for 0.1 ohm shunt, 3.2A max expected current.
   Current_LSB chosen per INA219 datasheet formula:
     Current_LSB = Max_Expected_Current / 32768
   With a 0.1R shunt this gives Current_LSB ~= 0.1mA, Cal ~= 4096. */
#define INA219_CAL_VALUE         4096
#define INA219_CURRENT_LSB_MA    0.1f

static bool ina219_write16(ina219_handle_t *h, uint8_t reg, uint16_t value)
{
    uint8_t buf[2] = { (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
    return HAL_I2C_Mem_Write((I2C_HandleTypeDef *)h->hi2c, INA219_I2C_ADDR,
                              reg, I2C_MEMADD_SIZE_8BIT, buf, 2, 10) == HAL_OK;
}

static bool ina219_read16(ina219_handle_t *h, uint8_t reg, uint16_t *value)
{
    uint8_t buf[2] = {0};
    if (HAL_I2C_Mem_Read((I2C_HandleTypeDef *)h->hi2c, INA219_I2C_ADDR,
                          reg, I2C_MEMADD_SIZE_8BIT, buf, 2, 10) != HAL_OK) {
        return false;
    }
    *value = ((uint16_t)buf[0] << 8) | buf[1];
    return true;
}

bool INA219_Init(ina219_handle_t *h, void *hi2c_handle)
{
    memset(h, 0, sizeof(*h));
    h->hi2c = hi2c_handle;
    h->current_lsb_mA = INA219_CURRENT_LSB_MA;

    /* Config: 32V bus range, gain /8 (+-320mV shunt), 12-bit ADC, continuous mode. */
    const uint16_t config = 0x399F;
    if (!ina219_write16(h, INA219_REG_CONFIG, config)) {
        return false;
    }
    return ina219_write16(h, INA219_REG_CALIBRATION, INA219_CAL_VALUE);
}

uint16_t INA219_ReadBusVoltage_mV(ina219_handle_t *h)
{
    uint16_t raw = 0;
    if (!ina219_read16(h, INA219_REG_BUS_VOLTAGE, &raw)) {
        return 0;
    }
    /* Bits [15:3] are the 13-bit voltage reading, LSB = 4mV. Bit 0 = conversion ready,
       bit 1 = math overflow flag; we shift those off. */
    uint16_t mv = (raw >> 3) * 4;
    return mv;
}

int16_t INA219_ReadCurrent_mA(ina219_handle_t *h)
{
    uint16_t raw = 0;
    if (!ina219_read16(h, INA219_REG_CURRENT, &raw)) {
        return 0;
    }
    int16_t signed_raw = (int16_t)raw;
    return (int16_t)((float)signed_raw * h->current_lsb_mA);
}

void INA219_SampleAndDispatch(ina219_handle_t *h)
{
    uint16_t vbus_mV   = INA219_ReadBusVoltage_mV(h);
    int16_t  current_mA = INA219_ReadCurrent_mA(h);

    /* Current should never legitimately be negative on a device-facing port;
       clamp instead of letting a sign flip mask an anomaly downstream. */
    uint16_t current_abs = current_mA < 0 ? (uint16_t)(-current_mA) : (uint16_t)current_mA;

    Guardian_OnCurrentSample(vbus_mV, current_abs);
}
