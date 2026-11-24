#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/intel_rcs_sampled_test.h"
#include "../drivers/intel_rcs_program_test.h"
static unsigned checks,cases;
#define CHECK(x) do {checks++;if (!(x)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}} while (0)
static intel_rcs_sampled_test_t fixture,saved;

static uint32_t bits(float v) {uint32_t n;memcpy(&n,&v,4);return n;}
static uint32_t color(unsigned x,unsigned y,unsigned level)
{
    return 0xff000000u|((level*40+x*4)<<16)|((level*20+y*8)<<8)|((x+y)*4);
}
static int address(int x,int size,unsigned mode)
{
    if (mode==1) return (x%size+size)%size;
    if (mode==2) {int p=(x%(2*size)+2*size)%(2*size);return p<size ? p : 2*size-1-p;}
    if (mode==4 && (x<0 || x>=size)) return -1;
    if (mode==5 && x<0) x=-x-1;
    return x<0 ? 0 : x>=size ? size-1 : x;
}
static void prepare(const struct intel_d3d9_program *p,unsigned mask,unsigned kind,unsigned parameter)
{
    memset(&fixture,0,sizeof(fixture));
    intel_rcs_sampled_draw_t *d=&fixture.draw;
    d->primitive=(intel_rcs_primitive_draw_t){
        .pixel={.batch_gpu=0x200000,.program_gpu=0x230000,.destination_gpu=0x220000,
            .destination_bytes=32768,.destination_width=96,.destination_height=72,.destination_pitch=416,
            .x=8,.y=8,.width=16,.height=16,.dispatch_mask=mask},
        .vertices_gpu=0x250000,.vertices_bytes=8192,.vertex_stride=144,
        .vertex_count=6,.topology=INTEL_RCS_TRIANGLE_LIST,
    };
    d->bound_mask=p->texture_mask;
    for (unsigned i=0;i<8;i++) d->texcoord_offset[i]=16+i*16;
    unsigned slot=p->texture_mask==0x8000 ? 15 : 0;
    intel_rcs_texture_binding_t *t=&d->textures[slot];
    CHECK(intel_gen9_image_layout(16,16,5,21,64,&t->image));
    t->gpu=0x210000;t->bytes=t->image.bytes;t->count=5;
    t->sampler=(intel_gen9_sampler_t){1,1,0,3,3,3,0,0,0,0xff112233};
    if (kind==1)t->sampler.min_filter=t->sampler.mag_filter=2;
    if (kind==2)t->sampler.address_u=parameter;
    if (kind==3) {t->first=parameter;t->count=5-parameter;}
    if (kind==4) {t->sampler.mip_filter=1;t->sampler.min_lod=t->sampler.max_lod=parameter*256;}
    memset(fixture.source,0xcd,sizeof(fixture.source));
    for (unsigned level=0;level<5;level++) {
        const intel_gen9_image_level_t *m=&t->image.mip[level];
        for (unsigned y=0;y<m->height;y++)for(unsigned x=0;x<m->width;x++)
            fixture.source[m->offset/4+y*t->image.pitch/4+x]=color(x,y,level);
    }
    if (kind==5 || kind==6) {
        unsigned second=kind==5 ? 1 : 15;
        d->textures[second]=*t;d->textures[second].gpu=0x212000;
        for (unsigned level=0;level<5;level++) {
            const intel_gen9_image_level_t *m=&t->image.mip[level];
            for (unsigned y=0;y<m->height;y++)for(unsigned x=0;x<m->width;x++)
                fixture.source[8192/4+m->offset/4+y*t->image.pitch/4+x]=color(m->width-1-x,m->height-1-y,level);
        }
    }
    const unsigned vx[]={8,24,8,8,24,24},vy[]={8,8,24,24,8,24};
    for (unsigned i=0;i<6;i++) {
        uint32_t *v=fixture.vertices+i*36;
        v[0]=bits((float)vx[i]);v[1]=bits((float)vy[i]);v[3]=bits(1.0f);
        float u=((float)vx[i]-8+(kind==1 ? 1.0f : 0.5f)-(kind==2 ? 4.0f : 0.0f))/16.0f;
        float vv=((float)vy[i]-8+(kind==1 ? 1.0f : 0.5f))/16.0f;
        for (unsigned c=0;c<8;c++) {
            v[4+c*4]=bits(kind==6 && c==7 ? 1.0f-u : u);
            v[5+c*4]=bits(vv);v[7+c*4]=bits(1.0f);
        }
    }
    memset(fixture.expected,0x5a,sizeof(fixture.expected));
    unsigned level=kind==3 || kind==4 ? parameter : 0,size=16>>level;
    for (unsigned y=0;y<16;y++)for(unsigned x=0;x<16;x++) {
        int sx=address(((int)x-(kind==2 ? 4 : 0))>>level,(int)size,t->sampler.address_u);
        unsigned sy=y>>level;
        uint32_t expected=sx<0 ? t->sampler.border_argb : color((unsigned)sx,sy,level);
        if (kind==1) {
            unsigned nx=x<15 ? x+1 : x,ny=y<15 ? y+1 : y;
            uint32_t a=color(x,y,0),b=color(nx,y,0),c=color(x,ny,0),e=color(nx,ny,0);
            expected=0xff000000;
            for (unsigned shift=0;shift<24;shift+=8)
                expected|=((((a>>shift)&255)+((b>>shift)&255)+((c>>shift)&255)+((e>>shift)&255))/4)<<shift;
        }
        if (kind==5) {
            /* First sample supplies normalized R/G coordinates to s1. */
            unsigned u=(x*4*16)/255,v=(y*8*16)/255;
            expected=color(15-u,15-v,0);
        } else if (kind==6) {
            uint32_t a=color(x,y,0),b=color(x,15-y,0);
            expected=0xff000000;
            for (unsigned shift=0;shift<24;shift+=8) {
                unsigned value=((a>>shift)&255)+((b>>shift)&255);
                expected|=(value>255 ? 255 : value)<<shift;
            }
        } else if (kind==7) expected=color(4,12,0); /* authored shader samples (0.25,0.75) */
        fixture.expected[(y+8)*104+x+8]=expected;
    }
}

