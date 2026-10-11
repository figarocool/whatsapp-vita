#ifndef WA_FPKG_HEAD_H
#define WA_FPKG_HEAD_H

#include <mbedtls/sha1.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Portions adapted from VitaShell package_installer.c, Copyright (C) 2015-2018
 * TheFloW. Builds the head.bin required by PromotePkgWithRif. The template is
 * VitaShell's resources/head.bin; see licenses/vitashell-GPL-3.0.txt.
 */
static uint32_t wa_head_be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

static void wa_head_hmac(const uint8_t *data,size_t len,uint8_t out[16]) {
    uint8_t digest[20],mix[64]={0};
    mbedtls_sha1(data,len,digest);
    memcpy(mix,digest+4,8);memcpy(mix+8,digest+4,8);
    memcpy(mix+16,digest+12,4);mix[20]=digest[16];
    mix[21]=digest[1];mix[22]=digest[2];mix[23]=digest[3];
    memcpy(mix+24,mix+16,8);
    mbedtls_sha1(mix,sizeof(mix),digest);memcpy(out,digest,16);
}

static int wa_make_head_bin(const char *root,const char *template_path,const char *title_id) {
    char template_file[256],out_dir[256],out_file[256],full_id[48];
    uint8_t head[2048],mac[16];
    int n=snprintf(template_file,sizeof(template_file),"%s",template_path);
    if(n<0||(size_t)n>=sizeof(template_file))return -1;
    n=snprintf(out_dir,sizeof(out_dir),"%s/sce_sys/package",root);
    if(n<0||(size_t)n>=sizeof(out_dir))return -1;
    n=snprintf(out_file,sizeof(out_file),"%s/head.bin",out_dir);
    if(n<0||(size_t)n>=sizeof(out_file))return -1;
    if(strlen(title_id)!=9)return -1;
    int fd=sceIoOpen(template_file,SCE_O_RDONLY,0);if(fd<0)return -1;
    int length=sceIoRead(fd,head,sizeof(head));sceIoClose(fd);
    if(length<0x100||length>(int)sizeof(head))return -1;
    snprintf(full_id,sizeof(full_id),"EP9000-%s_00-0000000000000000",title_id);
    memset(head+0x30,0,48);memcpy(head+0x30,full_id,strlen(full_id));
    uint32_t len=wa_head_be32(head+0xD0);
    if(len+16>(uint32_t)length)return -1;
    wa_head_hmac(head,len,mac);memcpy(head+len,mac,16);
    uint32_t off=wa_head_be32(head+0x8),info_len=wa_head_be32(head+0x10),out=wa_head_be32(head+0xD4);
    if(off>(uint32_t)length||info_len<64||off+info_len-64>(uint32_t)length||out+16>(uint32_t)length)return -1;
    wa_head_hmac(head+off,info_len-64,mac);memcpy(head+out,mac,16);
    len=wa_head_be32(head+0xE8);
    if(len+16>(uint32_t)length)return -1;
    wa_head_hmac(head,len,mac);memcpy(head+len,mac,16);
    sceIoMkdir(root,0777);
    char sce_sys[256];snprintf(sce_sys,sizeof(sce_sys),"%s/sce_sys",root);sceIoMkdir(sce_sys,0777);
    sceIoMkdir(out_dir,0777);
    fd=sceIoOpen(out_file,SCE_O_WRONLY|SCE_O_CREAT|SCE_O_TRUNC,0666);if(fd<0)return -1;
    int wrote=sceIoWrite(fd,head,(size_t)length);int close_res=sceIoClose(fd);
    return wrote==length&&close_res>=0?0:-1;
}
#endif
