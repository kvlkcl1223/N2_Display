"""Interpret the integer-only protocol C AST with pycparser; no compiler/build.

This is a deliberately small interpreter for the constructs used in
n2_protocol.c. It does not validate ARM timing, HAL, linking, or all C semantics.
Unsupported AST constructs fail instead of being silently ignored.
"""
from pathlib import Path
import hashlib
import json
import re
from pycparser import c_ast, c_parser

ROOT = Path(__file__).resolve().parent.parent

def clean(text):
    text = re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)
    return '\n'.join(x for x in text.splitlines() if not x.lstrip().startswith('#'))

header = (ROOT/'Core/Inc/n2_protocol.h').read_text()
source = (ROOT/'Core/Src/n2_protocol.c').read_text()
macros = dict(re.findall(r'^#define\s+(N2_\w+)\s+(\w+)', header, re.M))
unit = 'typedef unsigned char uint8_t; typedef unsigned short uint16_t; typedef unsigned int uint32_t;\n' + clean(header) + clean(source)
for key,value in macros.items():
    unit = re.sub(r'\b'+key+r'\b', value, unit)
ast = c_parser.CParser().parse(unit)

class Returned(Exception):
    def __init__(self, value): self.value=value
class Broken(Exception): pass

class CSubset:
    def __init__(self, tree):
        self.functions={n.decl.name:n for n in tree.ext if isinstance(n,c_ast.FuncDef)}
        self.constants={}
        for n in tree.ext:
            if isinstance(n,c_ast.Typedef) and isinstance(n.type.type,c_ast.Enum):
                value=0
                for e in n.type.type.values.enumerators:
                    value=self.expr(e.value,{}) if e.value else value
                    self.constants[e.name]=value
                    value+=1

    def call(self, name, args):
        f=self.functions[name]
        env={p.name:a for p,a in zip(f.decl.type.args.params,args)}
        try: self.run(f.body,env)
        except Returned as r: return r.value

    def get(self,node,env):
        if isinstance(node,c_ast.ID): return env[node.name]
        if isinstance(node,c_ast.ArrayRef): return self.expr(node.name,env)[self.expr(node.subscript,env)]
        if isinstance(node,c_ast.StructRef): return self.expr(node.name,env)[node.field.name]
        raise TypeError(type(node).__name__)

    def put(self,node,value,env):
        if isinstance(node,c_ast.ID): env[node.name]=value
        elif isinstance(node,c_ast.ArrayRef): self.expr(node.name,env)[self.expr(node.subscript,env)]=value
        elif isinstance(node,c_ast.StructRef): self.expr(node.name,env)[node.field.name]=value
        else: raise TypeError(type(node).__name__)

    def binary(self,op,a,b):
        if op=='+':return a+b
        if op=='-':return a-b
        if op=='*':return a*b
        if op=='/':return a//b
        if op=='%':return a%b
        if op=='^':return a^b
        if op=='|':return a|b
        if op=='&':return a&b
        if op=='>>':return a>>b
        if op=='<<':return a<<b
        if op=='==':return int(a==b)
        if op=='!=':return int(a!=b)
        if op=='<':return int(a<b)
        if op=='>':return int(a>b)
        if op=='<=':return int(a<=b)
        if op=='>=':return int(a>=b)
        raise ValueError(op)

    def expr(self,n,e):
        if n is None:return 0
        if isinstance(n,c_ast.Constant):return int(re.sub(r'[uUlL]+$','',n.value),0)
        if isinstance(n,c_ast.ID):return e[n.name] if n.name in e else self.constants[n.name]
        if isinstance(n,(c_ast.ArrayRef,c_ast.StructRef)):return self.get(n,e)
        if isinstance(n,c_ast.BinaryOp):
            a=self.expr(n.left,e)
            if n.op=='&&':return int(bool(a) and bool(self.expr(n.right,e)))
            if n.op=='||':return int(bool(a) or bool(self.expr(n.right,e)))
            return self.binary(n.op,a,self.expr(n.right,e))
        if isinstance(n,c_ast.TernaryOp):return self.expr(n.iftrue if self.expr(n.cond,e) else n.iffalse,e)
        if isinstance(n,c_ast.Cast):
            typename=n.to_type.type.type.names[0]
            masks={'uint8_t':255,'uint16_t':65535,'uint32_t':4294967295}
            return self.expr(n.expr,e)&masks[typename]
        if isinstance(n,c_ast.UnaryOp):
            if n.op in ['p++','++','p--','--']:
                old=self.get(n.expr,e);value=old+(1 if '+' in n.op else -1)
                self.put(n.expr,value,e)
                return old if n.op.startswith('p') else value
            a=self.expr(n.expr,e)
            if n.op=='!':return int(not a)
            if n.op=='-':return -a
            if n.op=='~':return ~a
            raise ValueError(n.op)
        if isinstance(n,c_ast.FuncCall):return self.call(n.name.name,[self.expr(x,e) for x in n.args.exprs])
        raise TypeError(type(n).__name__)

    def run(self,n,e):
        if n is None:return
        if isinstance(n,c_ast.Compound):
            for x in n.block_items or []:self.run(x,e)
        elif isinstance(n,c_ast.Decl):
            e[n.name]=[self.expr(x,e) for x in n.init.exprs] if isinstance(n.init,c_ast.InitList) else self.expr(n.init,e)
        elif isinstance(n,c_ast.Assignment):
            v=self.expr(n.rvalue,e)
            if n.op!='=':v=self.binary(n.op[:-1],self.get(n.lvalue,e),v)
            self.put(n.lvalue,v,e)
        elif isinstance(n,c_ast.If):self.run(n.iftrue if self.expr(n.cond,e) else n.iffalse,e)
        elif isinstance(n,c_ast.For):
            self.run(n.init,e)
            count=0
            while self.expr(n.cond,e):
                count+=1
                if count>100000:raise RuntimeError('Loop limit')
                try:self.run(n.stmt,e)
                except Broken:break
                self.run(n.next,e)
        elif isinstance(n,c_ast.Switch):
            val=self.expr(n.cond,e);items=n.stmt.block_items
            start=next((i for i,x in enumerate(items) if isinstance(x,c_ast.Case) and self.expr(x.expr,e)==val),None)
            if start is None:start=next((i for i,x in enumerate(items) if isinstance(x,c_ast.Default)),None)
            if start is not None:
                try:
                    for c in items[start:]:
                        for stmt in c.stmts or []:self.run(stmt,e)
                except Broken:pass
        elif isinstance(n,c_ast.Break):raise Broken()
        elif isinstance(n,c_ast.Return):raise Returned(self.expr(n.expr,e))
        elif isinstance(n,(c_ast.UnaryOp,c_ast.FuncCall)):self.expr(n,e)
        else:raise TypeError(type(n).__name__)

