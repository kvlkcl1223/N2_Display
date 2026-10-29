#include "app_n2.h"
#include "app_config.h"
#include "display_7seg.h"
#include "main.h"
#include "tim.h"
#include "usart.h"
#include <stdio.h>

#if (N2_SENSOR_ADDRESS < 1U) || (N2_SENSOR_ADDRESS > 255U)
#error Invalid_4MZ_address
#endif
#if (N2_RX_QUEUE_SIZE < 32U) || (N2_RX_QUEUE_SIZE > 65535U)
#error Invalid_UART_receive_queue_size
#endif

typedef struct
{
    uint32_t received_ms;
    uint8_t byte;
} RxByte;

typedef enum
{
    APP_OFFLINE = 0,
    APP_WARMUP,
    APP_ERROR,
    APP_MEASURING
} AppState;

N2_Diagnostics g_n2_diagnostics;
static volatile RxByte rx_queue[N2_RX_QUEUE_SIZE];
static volatile uint16_t rx_head;
static volatile uint16_t rx_tail;
static volatile uint8_t rx_recovery_needed;
static volatile uint32_t last_rx_ms;
static volatile uint32_t uart_error_count;
static volatile uint32_t overflow_count;
static uint8_t rx_byte;
static uint8_t frame[N2_RESPONSE_BYTES];
static uint8_t frame_length;
static uint8_t waiting_for_response;
static uint8_t query[8];
static uint32_t request_started_ms;
static uint32_t previous_byte_ms;
static uint32_t next_query_ms;
static uint32_t next_recovery_ms;
static uint32_t led_changed_ms;
static uint32_t last_measurement_ms;
static AppState app_state;

static uint8_t TimeReached(uint32_t now, uint32_t deadline)
{
    return ((int32_t)(now - deadline) >= 0) ? 1U : 0U;
}

static void SetState(AppState state)
{
    app_state = state;
    if (state == APP_MEASURING)
    {
        Display_ShowTenths(g_n2_diagnostics.sample.tenths_vol);
    }
    else if (state == APP_ERROR)
    {
        Display_ShowError();
    }
    else
    {
        Display_ShowDashes();
    }
}

static void DebugMessage(const char *message)
{
#if N2_DEBUG_UART_ENABLED
    uint16_t length = 0U;
    while (message[length] != '\0')
    {
        ++length;
    }
    /* Main-loop only. TIM3 and USART1 IRQs continue during this bounded send. */
    (void)HAL_UART_Transmit(&huart2, (uint8_t *)message, length, 60U);
#else
    (void)message;
#endif
}

static void DebugSample(N2_Result result)
{
#if N2_DEBUG_UART_ENABLED
    char line[192];
    const N2_Sample *s = &g_n2_diagnostics.sample;
    int count;
    if (result == N2_RESULT_EXCEPTION)
    {
        count = snprintf(line, sizeof(line), "N2 Modbus exception=0x%02X\r\n",
                         (unsigned int)s->exception);
    }
    else if (result != N2_RESULT_OK)
    {
        count = snprintf(line, sizeof(line),
                         "N2 result=%u gas=0x%02X meta=0x%04X raw=%u status=0x%02X\r\n",
                         (unsigned int)result, (unsigned int)s->gas,
                         (unsigned int)s->registers[0], (unsigned int)s->registers[1],
                         (unsigned int)s->status);
    }
    else
    {
        count = snprintf(line, sizeof(line),
                         "N2 result=%u gas=0x%02X meta=0x%04X raw=%u status=0x%02X "
                         "value=%u.%u %%VOL alarm=%u crc_bad=%lu timeout=%lu\r\n",
                         (unsigned int)result, (unsigned int)s->gas,
                         (unsigned int)s->registers[0], (unsigned int)s->registers[1],
                         (unsigned int)s->status, (unsigned int)(s->tenths_vol / 10U),
                         (unsigned int)(s->tenths_vol % 10U), (unsigned int)s->alarm,
                         (unsigned long)g_n2_diagnostics.frame_errors,
                         (unsigned long)g_n2_diagnostics.timeouts);
    }
    if (count > 0)
    {
        DebugMessage(line);
    }
#else
    (void)result;
#endif
}

