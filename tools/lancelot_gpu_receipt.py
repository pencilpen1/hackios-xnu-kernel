"""Decode fixed same-boot observation words; never treat them as an ownership API."""
import re
import struct
import zlib

HEADER=re.compile(r'lancelot-gpu-receipt: source=(UEFI|XNU) boot=((?:0x)?[a-f0-9]+) pool=((?:0x)?[a-f0-9]+) sequence=(\d+) fault=((?:0x)?[a-f0-9]+) size=(\d+) crc=((?:0x)?[a-f0-9]+) verified=1(?: version=(\d+))?')
WORD=re.compile(r'lancelot-gpu-receipt: word=(\d+) value=((?:0x)?[a-f0-9]+)')
def receipt_lines(text):
    """The capture reader may inject a [capture] note in the middle of a line.
    Drop those notes and rejoin a split receipt line; CRC still gates the frame."""
    lines=[line for line in text.splitlines() if not line.lstrip().startswith('[capture]')]
    merged=[]
    suffixes=('word=','value=','source=','boot=','pool=','sequence=','fault=','size=','crc=','verified=','0x')
    for line in lines:
        if merged and merged[-1].startswith('lancelot-gpu-receipt:') and \
           not HEADER.search(merged[-1]) and not WORD.search(merged[-1]) and \
           line.startswith(suffixes):
            merged[-1]+=line
        else:
            merged.append(line)
    return merged
def decode(text,expected_boot):
    frames=[];current=None;incomplete=0
    for line in receipt_lines(text):
        header=HEADER.search(line)
        if header:
            if current and len(current['words'])<96:incomplete+=1
            source,boot,pool,sequence,fault,size,crc,version=header.groups()
            current={'source':source,'boot':int(boot,16),'pool':int(pool,16),
                'sequence':int(sequence),'fault':int(fault,16),'size':int(size),
                'crc':int(crc,16),'version':int(version or 1),'words':{}}
            if current['size']!=432 or not expected_boot or current['boot']!=expected_boot or current['version'] not in (1,2,3,4):
                raise ValueError('Receipt size/boot/version mismatch')
        elif current and (word:=WORD.search(line)):
            index,value=word.groups();index=int(index);value=int(value,16)
            if not 0<=index<96 or index in current['words'] or value>0xffffffff:
                raise ValueError('Receipt word range/duplicate')
            current['words'][index]=value
            if len(current['words'])==96:
                words=[current['words'][i] for i in range(96)]
                packed=struct.pack('<4I4Q96I',0x5244474c,current['version'],432,0,current['boot'],current['pool'],
                    current['sequence'],current['fault'],*words)
                if zlib.crc32(packed)!=current['crc']:raise ValueError('Receipt CRC mismatch')
                booleans=[6,7,8,9,10,11,12,14,22,23,25]
                if current['version']>=2:
                    booleans += [80,81]
                    if words[74]>7 or words[82]&~31:raise ValueError('Receipt diagnostic fields')
                if current['version']>=3:
                    booleans += [86,87]
                    if words[85]>14 or words[88]>14:raise ValueError('Receipt phase fields')
                if current['version']==4:booleans += [92,93]
                reserved=74 if current['version']==1 else 83 if current['version']==2 else 92 if current['version']==3 else 96
                if any(words[i]>1 for i in booleans) or any(words[reserved:]):
                    raise ValueError('Receipt flags/reserved')
                current['word']=words;del current['words'];frames.append(current);current=None
    if current:incomplete+=1
    if not frames:raise ValueError('No complete verified receipt')
    if any(frame['version']!=frames[0]['version'] or frame['word']!=frames[0]['word'] or frame['pool']!=frames[0]['pool'] or
           frame['sequence']!=frames[0]['sequence'] or frame['fault']!=frames[0]['fault'] for frame in frames):
        raise ValueError('UEFI/XNU receipts disagree')
    chosen=next((f for f in frames if f['source']=='XNU'),frames[0])
    return {**chosen,'complete_frames':len(frames),'complete_sources':[f['source'] for f in frames],
        'incomplete_frames':incomplete}

def private_mmu_fault_receipt_accepted(receipt):
    """Full controlled-fault receipt gate; ordinary DMA/render PASS is separate."""
    if receipt.get('version')!=4 or receipt.get('source')!='XNU':return False
    w=receipt['word'];expected_va=w[94]|w[95]<<32
    signed=lambda n:n-(1<<32) if n&(1<<31) else n
    return w[92]==w[93]==1 and signed(w[0])==-5 and \
        all(w[i]==1 for i in [6,7,8,9,10,11,22,23,80]) and \
        not any(w[i] for i in [1,12,13,14,20,21,24,25,26,31,81,83,86,87,89]) and \
        receipt['sequence']==1 and w[15]==6 and w[74]==4 and \
        (w[30]&0x3f8)==0x3c0 and not w[91]&0x70007 and \
        receipt['fault']==expected_va==0x02003080 and w[27]==0xb3b2b8cd and \
        receipt.get('complete_frames',0)>=2 and {'UEFI','XNU'}<=set(receipt.get('complete_sources',[]))
