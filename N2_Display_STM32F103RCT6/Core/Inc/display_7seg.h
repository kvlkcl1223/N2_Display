#ifndef DISPLAY_7SEG_H
#define DISPLAY_7SEG_H

#include <stdint.h>

void Display_Init(void);
void Display_ShowTenths(uint16_t tenths_vol);
void Display_ShowDashes(void);
void Display_ShowError(void);
void Display_Scan1ms(void);

#endif