static uint8_t PopByte(RxByte *item)
{
    uint16_t tail = rx_tail;
    if (tail == rx_head)
    {
        return 0U;
    }
    __DMB();
    item->received_ms = rx_queue[tail].received_ms;
    item->byte = rx_queue[tail].byte;
    __DMB();
    rx_tail = (uint16_t)((tail + 1U) % N2_RX_QUEUE_SIZE);
    return 1U;
}

static void DropFirstByte(void)
{
    uint8_t i;
    for (i = 1U; i < frame_length; ++i)
    {
        frame[i - 1U] = frame[i];
    }
    if (frame_length != 0U)
    {
        --frame_length;
    }
}

static void ProcessByte(const RxByte *item)
{
    uint8_t expected;
    N2_Result result;
    if (waiting_for_response == 0U)
    {
        frame_length = 0U;
        return;
    }
    /* Use ISR arrival times, so debug UART work cannot create false frame gaps. */
    if ((uint32_t)(item->received_ms - request_started_ms) >= N2_RESPONSE_TIMEOUT_MS)
    {
        frame_length = 0U;
        return;
    }
    if ((frame_length != 0U) &&
        ((uint32_t)(item->received_ms - previous_byte_ms) >= N2_RX_FRAME_GAP_MS))
    {
        frame_length = 0U;
    }
    previous_byte_ms = item->received_ms;
    if (frame_length >= sizeof(frame))
    {
        DropFirstByte();
    }
    frame[frame_length++] = item->byte;
    while (frame_length != 0U)
    {
        if (frame[0] != N2_SENSOR_ADDRESS)
        {
            DropFirstByte();
            continue;
        }
        if (frame_length < 2U)
        {
            return;
        }
        if ((frame[1] != 0x03U) && (frame[1] != 0x83U))
        {
            DropFirstByte();
            continue;
        }
        if (frame_length < 3U)
        {
            return;
        }
        if ((frame[1] == 0x03U) && (frame[2] != 2U * N2_REGISTER_COUNT))
        {
            DropFirstByte();
            continue;
        }
        expected = (frame[1] == 0x83U) ? 5U : N2_RESPONSE_BYTES;
        if (frame_length < expected)
        {
            return;
        }
        result = N2_ParseResponse(frame, expected, N2_SENSOR_ADDRESS,
                                  &g_n2_diagnostics.sample);
        if (result == N2_RESULT_FRAME_ERROR)
        {
            ++g_n2_diagnostics.frame_errors;
            DropFirstByte();
            continue;
        }
        waiting_for_response = 0U;
        frame_length = 0U;
        ++g_n2_diagnostics.valid_frames;
        g_n2_diagnostics.last_result = result;
        if (result == N2_RESULT_OK)
        {
            last_measurement_ms = item->received_ms;
            SetState(APP_MEASURING);
        }
        else if (result == N2_RESULT_WARMUP)
        {
            SetState(APP_WARMUP);
        }
        else
        {
            SetState(APP_ERROR);
        }
        DebugSample(result);
        return;
    }
}

static void RecoverReceiver(uint32_t now)
{
    HAL_StatusTypeDef status;
    /* No DMA is used. AbortReceive is synchronous and has no long wait here. */
    HAL_NVIC_DisableIRQ(USART1_IRQn);
    (void)HAL_UART_AbortReceive(&huart1);
    __HAL_UART_CLEAR_OREFLAG(&huart1);
    rx_tail = rx_head;
    rx_recovery_needed = 0U;
    status = HAL_UART_Receive_IT(&huart1, &rx_byte, 1U);
    if (status != HAL_OK)
    {
        rx_recovery_needed = 1U;
    }
    last_rx_ms = now;
    HAL_NVIC_EnableIRQ(USART1_IRQn);
    frame_length = 0U;
    waiting_for_response = 0U;
    next_recovery_ms = now + N2_RX_RECOVERY_MS;
    g_n2_diagnostics.uart_errors = uart_error_count;
    g_n2_diagnostics.queue_overflows = overflow_count;
    SetState(APP_OFFLINE);
    DebugMessage("N2 UART receive recovery\r\n");
}

