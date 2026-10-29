#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* 4MZ UART: USART1, 9600 8N1. Change address only to match the real module. */
#define N2_SENSOR_ADDRESS          1U
#define N2_POLL_INTERVAL_MS        1000U
#define N2_RESPONSE_TIMEOUT_MS     500U
#define N2_DATA_STALE_MS           3000U
#define N2_STARTUP_DELAY_MS        200U
#define N2_RX_FRAME_GAP_MS         5U
#define N2_RX_QUEUE_SIZE           128U
#define N2_RX_RECOVERY_MS          50U
#define N2_UART_TX_TIMEOUT_MS      40U
#define N2_DEBUG_UART_ENABLED      1U

/* Wiring: common cathode, segment high = on, digit high = NMOS on. */
#define DISPLAY_BLANK_LOOPS        32U
#define LED_NORMAL_TOGGLE_MS       500U
#define LED_FAULT_TOGGLE_MS        125U

#endif
