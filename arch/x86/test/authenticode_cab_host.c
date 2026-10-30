/* Bounded CAB/CMS parser tests. External Microsoft CAB is read only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#define OSITOK_X86_TYPES_H
#include "../win32/authenticode.c"
static const BYTE *input;
static DWORD input_size, position;
static unsigned long checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); return 1; } } while (0)
DWORD WINAPI SetFilePointer(HANDLE file, LONG offset, LONG *high, DWORD method)
{
    (void)file; (void)high;
    if (offset < 0 || method || (DWORD)offset > input_size) return 0xffffffffU;
    return position = (DWORD)offset;
}
BOOL WINAPI ReadFile(HANDLE file, PVOID buffer, DWORD length, DWORD *read, PVOID overlapped)
{
    (void)file; (void)overlapped;
    if (position > input_size || length > input_size - position) { *read=0; return 0; }
    memcpy(buffer, input+position, length); position+=length; *read=length; return 1;
}
int main(int argc, char **argv)
{
    CHECK(argc==2);
    FILE *file=fopen(argv[1],"rb"); CHECK(file!=NULL);
    CHECK(fseek(file,0,SEEK_END)==0); long size=ftell(file); CHECK(size>60&&size<2000000);
    rewind(file); BYTE *bytes=malloc((size_t)size); CHECK(bytes!=NULL);
    CHECK(fread(bytes,1,(size_t)size,file)==(size_t)size); fclose(file);
    input=bytes;input_size=(DWORD)size;
    auth_pe_info_t info; CHECK(auth_cab_info((HANDLE)1,input_size,&info)==0);
    auth_cms_t cms; CHECK(auth_parse_cms(bytes+info.certificate_offset,info.certificate_size,0,&cms)==0);
    CHECK(auth_cab_verify_digest((HANDLE)1,&info,&cms)==AUTHENTICODE_VERIFY_OK);
    auth_der_t token;
    CHECK(auth_find_attribute(&cms.signer.unsigned_attributes,oid_rfc3161_timestamp,sizeof(oid_rfc3161_timestamp),&token)==0);
    auth_cms_t timestamp; CHECK(auth_parse_cms(token.tlv,token.tlv_len,1,&timestamp)==0);
    int index=auth_find_signer_cert(&timestamp.signer,timestamp.certs,timestamp.cert_count); CHECK(index>=0);
    CHECK(auth_timestamp_certid(&timestamp,index)==0);
    CHECK(auth_timestamp_usage(&timestamp.certs[index])==0);
    CHECK(auth_timestamp_usage(&cms.certs[0])!=0);
    uint32_t time=0; CHECK(auth_parse_rfc3161_time(&timestamp,&cms.signer,&time)==0);
    CHECK(time==1613595661U); /* 2021-02-17 21:01:01.371 UTC */
    BYTE *sig=(BYTE *)cms.signer.signature; sig[0]^=1;
    CHECK(auth_parse_rfc3161_time(&timestamp,&cms.signer,&time)!=0); sig[0]^=1;
    BYTE *imprint=(BYTE *)memchr(timestamp.content,0xbd,timestamp.content_len);
    CHECK(imprint!=NULL); *imprint^=1;
    CHECK(auth_parse_rfc3161_time(&timestamp,&cms.signer,&time)!=0);*imprint^=1;
    for (unsigned n=0;n<60;n++) { input_size=n; CHECK(auth_cab_info((HANDLE)1,n,&info)!=0); }
    input_size=(DWORD)size;CHECK(auth_cab_info((HANDLE)1,input_size,&info)==0);
    for (DWORD n=0;n<info.certificate_size-1;n++) {
        BYTE *cut=malloc(n?n:1); CHECK(cut!=NULL);memcpy(cut,bytes+info.certificate_offset,n);
        auth_cms_t parsed;CHECK(auth_parse_cms(cut,n,0,&parsed)!=0);free(cut);
    }
    for (DWORD n=0;n<token.tlv_len;n++) {
        BYTE *cut=malloc(n?n:1);CHECK(cut!=NULL);memcpy(cut,token.tlv,n);
        auth_cms_t parsed;CHECK(auth_parse_cms(cut,n,1,&parsed)!=0);free(cut);
    }
    for (unsigned n=8;n<36;n++) {
        bytes[n]^=1;
        CHECK(auth_cab_verify_digest((HANDLE)1,&info,&cms)!=AUTHENTICODE_VERIFY_OK);
        bytes[n]^=1;
    }
    for (unsigned n=52;n<60;n++) {
        bytes[n]^=1;CHECK(auth_cab_verify_digest((HANDLE)1,&info,&cms)==AUTHENTICODE_VERIFY_OK);bytes[n]^=1;
    }
    DWORD random=0x73697031;
    for (unsigned i=0;i<12000;i++) {
        random=random*1664525U+1013904223U;DWORD pos=random%info.certificate_size;
        BYTE *der=bytes+info.certificate_offset;BYTE old=der[pos];der[pos]^=(BYTE)(1U<<(i%8));
        auth_cms_t parsed;
        if (!auth_parse_cms(der,info.certificate_size,0,&parsed)) {
            auth_der_t enhanced; (void)auth_parse_enhanced_hash(&parsed,&enhanced);
            auth_der_t t;
            if (!auth_find_attribute(&parsed.signer.unsigned_attributes,oid_rfc3161_timestamp,sizeof(oid_rfc3161_timestamp),&t)) {
                auth_cms_t ts;
                if (!auth_parse_cms(t.tlv,t.tlv_len,1,&ts)) {
                    (void)auth_parse_rfc3161_time(&ts,&parsed.signer,&time);
                    int signer=auth_find_signer_cert(&ts.signer,ts.certs,ts.cert_count);
                    if(signer>=0) { (void)auth_timestamp_certid(&ts,signer); (void)auth_timestamp_usage(&ts.certs[signer]); }
                }
            }
        }
        der[pos]=old;checks++;
    }
    free(bytes);printf("CAB HOST PASS %lu checks\n",checks);return 0;
}
