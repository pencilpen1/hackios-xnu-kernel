"""Compile the repository's small AIR test functions into a local MTLB artifact."""
from pathlib import Path
import argparse,hashlib,json,subprocess,struct
P=Path(__file__).resolve().parent
ap=argparse.ArgumentParser();ap.add_argument('--out',required=True,type=Path);a=ap.parse_args()
a.out.mkdir(parents=True,exist_ok=False)
functions=[];inputs={}
tag=lambda name,data:name+struct.pack('<H',len(data))+data
for source,kind in [('vertex',0),('fragment',1),('fill',2)]:
    path=P/'tests/shaders'/(source+'.ll');inputs[str(path)]=hashlib.sha256(path.read_bytes()).hexdigest()
    bc=a.out/(source+'.bc');subprocess.run(['llvm-as',path,'-o',bc],check=True)
    raw=bc.read_bytes()
    if raw[:4]==struct.pack('<I',0x0b17c0de):
        wrapped=raw
    else:
        assert raw[:4]==b'BC\xc0\xde'
        wrapped=struct.pack('<5I',0x0b17c0de,0,20,len(raw),0xffffffff)+raw
    wrapped+=b'\0'*(-len(wrapped)%16)
    functions.append((source,kind,wrapped))
entries=[];modules=bytearray();public=bytearray();private=bytearray()
for source,kind,module in functions:
    name=('lancelot_'+source).encode()+b'\0'
    fields=tag(b'NAME',name)+tag(b'TYPE',bytes([kind]))+tag(b'HASH',hashlib.sha256(module).digest())
    fields+=tag(b'MDSZ',struct.pack('<Q',len(module)))
    fields+=tag(b'OFFT',struct.pack('<3Q',len(public),len(private),len(modules)))
    fields+=tag(b'VERS',struct.pack('<4H',2,5,3,0))+b'ENDT'
    entries.append(struct.pack('<I',len(fields)+4)+fields)
    public+=struct.pack('<I',8)+b'ENDT'
    private+=struct.pack('<I',8)+b'ENDT'
    modules+=module
table=struct.pack('<I',len(entries))+b''.join(entries)+b'ENDT'
header=bytearray(88);header[:4]=b'MTLB'
struct.pack_into('<6H',header,4,1,2,7,0x8200,16,7)
funcs=88;pub=funcs+len(table);priv=pub+len(public);bc=priv+len(private);total=bc+len(modules)
struct.pack_into('<9Q',header,16,total,funcs,len(table),pub,len(public),priv,len(private),bc,len(modules))
binary=a.out/'lancelot-test.metallib';binary.write_bytes(header+table+public+private+modules)
record={'passed':True,'inputs':inputs,'functions':[{'name':'lancelot_'+s,'type':k,'module_bytes':len(b)}for s,k,b in functions],
        'artifact':str(binary),'bytes':total,'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
        'only_repository_authored_shader_sources':True,'device_verified':False}
(a.out/'result.json').write_text(json.dumps(record,indent=2)+'\n')
print('Built repository AIR test shaders:',binary,flush=True)
