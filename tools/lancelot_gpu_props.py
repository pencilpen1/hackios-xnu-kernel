"""Strict envelope/TLV decoder for the public cached JM property stream."""
import struct

def decode(raw):
    if len(raw)<16:raise ValueError('Missing binary envelope')
    magic,length,version,reserved=struct.unpack_from('<4I',raw)
    if magic!=0x3150474c or reserved or length!=len(raw)-16 or length>16384:
        raise ValueError('Invalid binary envelope')
    if version&65535!=11:raise ValueError('Unexpected JM ABI')
    data=raw[16:];values={};at=0
    while at<len(data):
        if at+4>len(data):raise ValueError('Truncated property key')
        key=struct.unpack_from('<I',data,at)[0];at+=4
        size=1<<(key&3);identity=key>>2
        if not 0<identity<=128 or identity in values or at+size>len(data):
            raise ValueError('Unknown, duplicate or truncated property')
        values[identity]={'bytes':size,'value':int.from_bytes(data[at:at+size],'little')}
        at+=size
    required=[25,26,27,28,29,30,31,32,33,34,51,52,53,54,55,56,57,58,59,81,83]
    required+=list(range(35,51))
    if any(key not in values for key in required):raise ValueError('Missing required GPU property')
    if values[55]['value']!=0x74021000:raise ValueError('Unexpected GPU ID')
    return data,version,values
