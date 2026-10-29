#include "n2_protocol.h"

uint16_t N2_Crc16(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0xFFFFU;
    uint16_t i;
    uint8_t bit;
    for (i = 0U; i < length; ++i)
    {
        crc ^= data[i];
        for (bit = 0U; bit < 8U; ++bit)
        {
            if ((crc & 1U) != 0U)
            {
                crc = (uint16_t)((crc >> 1U) ^ 0xA001U);
            }
            else
            {
                crc >>= 1U;
            }
        }
    }
    return crc;
}

void N2_BuildReadQuery(uint8_t address, uint8_t query[8])
{
    uint16_t crc;
    query[0] = address;
    query[1] = 0x03U;
    query[2] = 0x00U;
    query[3] = 0x00U;
    query[4] = 0x00U;
    query[5] = N2_REGISTER_COUNT;
    crc = N2_Crc16(query, 6U);
    query[6] = (uint8_t)crc;
    query[7] = (uint8_t)(crc >> 8U);
}

N2_Result N2_ParseResponse(const uint8_t *frame, uint16_t length,
                          uint8_t address, N2_Sample *sample)
{
    uint16_t crc;
    uint8_t i;
    uint8_t code;
    uint32_t scale;
    uint32_t raw;

    /* Do not publish data before the frame length, address and CRC are valid. */
    if ((frame == 0) || (sample == 0) || (length < 5U) ||
        (frame[0] != address))
    {
        return N2_RESULT_FRAME_ERROR;
    }
    if (!(((frame[1] == 0x03U) && (length == N2_RESPONSE_BYTES) &&
           (frame[2] == 2U * N2_REGISTER_COUNT)) ||
          ((frame[1] == 0x83U) && (length == 5U))))
    {
        return N2_RESULT_FRAME_ERROR;
    }
    crc = N2_Crc16(frame, (uint16_t)(length - 2U));
    if ((frame[length - 2U] != (uint8_t)crc) ||
        (frame[length - 1U] != (uint8_t)(crc >> 8U)))
    {
        return N2_RESULT_FRAME_ERROR;
    }

    sample->exception = 0U;
    sample->alarm = 0U;
    sample->tenths_vol = 0U;
    sample->decimals = 0U;
    if (frame[1] == 0x83U)
    {
        sample->exception = frame[2];
        return N2_RESULT_EXCEPTION;
    }
    for (i = 0U; i < N2_REGISTER_COUNT; ++i)
    {
        sample->registers[i] = (uint16_t)(((uint16_t)frame[3U + 2U * i] << 8U) |
                                         frame[4U + 2U * i]);
    }
    sample->unit = (uint8_t)((sample->registers[0] >> 12U) & 0x0FU);
    sample->gas = (uint8_t)(sample->registers[8] >> 8U);
    sample->status = (uint8_t)sample->registers[5];

    /* A module in warmup may not yet have valid measurement metadata. */
    if (sample->status == 0x00U)
    {
        return N2_RESULT_WARMUP;
    }
    if (sample->gas != N2_GAS_CODE)
    {
        return N2_RESULT_WRONG_GAS;
    }
    if (sample->unit != N2_UNIT_VOL_CODE)
    {
        return N2_RESULT_WRONG_UNIT;
    }

    /* Manual p9: the nibble codes 0/4/8/C mean 0/1/2/3 decimal places. */
    code = (uint8_t)((sample->registers[0] >> 8U) & 0x0FU);
    switch (code)
    {
        case 0x00U: sample->decimals = 0U; scale = 1U; break;
        case 0x04U: sample->decimals = 1U; scale = 10U; break;
        case 0x08U: sample->decimals = 2U; scale = 100U; break;
        case 0x0CU: sample->decimals = 3U; scale = 1000U; break;
        default: return N2_RESULT_BAD_DECIMALS;
    }

    switch (sample->status)
    {
        case 0x01U: break;
        case 0x04U:
        case 0x05U:
        case 0x06U:
        case 0x0BU:
        case 0x0CU:
            sample->alarm = 1U; /* Measurement is still displayed during an alarm. */
            break;
        default: return N2_RESULT_SENSOR_ERROR;
    }
    raw = sample->registers[1];
    if (raw > (100U * scale))
    {
        return N2_RESULT_OUT_OF_RANGE;
    }
    sample->tenths_vol = (uint16_t)((raw * 10U + scale / 2U) / scale);
    return N2_RESULT_OK;
}

void N2_FormatTenths(uint16_t tenths_vol, uint8_t segments[3])
{
    static const uint8_t digits[10] =
    {
        0x3FU, 0x06U, 0x5BU, 0x4FU, 0x66U,
        0x6DU, 0x7DU, 0x07U, 0x7FU, 0x6FU
    };
    if (tenths_vol > 1000U)
    {
        segments[0] = 0x79U; /* E */
        segments[1] = 0x50U; /* r */
        segments[2] = 0x50U;
    }
    else if (tenths_vol == 1000U)
    {
        segments[0] = digits[1];
        segments[1] = digits[0];
        segments[2] = digits[0];
    }
    else
    {
        segments[0] = (tenths_vol >= 100U) ? digits[tenths_vol / 100U] : 0U;
        segments[1] = (uint8_t)(digits[(tenths_vol / 10U) % 10U] | 0x80U);
        segments[2] = digits[tenths_vol % 10U];
    }
}
