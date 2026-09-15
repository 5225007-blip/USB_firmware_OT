/**
 * @file    ina219.h
 * @brief   Minimal driver for the Adafruit INA219 current/voltage sensor,
 *          used to watch VBUS for charge-pump spikes and USB Killer pulses.
 */

#ifndef INA219_H
#define INA219_H

#include <stdint.h>
#include <stdbool.h>

#define INA219_I2C_ADDR          (0x40 << 1)  /* 7-bit 0x40, shifted for HAL */

/* Register map */
#define INA219_REG_CONFIG        0x00
#define INA219_REG_SHUNT_VOLTAGE 0x01
#define INA219_REG_BUS_VOLTAGE   0x02
#define INA219_REG_POWER         0x03
#define INA219_REG_CURRENT       0x04
#define INA219_REG_CALIBRATION   0x05

typedef struct {
    void   *hi2c;             /* I2C_HandleTypeDef* - opaque here to avoid HAL header coupling */
    float   current_lsb_mA;   /* set by INA219_Calibrate() */
} ina219_handle_t;

/**
 * Initialize and calibrate for a 0.1 ohm shunt, +-3.2A range (matches the
 * VBUS current levels expected on a USB2/USB3 port, including PD negotiated
 * loads). Returns true on success (device ACKs on the bus).
 */
bool INA219_Init(ina219_handle_t *h, void *hi2c_handle);

/** Bus voltage in millivolts (this is VBUS, i.e. what the crowbar threshold compares against). */
uint16_t INA219_ReadBusVoltage_mV(ina219_handle_t *h);

/** Current draw in milliamps. */
int16_t INA219_ReadCurrent_mA(ina219_handle_t *h);

/**
 * High-rate sampling entry point intended to be called from a timer ISR
 * every GUARD_VBUS_SAMPLE_PERIOD_US. Reads both voltage and current in one
 * pass and forwards them to Guardian_OnCurrentSample().
 */
void INA219_SampleAndDispatch(ina219_handle_t *h);

#endif /* INA219_H */
