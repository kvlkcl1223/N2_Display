"""Grammar/project checks only. Does not invoke a C compiler or linker."""
from pathlib import Path
import hashlib
import json
import re
import xml.etree.ElementTree as ET
from pycparser import c_ast,c_parser

ROOT=Path(__file__).resolve().parent.parent
def clean(s):
    s=re.sub(r'/\*.*?\*/|//[^\n]*','',s,flags=re.S)
    return '\n'.join(l for l in s.splitlines() if not l.lstrip().startswith('#'))

prefix='''
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned int HAL_StatusTypeDef;
typedef struct { int Instance; } UART_HandleTypeDef;
typedef struct { int Instance; } TIM_HandleTypeDef;
typedef struct { int dummy; } GPIO_TypeDef;
'''
for h in ['n2_protocol.h','display_7seg.h','app_n2.h']:
    prefix+=clean((ROOT/'Core/Inc'/h).read_text())+'\n'

trees={}
for name in ['n2_protocol.c','display_7seg.c','app_n2.c']:
    trees[name]=c_parser.CParser().parse(prefix+clean((ROOT/'Core/Src'/name).read_text()),filename=name)

class Calls(c_ast.NodeVisitor):
    def __init__(self):self.names=set()
    def visit_FuncCall(self,node):
        assert isinstance(node.name,c_ast.ID),'Unexpected indirect call'
        self.names.add(node.name.name)
        self.generic_visit(node)

functions={n.decl.name for t in trees.values() for n in t.ext if isinstance(n,c_ast.FuncDef)}
allowed={'HAL_GetTick','HAL_GPIO_WritePin','HAL_GPIO_TogglePin','HAL_UART_Transmit',
         'HAL_UART_Receive_IT','HAL_UART_AbortReceive','HAL_NVIC_DisableIRQ',
         'HAL_NVIC_EnableIRQ','HAL_TIM_Base_Start_IT','Error_Handler',
         '__NOP','__DMB','__HAL_UART_CLEAR_OREFLAG','snprintf'}
called=set()
for tree in trees.values():
    v=Calls();v.visit(tree);called|=v.names
assert not (called-functions-allowed),called-functions-allowed

project=ROOT/'MDK-ARM/N2_Display.uvprojx'
xml=ET.parse(project)
paths=[x.text for x in xml.findall('.//FilePath') if x.text]
checks={
 'application_C_grammar_parsed':len(trees)==3,
 'application_function_calls_resolved':not(called-functions-allowed),
 'keil_all_sources_exist':all((project.parent/p.replace('\\','/')).is_file() for p in paths),
 'keil_no_duplicate_source_paths':len({str((project.parent/p.replace('\\','/')).resolve()).lower() for p in paths})==len(paths),
 'custom_sources_in_project':all(any(p.endswith('/'+n) or p.endswith('\\'+n) for p in paths) for n in trees),
 'user_code_hooks':all(s in (ROOT/'Core/Src/main.c').read_text() for s in ['#include "app_n2.h"','App_N2_Init();','App_N2_Task();']),
 'stack_ioc_2048':'ProjectManager.StackSize=0x800' in (ROOT/'N2_Display.ioc').read_text(),
 'stack_startup_2048':bool(re.search(r'Stack_Size\s+EQU\s+0x800\b',(ROOT/'MDK-ARM/startup_stm32f103xe.s').read_text())),
 'hal_weak_callbacks_enabled':all(re.search(r'#define\s+USE_HAL_'+periph+r'_REGISTER_CALLBACKS\s+0U',(ROOT/'Core/Inc/stm32f1xx_hal_conf.h').read_text()) for periph in ['TIM','UART']),
}
for name in ['HAL_TIM_PeriodElapsedCallback','HAL_UART_RxCpltCallback','HAL_UART_ErrorCallback']:
    count=sum(len(re.findall(r'\bvoid\s+'+name+r'\s*\(',p.read_text())) for p in (ROOT/'Core/Src').glob('*.c'))
    checks[name+'_single_definition']=count==1
for irq,handle in [('TIM3','htim3'),('USART1','huart1')]:
    handler='HAL_TIM_IRQHandler' if irq=='TIM3' else 'HAL_UART_IRQHandler'
    checks[irq+'_HAL_irq_dispatch']=handler+'(&'+handle+');' in (ROOT/'Core/Src/stm32f1xx_it.c').read_text()

report={'all_passed':all(checks.values()),'checks':checks,
 'scope':'C grammar parsing with HAL type placeholders, call-name resolution, source paths and hooks only; no compiler, linker or hardware test',
 'source_sha256':{n:hashlib.sha256((ROOT/'Core/Src'/n).read_bytes()).hexdigest() for n in trees}}
(ROOT/'validation/application_static_checks.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report,indent=2))
assert report['all_passed']
