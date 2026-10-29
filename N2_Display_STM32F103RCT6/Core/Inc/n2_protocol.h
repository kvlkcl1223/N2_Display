#ifndef N2_PROTOCOL_H
#define N2_PROTOCOL_H

#include <stdint.h>

#define N2_REGISTER_COUNT  10U
#define N2_RESPONSE_BYTES  25U
#define N2_GAS_CODE        0x40U
#define N2_UNIT_VOL_CODE   0x04U

typedef enum
{
    N2_RESULT_OK = 0,
    N2_RESULT_WARMUP,
    N2_RESULT_FRAME_ERROR,
    N2_RESULT_EXCEPTION,
    N2_RESULT_WRONG_GAS,
    N2_RESULT_WRONG_UNIT,
    N2_RESULT_BAD_DECIMALS,
    N2_RESULT_SENSOR_ERROR,
    N2_RESULT_OUT_OF_RANGE
} N2_Result;

typedef struct
{
    uint16_t registers[N2_REGISTER_COUNT];
    uint16_t tenths_vol;
    uint8_t gas;
    uint8_t unit;
    uint8_t decimals;
    uint8_t status;
    uint8_t alarm;
    uint8_t exception;
} N2_Sample;

uint16_t N2_Crc16(const uint8_t *data, uint16_t length);
void N2_BuildReadQuery(uint8_t address, uint8_t query[8]);
N2_Result N2_ParseResponse(const uint8_t *frame, uint16_t length,
                          uint8_t address, N2_Sample *sample);
/* Glyph bits: bit0=A ... bit6=G, bit7=DP. No GPIO access in this function. */
void N2_FormatTenths(uint16_t tenths_vol, uint8_t segments[3]);

#endif
