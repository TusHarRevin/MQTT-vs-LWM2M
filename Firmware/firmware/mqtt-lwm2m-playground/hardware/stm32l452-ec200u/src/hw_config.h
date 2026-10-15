#ifndef HW_CONFIG_H
#define HW_CONFIG_H

/*
 * Board settings for NUCLEO-L452RE-P + Quectel EC200U-CN. Edit these, or
 * override any of them with -D at configure time:
 *   cmake ... -DHW_DEFINES="MODEM_APN=\"jionet\";MODEM_PWRKEY_PIN=GPIO_PIN_8"
 *
 * Wiring (same as R:\Revin_ECO\EC200 and R:\LWM2M_Test):
 *   PA9  (D8)  USART1_TX  -> EC200U RXD     (EC200U UART is 1.8 V: level shifter!)
 *   PA10 (D2)  USART1_RX  <- EC200U TXD
 *   PA0  (A0)  PWRKEY     -> EC200U power key circuit (optional)
 *   PA1  (A1)  RESET      -> EC200U reset circuit (optional, not driven)
 *   GND        GND
 *   PA2/PA3    USART2     -> ST-LINK virtual COM port (USB) - log + lab link
 */

/* ---- Cellular ------------------------------------------------------------ */
#ifndef MODEM_APN
#define MODEM_APN "airtelgprs.com"   /* Jio: "jionet", BSNL: "bsnlnet", Vi: "www" */
#endif
#ifndef MODEM_PDP_CONTEXT
#define MODEM_PDP_CONTEXT 1
#endif
#ifndef MODEM_BAUD
#define MODEM_BAUD 115200u
#endif
#ifndef MODEM_REG_TIMEOUT_MS
#define MODEM_REG_TIMEOUT_MS 180000u /* network registration */
#endif

/* PWRKEY: pulsed only if the modem doesn't answer AT at boot. The Revin EC200
 * board drives it directly (low 1.25 s = power on). Undefine
 * MODEM_PWRKEY_PORT if your board powers the modem up by itself. */
#ifndef MODEM_PWRKEY_PORT
#define MODEM_PWRKEY_PORT GPIOA
#endif
#ifndef MODEM_PWRKEY_PIN
#define MODEM_PWRKEY_PIN GPIO_PIN_0
#endif
#ifndef MODEM_PWRKEY_ACTIVE
#define MODEM_PWRKEY_ACTIVE GPIO_PIN_RESET /* level that means "pressed" */
#endif
#ifndef MODEM_PWRKEY_PULSE_MS
#define MODEM_PWRKEY_PULSE_MS 1250u
#endif

/* ---- ST-LINK virtual COM port (USART2) ------------------------------------ */
/* Fast, so event lines never back up into the timed protocol exchanges (one
 * event is ~1 KB: 11 ms at 921600, 100 ms at 115200). Must match the lab's
 * --hw-baud. If your terminal or ST-LINK can't do it, use 115200. */
#ifndef VCP_BAUD
#define VCP_BAUD 921600u
#endif

#endif /* HW_CONFIG_H */
