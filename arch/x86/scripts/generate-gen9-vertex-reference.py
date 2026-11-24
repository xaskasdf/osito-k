#!/usr/bin/env python3
from pathlib import Path
import subprocess,sys
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
gen=Path('arch/x86/lib/intel_d3d9/mesa-intel/src/intel/genxml')
symbols='3DSTATE_VS,3DSTATE_CLIP,3DSTATE_SF,3DSTATE_RASTER,3DSTATE_URB_VS,3DSTATE_PUSH_CONSTANT_ALLOC_VS,3DSTATE_PUSH_CONSTANT_ALLOC_PS,3DSTATE_CONSTANT_VS,3DSTATE_CONSTANT_BODY,3DSTATE_SBE,3DSTATE_SBE_SWIZ,SF_OUTPUT_ATTRIBUTE_DETAIL,SF_CLIP_VIEWPORT,VERTEX_ELEMENT_STATE'
with (out/'gen9-vertex-pack.h').open('w') as f:
    subprocess.run([sys.executable,str(gen/'gen_pack_header.py'),'--engines','render','--include-symbols',symbols,str(gen/'gen90.xml')],stdout=f,check=True)
