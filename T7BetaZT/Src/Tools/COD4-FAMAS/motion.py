"""Convert COD4 v17 FAMAS tracks to T7 animation streams."""
import json
import struct
from pathlib import Path
import sys
import numpy as np
from scipy.spatial.transform import Rotation, Slerp
import cod4_reader as reader

def decode(path):
    r=reader.Reader(path)
    version,frames,count,flags,kind,fps=r.read('3H2BH')
    assert version==17 and flags in (0,1) and frames<255
    width=(count+7)//8
    flip=r.b[r.p:r.p+width];r.skip(width)
    simple=r.b[r.p:r.p+width];r.skip(width)
    names=[r.string() for _ in range(count)]
    length=frames+flags
    tracks={}
    def keys():
        n=r.read('H')
        if n>1 and n!=length:
            idx=np.atleast_1d(r.read(str(n)+'B'))
            assert np.all(np.diff(idx.astype(int))>0) and idx[-1]<length,(path,r.p,idx,length)
            return idx
        return np.arange(n)
    for i,name in enumerate(names):
        qkeys=keys(); n=len(qkeys)
        quat=None
        if n:
            size=1 if simple[i//8]&(1<<(i%8)) else 3
            xyz=np.array(r.read(str(n*size)+'h')).reshape(n,size)/32768.
            if size==1: xyz=np.column_stack([np.zeros((n,2)),xyz])
            quat=np.column_stack([xyz,np.sqrt(np.maximum(0,1-(xyz*xyz).sum(1)))])
            if flip[i//8]&(1<<(i%8)): quat[0]*=-1
        pkeys=keys();n=len(pkeys)
        pos=None
        if n==1: pos=np.array([r.read('3f')])
        if n>1:
            small=r.read('B');assert small in (0,1)
            lo,span=np.array(r.read('6f')).reshape(2,3)
            pos=lo+span*np.array(r.read(str(n*3)+('B' if small else 'H'))).reshape(n,3)/(255 if small else 65535)
        tracks[name]=(qkeys,quat,pkeys,pos)
    notes=[(r.string(),r.read('H')) for _ in range(r.read('B'))]
    assert r.p==len(r.b),(path,r.p,len(r.b))
    return frames,flags,fps,tracks,notes

if __name__=='__main__':
    reader.root=Path(sys.argv[1]); out=Path(sys.argv[2]); anim=out/'animations';anim.mkdir(exist_ok=True)
    bones={b['name']:b for b in reader.parts('viewmodel_bo_famas1')}
    # In T7 the torso owns tag_weapon_right, which owns tag_weapon. COD4
    # stores the entire weapon transform directly on tag_weapon.
    idle=decode(reader.root/'xanim/viewmodel_famas_idle')[3]
    catalog=[]
    for suffix in 'idle fire ads_fire reload reload_empty pullout first_raise putaway sprint_in sprint_loop sprint_out ads_up ads_down pullout_quick putaway_quick last_shot'.split():
        frames,loop,fps,tracks,notes=decode(reader.root/'xanim'/('viewmodel_famas_'+suffix))
        end=frames if loop else frames-1
        keys=np.array(sorted(set(range(0,end,2))|{end}),dtype='u1')
        names=[]; db=bytearray();ds=bytearray();ints=bytearray();qr=bytearray();tr=bytearray()
        ads=suffix in ('ads_up','ads_down')
        if not ads:
            # Preserve the original local translations as well as rotations.
            # In particular the elbow and wrist translations are the arm lengths.
            tracks={**{name:([],None,[],None) for name in idle},**tracks}
        for name,(qk,q,pk,pos) in tracks.items():
            target=name
            if ads:
                if name!='tag_torso':continue
            elif name=='tag_weapon':target='tag_weapon_right'
            elif name in bones:
                if name not in ('tag_flash','tag_brass'):target='t7mp_famas_'+name
            elif not name.startswith(('j_shoulder','j_elbow','j_wrist','j_thumb','j_index','j_mid','j_ring','j_pinky')):continue
            if target.startswith(('j_index_','j_pinky_','j_ring_')) and target.endswith('_0'):
                digit,side,_=target[2:].split('_')
                target='j_'+digit+'base_'+side
            reference=idle.get(name,([],None,[],None))
            if q is None and pos is None and name not in bones and reference[1] is None and reference[3] is None:continue
            q0=bones[name]['q'] if name in bones else [0.,0.,0.,1.]
            if q is None:qrow=np.tile(reference[1][0] if reference[1] is not None else q0,(len(keys),1))
            elif len(q)==1:qrow=np.tile(q[0],(len(keys),1))
            else:qrow=Slerp(qk,Rotation.from_quat(q))(np.clip(keys,qk[0],qk[-1])).as_quat()
            for i in range(1,len(keys)):
                if np.dot(qrow[i-1],qrow[i])<0:qrow[i]*=-1
            if pos is not None:
                prow=np.stack([np.interp(keys,pk,a) for a in pos.T],axis=1)
            else:
                p0=np.zeros(3)
                prow=np.tile(reference[3][0] if reference[3] is not None else p0,(len(keys),1))
            # COD4 weapon translation keys are offsets from the part's bind position.
            # The clip and bolt both start at zero even though their pivots are nonzero.
            if ads:
                hip=0 if suffix=='ads_up' else -1
                aimed=-1 if hip==0 else 0
                prow-=prow[hip].copy()
                # Rear aperture center (631,182), viewport center (638.5,358).
                # The .77-unit rear guards span about 96 pixels in this ADS frame.
                # Use the original blend curve so the hip pose and return agree.
                blend=np.clip(prow[:,1]/prow[aimed,1],0,1)
                px_per_unit=96/.77
                prow[:,1]-=blend*(638.5-631)/px_per_unit
                prow[:,2]-=blend*(358-182)/px_per_unit
            idx=len(names);names.append(target)
            qr.extend(qrow.astype('<f2').tobytes())
            lo=prow.min(0);scale=np.ptp(prow,axis=0)/65535
            ints.extend(np.r_[lo,scale].astype('<f4').tobytes())
            tr.extend(np.rint((prow-lo)/np.where(scale,scale,1)).clip(0,65535).astype('<u2').tobytes())
        if not ads:
            names.append('tag_weapon')
            qr.extend(np.tile([0,0,0,1],(len(keys),1)).astype('<f2').tobytes())
            tr.extend(bytes(len(keys)*6))
            ints.extend(bytes(24))
        assert names and len(keys)<end+1
        ds=struct.pack('<H',len(keys)-1)*2*len(names)
        db=keys.tobytes()*len(names)+b''.join(bytes([i])+keys.tobytes() for i in range(len(names)))
        source='am_bo_famas_'+suffix
        data=dict(name='t5dw_'+source,source=source,fps=fps,frames=end,loop=bool(loop),adsTorsoDelta=ads,bones=names,keyFrames=keys.tolist(),dataBytes=db.hex(),dataShort=ds.hex(),dataInt=ints.hex(),randomShort=(qr+tr).hex(),soundNotes=notes)
        (anim/(source+'.json')).write_text(json.dumps(data,separators=(',',':')))
        catalog.append(dict(source=source))
    (out/'animations.json').write_text(json.dumps(catalog))
