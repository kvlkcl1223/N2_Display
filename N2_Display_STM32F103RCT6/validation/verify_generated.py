from pathlib import Path
import hashlib
import json
import re
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parent.parent
src = root / 'Core/Src'
inc = root / 'Core/Inc'
read = lambda p: p.read_text(encoding='utf-8', errors='replace')
ioc = read(root/'N2_Display.ioc')
main, gpio, tim, uart, irq, msp = [read(src/name) for name in
    ['main.c','gpio.c','tim.c','usart.c','stm32f1xx_it.c','stm32f1xx_hal_msp.c']]
header = read(inc/'main.h')
checks = {}
checks['mcu_rct6_lqfp64'] = 'Mcu.CPN=STM32F103RCT6' in ioc and 'Mcu.Package=LQFP64' in ioc
checks['hsi_64mhz_no_hse'] = all(s in main for s in [
    'OscillatorType = RCC_OSCILLATORTYPE_HSI;', 'HSIState = RCC_HSI_ON;',
    'PLLSource = RCC_PLLSOURCE_HSI_DIV2;', 'PLLMUL = RCC_PLL_MUL16;',
    'SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;', 'AHBCLKDivider = RCC_SYSCLK_DIV1;',
    'APB1CLKDivider = RCC_HCLK_DIV2;', 'APB2CLKDivider = RCC_HCLK_DIV1;',
    'FLASH_LATENCY_2']) and 'RCC_PLLSOURCE_HSE' not in main and 'HSEState' not in main
checks['ioc_64mhz'] = 'RCC.SYSCLKFreq_VALUE=64000000' in ioc
pins = {'DIG1':('GPIOA',0),'DIG2':('GPIOA',1),'DIG3':('GPIOA',4),
        'LED_STATUS':('GPIOD',2)}
pins.update({'SEG_'+name:('GPIOB',i+8) for i,name in enumerate(['A','B','C','D','E','F','G','DP'])})
for name,(port,pin) in pins.items():
    checks[name] = bool(re.search(r'#define '+name+r'_Pin\s+GPIO_PIN_'+str(pin)+r'\b',header)) and bool(re.search(r'#define '+name+r'_GPIO_Port\s+'+port+r'\b',header))
checks['led_pp_low'] = all(s in gpio for s in [
    'HAL_GPIO_WritePin(LED_STATUS_GPIO_Port, LED_STATUS_Pin, GPIO_PIN_RESET);',
    'GPIO_InitStruct.Pin = LED_STATUS_Pin;',
    'GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;',
    'HAL_GPIO_Init(LED_STATUS_GPIO_Port, &GPIO_InitStruct);'])
checks['tim3_1ms'] = all(s in tim for s in [
    'Prescaler = 63;', 'Period = 999;', 'TIM_CLOCKSOURCE_INTERNAL',
    'HAL_NVIC_SetPriority(TIM3_IRQn, 1, 0);', 'HAL_NVIC_EnableIRQ(TIM3_IRQn);']) and 'HAL_TIM_IRQHandler(&htim3);' in irq
for n,baud in [(1,9600),(2,115200)]:
    checks[f'USART{n}_{baud}_8N1'] = all(f'huart{n}.Init.'+s in uart for s in [
        f'BaudRate = {baud};', 'WordLength = UART_WORDLENGTH_8B;',
        'StopBits = UART_STOPBITS_1;', 'Parity = UART_PARITY_NONE;',
        'Mode = UART_MODE_TX_RX;', 'HwFlowCtl = UART_HWCONTROL_NONE;']) and f'HAL_UART_IRQHandler(&huart{n});' in irq
checks['swd'] = '__HAL_AFIO_REMAP_SWJ_NOJTAG();' in msp
checks['no_external_osc_pins'] = 'PD0-OSC_IN.Signal=' not in ioc and 'PD1-OSC_OUT.Signal=' not in ioc
checks['generation_no_auto_build'] = 'ProjectManager.ProjectBuild=false' in ioc

project = root/'MDK-ARM/N2_Display.uvprojx'
xml = ET.parse(project)
checks['keil_device'] = xml.findtext('.//Device') == 'STM32F103RC'
cpu = xml.findtext('.//Cpu') or ''
checks['keil_256k_flash_48k_ram'] = 'IRAM(0x20000000-0x2000BFFF)' in cpu and 'IROM(0x8000000-0x803FFFF)' in cpu
checks['keil_clock_metadata_64mhz'] = 'CLOCK(64000000)' in cpu
bad_files=[]
for p in xml.findall('.//FilePath'):
    if not (project.parent / (p.text or '').replace('\\','/')).is_file():
        bad_files.append(p.text)
checks['keil_source_files_exist'] = not bad_files
bad_includes=[]
for e in xml.findall('.//IncludePath'):
    for p in (e.text or '').split(';'):
        if p and not (project.parent/p.replace('\\','/')).is_dir():
            bad_includes.append(p)
checks['keil_include_paths_exist'] = not bad_includes
report = {
    'checks':checks, 'all_passed':all(checks.values()),
    'bad_source_paths':bad_files, 'bad_include_paths':bad_includes,
    'validation':'CubeMX project generation and static inspection only; compiler not invoked; no hardware test',
    'ioc_sha256':hashlib.sha256((root/'N2_Display.ioc').read_bytes()).hexdigest(),
}
(root/'validation/verification.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report,indent=2))
assert report['all_passed'], 'Generated-project verification failed'