static void negative(const struct intel_d3d9_program *p)
{
    saved=fixture;
    for (unsigned i=0;i<21;i++) {
        fixture=saved;
        intel_rcs_sampled_draw_t *d=&fixture.draw;
        switch (i) {
        case 0:d->primitive.pixel.batch_gpu+=4096;break;
        case 1:d->primitive.pixel.program_gpu+=4096;break;
        case 2:d->primitive.pixel.destination_gpu+=4096;break;
        case 3:d->primitive.vertices_gpu+=4096;break;
        case 4:d->primitive.pixel.destination_pitch+=4;break;
        case 5:d->primitive.pixel.destination_width--;break;
        case 6:d->primitive.pixel.destination_height--;break;
        case 7:d->primitive.pixel.destination_bytes+=4096;break;
        case 8:d->primitive.vertices_bytes+=4096;break;
        case 9:d->textures[0].gpu=0x20f000;break;
        case 10:d->textures[0].gpu=0x215000;break;
        case 11:d->textures[0].bytes=UINT64_MAX;break;
        case 12:fixture.vertices[0]=0x7f800000;break;
        case 13:fixture.vertices[2]=0x3f800000;break;
        case 14:fixture.vertices[3]=0;break;
        case 15:fixture.vertices[4]=0x7fc00000;break;
        case 16:d->primitive.vertex_offset=8192;break;
        case 17:d->primitive.vertex_count=65532;break;
        case 18:d->bound_mask=0;break;
        case 19:d->texcoord_offset[0]=144;break;
        case 20:d->primitive.pixel.dispatch_mask=0;break;
        }
        CHECK(!intel_rcs_sampled_test_validate(p,p->allocation_bytes,&fixture,sizeof(fixture),saved.draw.primitive.pixel.dispatch_mask));
    }
    fixture=saved;
    CHECK(!intel_rcs_sampled_test_validate(p,p->allocation_bytes,&fixture,sizeof(fixture)-1,saved.draw.primitive.pixel.dispatch_mask));
    CHECK(!intel_rcs_sampled_test_validate(p,p->allocation_bytes,NULL,sizeof(fixture),saved.draw.primitive.pixel.dispatch_mask));
    CHECK(!intel_rcs_sampled_test_validate(p,p->allocation_bytes,(char *)&fixture+1,sizeof(fixture),saved.draw.primitive.pixel.dispatch_mask));
}

