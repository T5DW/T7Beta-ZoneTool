"""Convert the bare COD4 BO FAMAS view/world models for the T7 rigid mesh importer."""
import io
import json
from pathlib import Path
import struct
import sys

import numpy as np
from PIL import Image
from scipy.spatial.transform import Rotation
import cod4_reader as reader

reader.root, output = map(Path, sys.argv[1:3])

def texture(name):
    raw = (reader.root / 'images' / (name + '.iwi')).read_bytes()
    assert raw[:4] == b'IWi\x06'
    codec = {11:b'DXT1',12:b'DXT3',13:b'DXT5'}[raw[4]]
    w, h = struct.unpack_from('<HH', raw, 6)
    length = ((w+3)//4)*((h+3)//4)*(8 if codec == b'DXT1' else 16)
    header = struct.pack('<7I',124,0x81007,h,w,length,0,1)+bytes(44)+struct.pack('<II4sIIIII',32,4,codec,0,0,0,0,0)+struct.pack('<5I',0x1000,0,0,0,0)
    return Image.open(io.BytesIO(b'DDS '+header+raw[-length:])).convert('RGBA')

def maps(material):
    color = texture(material+'_col')
    n = np.asarray(texture(material+'_nml')).astype(float)
    x, y = n[:,:,3]/127.5-1, n[:,:,1]/127.5-1
    z = np.sqrt(np.maximum(0,1-x*x-y*y))
    normal = Image.fromarray(np.stack([(x+1)*127.5,(y+1)*127.5,(z+1)*127.5,np.full_like(z,255)],2).astype('uint8'))
    spec = texture(next((reader.root/'images').glob('~'+material+'_spc*')).stem).getchannel('A')
    return color,normal,Image.merge('RGBA',(spec,spec,spec,Image.new('L',spec.size,255)))

body, sights = maps('mtl_bo_famas'), maps('mtl_bo_iron_sights')
def pack_vec(vec):
    return sum(int(np.clip(round(float(x)*511+512),0,1023)) << (axis*10) for axis,x in enumerate(vec))

for name,folder in [('viewmodel_bo_famas',output),('worldmodel_bo_famas',output/'world')]:
    folder.mkdir(parents=True,exist_ok=True)
    bones = reader.parts(name+'1')
    # Keep the engine attachment at index zero and the original body as its own joint.
    for b in bones: b['parent'] += 1
    bones.insert(0,dict(name='tag_weapon',parent=-1,p=np.zeros(3),q=np.array([0.,0.,0.,1.]),world=np.eye(4)))
    pos, attrs, weights, indices = (bytearray() for _ in range(4))
    groups = []
    for material,surface in zip(reader.materials(name),reader.surfaces(name+'1'),strict=True):
        if material not in {'mtl_bo_famas','mtl_bo_famas_plastic','mtl_bo_iron_sights'}: continue
        vertices, faces = surface['vertices'],surface['triangles']
        bone = vertices[0]['weights'][0][0]+1
        groups.append(dict(bone=bone,vertexFirst=len(pos)//12,vertexCount=len(vertices),indexFirst=len(indices)//2,triangleCount=len(faces)))
        for v in vertices:
            assert v['weights']==[(bone-1,1.)]
            n = v['n']/np.linalg.norm(v['n'])
            tangent = np.cross(n,[0,0,1] if abs(n[2])<0.99 else [0,1,0])
            tangent /= np.linalg.norm(tangent)
            u,y = v['uv']
            pos.extend(struct.pack('<3f',*v['p']))
            attrs.extend(struct.pack('<4B2eII',255,255,255,255,(u+(2 if material=='mtl_bo_iron_sights' else 1))/4,y,pack_vec(n),pack_vec(tangent)))
            weights.extend(struct.pack('<4B4H',255,0,0,0,bone,0,0,0))
        indices.extend(faces.astype('<u2').tobytes())
    (folder/'mesh.bin').write_bytes(b'MSMC0001'+struct.pack('<II',len(pos)//12,len(indices)//6)+pos+attrs+weights+indices)
    (folder/'surfaces.json').write_text(json.dumps(groups))
    names = [b['name'] if b['name'] in ('tag_flash','tag_brass') else 't7mp_famas_'+b['name'] for b in bones]
    names[0]='tag_weapon'
    (folder/'skeleton.json').write_text(json.dumps(dict(names=names, parents=bytes(i-b['parent'] for i,b in enumerate(bones) if i).hex(),rotations=b''.join(struct.pack('<4e',*b['q']) for b in bones[1:]).hex(),translations=b''.join(struct.pack('<3f',*b['p']) for b in bones[1:]).hex(),baseMatrices=b''.join(struct.pack('<8f',*Rotation.from_matrix(b['world'][:3,:3]).as_quat(),*b['world'][:3,3],2) for b in bones).hex())))
    if folder != output: continue
    images = {}
    for i,key in enumerate(['color','normal','gloss']):
        tile = body[i].resize((1024,1024))
        atlas = Image.new('RGBA',(4096,1024))
        for col,im in enumerate([tile,tile,sights[i].resize(tile.size),tile]): atlas.paste(im,(1024*col,0))
        images[key]=atlas
    for key,pixel in [('mask',(0,0,0,255)),('ao',(255,255,255,255))]: images[key]=Image.new('RGBA',(1,1),pixel)
    for key,im in images.items(): (folder/(key+'.rgba')).write_bytes(struct.pack('<II',*im.size)+im.tobytes())
    print(name,len(groups),len(pos)//12,len(indices)//6)
