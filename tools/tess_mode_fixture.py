"""Relocate modes in owned witness SPIR-V, not runtime shader rewriting."""
import struct

def decode(data):
    if len(data)<20 or len(data)%4:raise ValueError('SPIR-V size')
    words=list(struct.unpack('<'+'I'*(len(data)//4),data))
    if words[0]!=0x07230203:raise ValueError('SPIR-V magic')
    ops=[];i=5
    while i<len(words):
        n=words[i]>>16
        if n==0 or i+n>len(words):raise ValueError('SPIR-V instruction size')
        ops.append(words[i:i+n]);i+=n
    return words[:5],ops

def encode(module):
    header,ops=module;words=header+[x for op in ops for x in op]
    return struct.pack('<'+'I'*len(words),*words)

def swap_owned_modes(control,evaluation):
    """TES domain/spacing/winding -> TCS; TCS OutputVertices -> TES."""
    c,e=decode(control),decode(evaluation)
    def entry(module,model):
        entries=[op for op in module[1] if op[0]&65535==15]
        if len(entries)!=1 or entries[0][1]!=model:raise ValueError('owned entrypoint expected')
        return entries[0][2]
    cid,eid=entry(c,1),entry(e,2)
    def move(source,target,modes,target_id,expected):
        selected=[op[:] for op in source[1] if op[0]&65535==16 and op[2] in modes]
        if len(selected)!=expected:raise ValueError('unexpected owned mode count')
        for op in selected:op[1]=target_id
        source[1][:]=[op for op in source[1] if not(op[0]&65535==16 and op[2] in modes)]
        insertion=next(i for i,op in enumerate(target[1]) if op[0]&65535==15)+1
        target[1][insertion:insertion]=selected
    move(c,e,{26},eid,1)
    move(e,c,{1,2,3,4,5,22,24,25},cid,3)
    return encode(c),encode(e)
