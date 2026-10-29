#ifndef APP_N2_H
#define APP_N2_H

#include "n2_protocol.h"

typedef struct
{
    N2_Sample sample;
    N2_Result last_result;
    uint32_t valid_frames;
    uint32_t frame_errors;
    uint32_t timeouts;
    uint32_t uart_errors;
    uint32_t queue_overflows;
} N2_Diagnostics;

/* Watch this symbol in the debugger; it is updated from the main loop. */
extern N2_Diagnostics g_n2_diagnostics;

void App_N2_Init(void);
void App_N2_Task(void);

#endif
