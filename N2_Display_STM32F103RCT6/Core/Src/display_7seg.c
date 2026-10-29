#include "display_7seg.h"
#include "main.h"
#include "n2_protocol.h"
#include "app_config.h"

static GPIO_TypeDef * const digit_ports[3] =
{
    DIG1_GPIO_Port, DIG2_GPIO_Port, DIG3_GPIO_Port
};
static const uint16_t digit_pins[3] = {DIG1_Pin, DIG2_Pin, DIG3_Pin};
static GPIO_TypeDef * const segment_ports[8] =
{
    SEG_A_GPIO_Port, SEG_B_GPIO_Port, SEG_C_GPIO_Port, SEG_D_GPIO_Port,
    SEG_E_GPIO_Port, SEG_F_GPIO_Port, SEG_G_GPIO_Port, SEG_DP_GPIO_Port
};
static const uint16_t segment_pins[8] =
{
    SEG_A_Pin, SEG_B_Pin, SEG_C_Pin, SEG_D_Pin,
    SEG_E_Pin, SEG_F_Pin, SEG_G_Pin, SEG_DP_Pin
};

/* Aligned 32-bit publish; the ISR latches a whole frame at the first digit. */
static volatile uint32_t pending_frame;
static uint32_t scanning_frame;
static uint8_t scan_digit;

static void Publish(const uint8_t glyphs[3])
{
    pending_frame = (uint32_t)glyphs[0] |
                    ((uint32_t)glyphs[1] << 8U) |
                    ((uint32_t)glyphs[2] << 16U);
}

void Display_ShowDashes(void)
{
    const uint8_t glyphs[3] = {0x40U, 0x40U, 0x40U};
    Publish(glyphs);
}

void Display_ShowError(void)
{
    const uint8_t glyphs[3] = {0x79U, 0x50U, 0x50U};
    Publish(glyphs);
}

void Display_ShowTenths(uint16_t tenths_vol)
{
    uint8_t glyphs[3];
    N2_FormatTenths(tenths_vol, glyphs);
    Publish(glyphs);
}

void Display_Init(void)
{
    uint8_t i;
    for (i = 0U; i < 3U; ++i)
    {
        HAL_GPIO_WritePin(digit_ports[i], digit_pins[i], GPIO_PIN_RESET);
    }
    for (i = 0U; i < 8U; ++i)
    {
        HAL_GPIO_WritePin(segment_ports[i], segment_pins[i], GPIO_PIN_RESET);
    }
    Display_ShowDashes();
    scanning_frame = pending_frame;
    scan_digit = 0U;
}

void Display_Scan1ms(void)
{
    uint8_t i;
    uint8_t pattern;
    volatile uint32_t blank;
    /* Break before make: all cathode NMOS gates off before changing segments. */
    for (i = 0U; i < 3U; ++i)
    {
        HAL_GPIO_WritePin(digit_ports[i], digit_pins[i], GPIO_PIN_RESET);
    }
    if (scan_digit == 0U)
    {
        scanning_frame = pending_frame;
    }
    pattern = (uint8_t)(scanning_frame >> (8U * scan_digit));
    for (i = 0U; i < 8U; ++i)
    {
        HAL_GPIO_WritePin(segment_ports[i], segment_pins[i],
                          ((pattern & (1U << i)) != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
    /* Small extra blanking interval; verify ghosting with the actual NMOS. */
    for (blank = 0U; blank < DISPLAY_BLANK_LOOPS; ++blank)
    {
        __NOP();
    }
    HAL_GPIO_WritePin(digit_ports[scan_digit], digit_pins[scan_digit], GPIO_PIN_SET);
    scan_digit = (uint8_t)((scan_digit + 1U) % 3U);
}