static void SendQuery(uint32_t now)
{
    HAL_StatusTypeDef status;
    /* A new transaction never reuses an old partial response or queued bytes. */
    rx_tail = rx_head;
    frame_length = 0U;
    request_started_ms = now;
    waiting_for_response = 1U;
    next_query_ms = now + N2_POLL_INTERVAL_MS;
    status = HAL_UART_Transmit(&huart1, query, sizeof(query), N2_UART_TX_TIMEOUT_MS);
    if (status != HAL_OK)
    {
        waiting_for_response = 0U;
        rx_recovery_needed = 1U;
        ++uart_error_count;
        SetState(APP_OFFLINE);
        DebugMessage("N2 query transmit failed\r\n");
    }
}

void App_N2_Init(void)
{
    uint32_t now = HAL_GetTick();
    rx_head = 0U;
    rx_tail = 0U;
    frame_length = 0U;
    waiting_for_response = 0U;
    last_rx_ms = now;
    led_changed_ms = now;
    next_query_ms = now + N2_STARTUP_DELAY_MS;
    next_recovery_ms = now;
    Display_Init();
    SetState(APP_OFFLINE);
    HAL_GPIO_WritePin(LED_STATUS_GPIO_Port, LED_STATUS_Pin, GPIO_PIN_RESET);
    N2_BuildReadQuery(N2_SENSOR_ADDRESS, query);
    if (HAL_UART_Receive_IT(&huart1, &rx_byte, 1U) != HAL_OK)
    {
        rx_recovery_needed = 1U;
        ++uart_error_count;
    }
    if (HAL_TIM_Base_Start_IT(&htim3) != HAL_OK)
    {
        Error_Handler();
    }
    DebugMessage("N2 display start: HSI 64MHz, USART1 9600 8N1, poll 1000ms\r\n");
}

void App_N2_Task(void)
{
    RxByte item;
    uint32_t now = HAL_GetTick();
    uint32_t led_period;
    if ((rx_recovery_needed != 0U) && (TimeReached(now, next_recovery_ms) != 0U))
    {
        RecoverReceiver(now);
    }
    while (PopByte(&item) != 0U)
    {
        if (rx_recovery_needed != 0U)
        {
            frame_length = 0U;
            SetState(APP_OFFLINE);
            break;
        }
        ProcessByte(&item);
    }
    now = HAL_GetTick();
    if ((app_state == APP_MEASURING) &&
        ((uint32_t)(now - last_measurement_ms) >= N2_DATA_STALE_MS))
    {
        SetState(APP_OFFLINE);
        DebugMessage("N2 measurement stale\r\n");
    }
    if ((waiting_for_response != 0U) &&
        ((uint32_t)(now - request_started_ms) >= N2_RESPONSE_TIMEOUT_MS))
    {
        waiting_for_response = 0U;
        frame_length = 0U;
        ++g_n2_diagnostics.timeouts;
        SetState(APP_OFFLINE);
        DebugMessage("N2 response timeout\r\n");
    }
    if ((waiting_for_response == 0U) && (rx_recovery_needed == 0U) &&
        (TimeReached(now, next_query_ms) != 0U) &&
        ((uint32_t)(now - last_rx_ms) >= N2_RX_FRAME_GAP_MS))
    {
        SendQuery(now);
    }
    led_period = ((app_state == APP_MEASURING) &&
                  (g_n2_diagnostics.sample.alarm == 0U)) ?
                  LED_NORMAL_TOGGLE_MS : LED_FAULT_TOGGLE_MS;
    now = HAL_GetTick();
    if ((uint32_t)(now - led_changed_ms) >= led_period)
    {
        HAL_GPIO_TogglePin(LED_STATUS_GPIO_Port, LED_STATUS_Pin);
        led_changed_ms = now;
    }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3)
    {
        Display_Scan1ms();
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    uint16_t head;
    uint16_t next;
    if (huart->Instance != USART1)
    {
        return;
    }
    last_rx_ms = HAL_GetTick();
    head = rx_head;
    next = (uint16_t)((head + 1U) % N2_RX_QUEUE_SIZE);
    if (next != rx_tail)
    {
        rx_queue[head].received_ms = last_rx_ms;
        rx_queue[head].byte = rx_byte;
        __DMB();
        rx_head = next;
    }
    else
    {
        ++overflow_count;
        rx_recovery_needed = 1U;
    }
    if (HAL_UART_Receive_IT(huart, &rx_byte, 1U) != HAL_OK)
    {
        rx_recovery_needed = 1U;
        ++uart_error_count;
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        ++uart_error_count;
        rx_recovery_needed = 1U;
    }
}