vm=CSubset(ast)

def reference_crc(data):
    c=0xffff
    for b in data:
        c^=b
        for _ in range(8):c=(c>>1)^0xa001 if c&1 else c>>1
    return c

def frame(raw=781,meta=0x4400,status=1,gas=0x40):
    regs=[meta,raw,0,0,1000,status,0,0,gas<<8,0]
    b=[1,3,20]+[x for r in regs for x in [r>>8,r&255]]
    crc=reference_crc(b)
    return b+[crc&255,crc>>8]

def sample():
    return dict(registers=[0]*10,tenths_vol=0,gas=0,unit=0,decimals=0,status=0,alarm=0,exception=0)

checks=[]
query=[0]*8
vm.call('N2_BuildReadQuery',[1,query])
assert query==list(bytes.fromhex('01 03 00 00 00 0A C5 CD'))
checks.append('fixed_modbus_query_crc')
for n in range(40):
    b=list(range(n))
    assert vm.call('N2_Crc16',[b,len(b)])==reference_crc(b)
checks.append('crc_lengths_0_to_39')

cases=[
 ('78.1',dict(),0,781),
 ('0.0',dict(raw=0),0,0),
 ('9.9',dict(raw=99),0,99),
 ('99.9',dict(raw=999),0,999),
 ('100',dict(raw=1000),0,1000),
 ('integer_78_to_78.0',dict(raw=78,meta=0x4000),0,780),
 ('99.94',dict(raw=9994,meta=0x4800),0,999),
 ('99.95_rounds_to_100',dict(raw=9995,meta=0x4800),0,1000),
 ('three_decimals_12.345',dict(raw=12345,meta=0x4c00),0,123),
 ('more_than_100',dict(raw=1001),8,None),
 ('warmup',dict(status=0),1,None),
 ('sensor_fault',dict(status=3),7,None),
 ('wrong_gas_oxygen',dict(gas=67),4,None),
 ('wrong_unit_ppm',dict(meta=0x0400),5,None),
 ('unknown_decimal_code',dict(meta=0x4100),6,None),
 ('low_alarm_still_numeric',dict(status=5),0,781),
]
vectors=[]
for name,kwargs,expected,tenths in cases:
    f=frame(**kwargs);s=sample()
    result=vm.call('N2_ParseResponse',[f,len(f),1,s])
    assert result==expected,(name,result,expected)
    if tenths is not None:assert s['tenths_vol']==tenths,(name,s)
    if kwargs.get('status')==5:assert s['alarm']==1
    vectors.append({'name':name,'frame_hex':bytes(f).hex(' ').upper(),'result':expected,'tenths':tenths})
    checks.append(name)
f=frame();f[6]^=1
assert vm.call('N2_ParseResponse',[f,25,1,sample()])==2
assert vm.call('N2_ParseResponse',[frame(),24,1,sample()])==2
assert vm.call('N2_ParseResponse',[frame(),25,2,sample()])==2
assert vm.call('N2_ParseResponse',[0,25,1,sample()])==2
checks.extend(['bad_crc','truncated_frame','wrong_address','null_frame'])
exc=[1,0x83,2];crc=reference_crc(exc);exc+=[crc&255,crc>>8]
s=sample()
assert vm.call('N2_ParseResponse',[exc,5,1,s])==3 and s['exception']==2
checks.append('modbus_exception')

digits=[0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f]
for tenths in range(1001):
    out=[0,0,0];vm.call('N2_FormatTenths',[tenths,out])
    if tenths==1000:expected=[6,63,63]
    else:expected=[digits[tenths//100] if tenths>=100 else 0,digits[tenths//10%10]|128,digits[tenths%10]]
    assert out==expected,(tenths,out,expected)
out=[0,0,0];vm.call('N2_FormatTenths',[1001,out]);assert out==[0x79,0x50,0x50]
checks.append('all_1001_display_values_and_overflow')
result={'all_passed':True,'checks':checks,'display_values_checked':1001,
        'method':'pycparser AST interpretation of actual n2_protocol.c, using the documented integer-only subset; no compiler invoked',
        'not_tested':['ARM execution','HAL UART ISR timing','main-loop state machine on target','linking','electrical wiring'],
        'source_sha256':hashlib.sha256(source.encode()).hexdigest()}
(ROOT/'validation/application_protocol_checks.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
(ROOT/'validation/sensor_test_vectors.json').write_text(json.dumps(vectors,indent=2),encoding='utf-8')
print(json.dumps(result,indent=2))
