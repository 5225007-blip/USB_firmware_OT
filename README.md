# USB_firmware_OT
a usb guardian for your Personal machine safeguard
# USB Guardian OT — Firmware

STM32F407VGT6 firmware implementing the three defenses from the briefing:
**hardware fingerprinting**, **voltage crowbar protection**, and the
**"inspect first, connect second"** relay gate. (Data-block tokenization —
the HMAC-per-512-byte-block feature — lives in the software daemon on the
OT gateway, not here, since it operates on the mass-storage data path
after the relay has already closed.)

## Design principle

The relay closes in **exactly one place** in the whole codebase:
`Guardian_OnDescriptorsRead()` / `Guardian_ApproveDevice()` in
`usb_guardian.c`, and only after the class whitelist check and the
fingerprint/approval check both pass. Every other path — rejection,
crowbar trip, tamper, remote kill — opens the relay and never closes it.
If you extend this firmware, preserve that invariant; it's the entire
security property of the device.

## Files

| File | Purpose | Maps to BOM part |
|---|---|---|
| `usb_guardian.h/.c` | State machine core: whitelist, fingerprinting, HID rate check, orchestration | STM32F407VGT6 |
| `relay_control.h/.c` | Relay + SCR crowbar GPIO driver | G5V-1 relay |
| `ina219.h/.c` | VBUS current/voltage sensing over I2C | Adafruit INA219 |
| `sha256.h/.c` | Software SHA-256 for salted device fingerprints | — (F407 has no crypto accelerator) |
| `can_alert.h/.c` | OT-network alert frames | SN65HVD230 CAN transceiver |
| `main.c` | Example wiring into a CubeMX-generated project | — |

Not included (out of scope for firmware): the ADuM3160 USB isolator and
ISO7241 digital isolator are purely analog/signal-path components with no
firmware footprint — they isolate the untrusted port electrically so the
USB host stack can enumerate a device for *inspection* without exposing
the real host until `Relay_Close()` fires. The TPD4E05U06 ESD array is
passive protection, also with no firmware footprint.

## Integrating into a real project

1. Generate a CubeMX project for **STM32F407VGT6** with:
   - I2C1 (or whichever bus the INA219 is wired to)
   - CAN1 + the SN65HVD230 on your chosen pins
   - USB Host (Full-Speed, using the on-chip OTG_FS or OTG_HS peripheral)
     pointed at the **upstream/untrusted** port only
   - A timer (e.g. TIM6) at a period matching `GUARD_VBUS_SAMPLE_PERIOD_US` (50µs)
   - A GPIO output for the relay coil driver, a GPIO output for the crowbar
     SCR gate, and an EXTI input for the housing tamper switch
2. Drop `Core/Inc/*.h` and `Core/Src/*.c` from this package into the
   generated project's `Core/Inc` and `Core/Src`.
3. Update the pin/port `#define`s at the top of `relay_control.h` to match
   your actual CubeMX pin assignment.
4. Wire the callbacks shown in `main.c`:
   - `HAL_TIM_PeriodElapsedCallback` → `INA219_SampleAndDispatch()`
   - `HAL_GPIO_EXTI_Callback` (tamper pin) → `Guardian_OnTamperTriggered()`
   - Your USBH class driver's descriptor-ready hook → `App_OnUsbDeviceEnumerated()`
   - Your USBH HID report callback → `App_OnHidKeystroke()`
5. Replace the hardcoded `Guardian_LoadWhitelist()` example with a real
   policy load (flash-stored table, or synced from the software daemon
   over USB-CDC/UART at boot).
6. Build in STM32CubeIDE (or your Makefile/CMake toolchain of choice) —
   these files are plain HAL C99 with no other external dependencies.

## What's been verified so far

The portable logic (SHA-256 correctness against the NIST "abc" test
vector, and the full state machine: whitelist rejection, unknown-device
pending-approval flow, overvoltage → crowbar trip, and reset) was
exercised in an off-target harness with stubbed HAL calls. The
HAL-dependent I/O (actual I2C timing, actual USB host enumeration, actual
GPIO drive strength into the G5V-1 coil) can only be verified on real
hardware — bench-test the crowbar path especially, with a current-limited
bench supply before trusting it near a live USB Killer test.