static void emit(FILE *f,const struct intel_d3d9_program *p,unsigned mask,unsigned kind,unsigned parameter)
{
    prepare(p,mask,kind,parameter);
    CHECK(intel_rcs_sampled_test_validate(p,p->allocation_bytes,&fixture,sizeof(fixture),mask));
    intel_rcs_program_test_case_t c={p->allocation_bytes,0,mask,sizeof(fixture)};
    CHECK(fwrite(&c,sizeof(c),1,f)==1);CHECK(fwrite(p,p->allocation_bytes,1,f)==1);
    CHECK(fwrite(&fixture,sizeof(fixture),1,f)==1);cases++;
}

static struct intel_d3d9_program *read_program(const char *folder,const char *name)
{
    char path[1024];snprintf(path,sizeof(path),"%s/%s.program",folder,name);
    FILE *f=fopen(path,"rb");CHECK(f!=NULL);CHECK(!fseek(f,0,SEEK_END));
    long bytes=ftell(f);CHECK(bytes>128 && bytes<65536);rewind(f);
    void *data=malloc((size_t)bytes);CHECK(data!=NULL);CHECK(fread(data,1,(size_t)bytes,f)==(size_t)bytes);
    fclose(f);return data;
}
int main(int argc,char **argv)
{
    CHECK(argc==3);
    struct intel_d3d9_program *p=read_program(argv[1],"ps30-sample"),*sparse=read_program(argv[1],"ps30-sparse");
    struct intel_d3d9_program *dependent=read_program(argv[1],"ps30-dependent"),*pair=read_program(argv[1],"ps30-pair");
    struct intel_d3d9_program *constant=read_program(argv[1],"ps30-constant-coordinate");
    prepare(p,3,0,0);negative(p);
    for (unsigned suite=0;suite<3;suite++) {
        char path[1024];snprintf(path,sizeof(path),"%s/%s.bin",argv[2],suite==2 ? "dependent" : suite ? "filtering" : "basic");
        FILE *f=fopen(path,"wb");CHECK(f!=NULL);
        intel_rcs_program_test_header_t header={INTEL_RCS_PROGRAM_TEST_MAGIC,3,0,0};
        CHECK(fwrite(&header,sizeof(header),1,f)==1);unsigned start=cases;
        if (!suite) for (unsigned mask=1;mask<=3;mask++) {emit(f,p,mask,0,0);emit(f,sparse,mask,0,0);}
        else if (suite==1) {
            for (unsigned mask=1;mask<=3;mask++)emit(f,p,mask,1,0);
            for (unsigned mode=1;mode<=5;mode++)emit(f,p,3,2,mode);
            for (unsigned level=1;level<=4;level++) {emit(f,p,3,3,level);emit(f,p,3,4,level);}
        } else {
            for (unsigned mask=1;mask<=7;mask++)emit(f,dependent,mask,5,0);
            for (unsigned mask=1;mask<=3;mask++) {emit(f,pair,mask,6,0);emit(f,constant,mask,7,0);}
        }
        long bytes=ftell(f);CHECK(bytes>0 && bytes<=INTEL_RCS_PROGRAM_TEST_MAX_BYTES);
        header.cases=cases-start;header.bytes=(uint32_t)bytes;
        CHECK(!fseek(f,0,SEEK_SET));CHECK(fwrite(&header,sizeof(header),1,f)==1);CHECK(!fclose(f));
    }
    free(p);free(sparse);free(dependent);free(pair);free(constant);
    printf("Native sampled fixture PASS checks=%u cases=%u bounded physical inputs; GPU not executed\n",checks,cases);
    return 0;
}
