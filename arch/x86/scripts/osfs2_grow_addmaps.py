#!/usr/bin/env python3
"""Grow OsitoFS v2 and transactionally add maps from a host directory."""

MAGIC=0x4F534632; LAYOUT_MAGIC=0x4F324C59; SUPER_BACKUP_OFF=4096; FILETAB_OFF=1<<20
LEGACY_MAX_FILES=4096; MAX_BLOCKS=262144; CRCTAB_SIZE=MAX_BLOCKS*4; LAYERIDX_SIZE=512*2048
NAME_LEN=64; FLAG_VALID=1; FLAG_RAW=4
CRC_OFF=84

from osfs2_journal import (JOURNAL_OP_REPLACE, commit_entries, fix_super_crc,
                           lock, read_super, recover)
from osfs2_replace import (CRCTAB_OFF, FILETAB_OFF, FLAG_RAW, FLAG_VALID,
                           MAX_FILES, NAME_LEN, find_free_extent, find_part,
                           recompute_super)


def layout(s):
    layout_magic,file_slots,metadata_bytes=struct.unpack_from('<3I',s,88)
    slots=LEGACY_MAX_FILES
    if layout_magic==LAYOUT_MAGIC:
        slots=file_slots
        if slots<LEGACY_MAX_FILES or slots%LEGACY_MAX_FILES:
            raise ValueError(f"invalid file slot count: {slots}")
    crctab_off=FILETAB_OFF+slots*256
    layeridx_off=crctab_off+CRCTAB_SIZE
    data_off=layeridx_off+LAYERIDX_SIZE
    if layout_magic==LAYOUT_MAGIC and metadata_bytes!=data_off:
        raise ValueError("metadata_bytes does not match layout")
    return slots,crctab_off

def main():
    img=sys.argv[1]; hostdir=sys.argv[2]
    headroom=int(sys.argv[3]) if len(sys.argv)>3 else 64
    files=sorted(glob.glob(os.path.join(hostdir,'*.unr')))
    if not files:
        print(f"no *.unr in {hostdir}"); return 1
    with open(img,'r+b') as f:
        p=find_part(f)
        if p is None: print("no superblock"); return 1
        f.seek(p); s=bytearray(f.read(512))
        magic,ver,bsz,total,used,fcount,nextblk=struct.unpack_from('<7I',s,0)
        max_files,crctab_off=layout(s)
        print(f"before: total={total} used={used} fcount={fcount} nextblk={nextblk} bsz={bsz}")
        # collect existing names + free slots
        names=set(); free_slots=[]
        f.seek(p+FILETAB_OFF); tab=f.read(max_files*256)
        for i in range(max_files):
            ent=tab[i*256:(i+1)*256]
            flags=struct.unpack_from('<I',ent,84)[0]
            if flags & FLAG_VALID:
                names.add(entry[:NAME_LEN].split(b'\0')[0]
                          .decode('latin1', 'replace').lower())
            else:
                free_slots.append(i)
        # plan: which files to add (skip dups), total blocks needed
        plan=[]; need=0
        for path in files:
            dest=os.path.basename(path)
            if dest.lower() in names: continue
            sz=os.path.getsize(path); bc=(sz+bsz-1)//bsz
            plan.append((path,dest,sz,bc)); need+=bc
        if len(plan)>len(free_slots):
            print(f"not enough file slots: need {len(plan)}, free {len(free_slots)}"); return 1
        new_total=nextblk+need+headroom
        new_size=p+new_total*bsz
        print(f"adding {len(plan)} maps, {need} blocks; growing total {total} -> {new_total} "
              f"(image {os.path.getsize(img)/1e9:.2f}GB -> {new_size/1e9:.2f}GB)")
        # extend image file
        f.truncate(new_size)
        # write each file
        si=0
        for path,dest,sz,bc in plan:
            data=open(path,'rb').read()
            start=nextblk
            f.seek(p+start*bsz); f.write(data)
            pad=bc*bsz-len(data)
            if pad: f.write(b'\x00'*pad)
            ent=bytearray(256); nm=dest.encode('latin1'); ent[:len(nm)]=nm
            struct.pack_into('<Q',ent,64,len(data))
            struct.pack_into('<I',ent,72,start)
            struct.pack_into('<I',ent,76,bc)
            struct.pack_into('<I',ent,80,zlib.crc32(data)&0xFFFFFFFF)
            struct.pack_into('<I',ent,84,FLAG_VALID|FLAG_RAW)
            struct.pack_into('<H',ent,244,0xFFFF)
            slot=free_slots[si]; si+=1
            f.seek(p+FILETAB_OFF+slot*256); f.write(ent)
            for b in range(start,start+bc):
                if b<MAX_BLOCKS:
                    f.seek(p+crctab_off+b*4); f.write(b'\x00\x00\x00\x00')
            nextblk+=bc; used+=bc; fcount+=1
        # update superblock (primary + backup) + CRC
        struct.pack_into('<7I',s,0,magic,ver,bsz,new_total,used,fcount,nextblk)
        fix_super_crc(s)
        f.seek(p); f.write(s)
        f.seek(p+SUPER_BACKUP_OFF); f.write(s)
        print(f"after: total={new_total} used={used} fcount={fcount} nextblk={nextblk}")
        print("done.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
