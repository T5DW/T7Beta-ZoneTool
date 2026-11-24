from pathlib import Path
import struct,math
import numpy as np
from scipy.spatial.transform import Rotation
class Reader:
 def __init__(self,p):self.b=Path(p).read_bytes();self.p=0
 def read(self,fmt):
  n=struct.calcsize('<'+fmt);v=struct.unpack_from('<'+fmt,self.b,self.p);self.p+=n;return v[0] if len(v)==1 else v
 def skip(self,n):self.p+=n
 def string(self):
  e=self.b.index(0,self.p);s=self.b[self.p:e].decode();self.p=e+1;return s
root=Path('cod4-msmc/MSMC')
def parts(name):
 r=Reader(root/'xmodelparts'/name);assert r.read('H')==25;count,roots=r.read('2H');bones=[{'parent':-1,'p':np.zeros(3),'q':np.array([0,0,0,1.])} for _ in range(roots)]
 for _ in range(count):
  p=r.read('b');t=np.array(r.read('3f'));q=np.array(r.read('3h'))/32768;q=np.r_[q,math.sqrt(max(0,1-np.dot(q,q)))];bones.append({'parent':p,'p':t,'q':q})
 for b in bones:b['name']=r.string()
 for b in bones:
  m=np.eye(4);m[:3,:3]=Rotation.from_quat(b['q']).as_matrix();m[:3,3]=b['p'];b['local']=m;b['world']=bones[b['parent']]['world']@m if b['parent']>=0 else m
 assert len(r.b)-r.p<=len(bones)+1,(r.p,len(r.b));return bones

def materials(name):
 r=Reader(root/'xmodel'/name);assert r.read('H')==25;r.skip(25);r.string();lods=[]
 for _ in range(4):
  dist=r.read('f');n=r.string()
  if n:lods.append(n)
 r.skip(4)
 for _ in range(r.read('I')):n=r.read('I');r.skip(n*48+36)
 return [r.string() for _ in range(r.read('H'))]
def surfaces(name):
 r=Reader(root/'xmodelsurfs'/name);assert r.read('H')==25;out=[]
 for _ in range(r.read('H')):
  r.skip(3);v,t,v2=r.read('3H')
  if v!=v2:
   r.skip(2)
   if v2:
    while r.read('H'):pass
    r.skip(2)
  else:defaultBone=r.read('H');r.skip(2)
  verts=[]
  for _ in range(v):
   n=r.read('3f');c=r.read('4B');uv=r.read('2f');r.skip(24);wc=r.read('B') if v!=v2 else 0;bone=r.read('H') if v!=v2 else defaultBone;p=r.read('3f');weights=[(bone,1.)]
   for _ in range(wc):
    bone2,w=r.read('2H');f=w/65535;weights[0]=(weights[0][0],weights[0][1]-f);weights.append((bone2,f))
   verts.append({'p':np.array(p),'n':np.array(n),'uv':np.array(uv),'c':c,'weights':weights})
  # Retain engine winding. Importer source reverses for Blender only.
  tri=np.array([r.read('3H') for _ in range(t)]);assert tri.max()<v
  out.append({'vertices':verts,'triangles':tri})
 assert r.p==len(r.b),(r.p,len(r.b));return out
if __name__=='__main__':
 for prefix in ['viewmodel','worldmodel']:
  name=prefix+'_bo2_msmc';bones=parts(name+'1');mats=materials(name);s=surfaces(name+'1');print(name,[(b['name'],b['parent']) for b in bones]);print([(m,len(x['vertices']),len(x['triangles'])) for m,x in zip(mats,s)])
