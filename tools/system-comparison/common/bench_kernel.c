/* SPDX-License-Identifier: MIT. Identical C source and flags in both guests. */
#include "bench_kernel.h"
#include <string.h>
static uint32_t memory[32768];
static float matrix_a[4096],matrix_b[4096],matrix_c[4096];

__attribute__((noinline)) uint32_t bench_kernel(unsigned kind)
{
    uint32_t x=0x12345678u,sum=0;
    if(kind==0){
        for(unsigned i=0;i<500000;i++){
            x^=x<<13;x^=x>>17;x^=x<<5;x=x*1664525u+1013904223u;
            sum+=(x>>7)^i;
        }
        return sum^x;
    }
    if(kind==1){
        float a=0.25f,b=0.75f;
        for(unsigned i=0;i<500000;i++){
            a=a*0.99997f+b*0.00003f;b=b*0.99991f+a*0.00009f;
        }
        memcpy(&sum,&a,4);return sum;
    }
    if(kind==2){
        for(unsigned i=0;i<32768;i++)memory[i]=i*2654435761u;
        for(unsigned pass=0;pass<32;pass++){
            for(unsigned i=0;i<32768;i++)memory[i]=memory[i]*1664525u+1013904223u;
        }
        for(unsigned i=0;i<32768;i++)sum^=memory[i];
        return sum;
    }
    if(kind==3){
        for(unsigned i=0;i<32768;i++)memory[i]=(i+8191u)&32767u;
        x=0;
        for(unsigned i=0;i<524289;i++)x=memory[x];
        return x;
    }
    for(unsigned i=0;i<4096;i++){
        matrix_a[i]=(float)(i%17)*0.0625f;matrix_b[i]=(float)(i%13)*0.125f;
    }
    for(unsigned y=0;y<64;y++)for(unsigned col=0;col<64;col++){
        float value=0;
        for(unsigned k=0;k<64;k++)value+=matrix_a[y*64+k]*matrix_b[k*64+col];
        matrix_c[y*64+col]=value;
    }
    for(unsigned i=0;i<4096;i++){memcpy(&x,matrix_c+i,4);sum+=x;}
    return sum;
}

__attribute__((noinline)) void bench_paint(uint16_t *p,unsigned w,unsigned h,unsigned pitch,unsigned mode,unsigned phase)
{
    if(mode==0)return; /* Already initialized: presentation-only workload. */
    if(mode==1 || mode==3){
        uint16_t color=(uint16_t)((phase*73u)^0x1234u);
        for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)p[y*pitch+x]=color;
        if(mode==1)return;
        for(unsigned i=0;i<128;i++){
            unsigned x0=(i*19+phase*3)%(w-20),y0=(i*23+phase*2)%(h-14);
            for(unsigned y=y0;y<y0+14;y++)for(unsigned x=x0;x<x0+20;x++)p[y*pitch+x]=(uint16_t)(i*503u);
        }
        return;
    }
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){
        uint32_t v=(x+phase)*0x45d9f3bu ^ (y+phase)*0x119de1f3u;
        v^=v>>16;v*=0x45d9f3bu;v^=v>>16;p[y*pitch+x]=(uint16_t)v;
    }
}
uint32_t bench_checksum(const uint16_t *p,unsigned w,unsigned h,unsigned pitch)
{
    uint32_t value=2166136261u;
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)value=(value^p[y*pitch+x])*16777619u;
    return value;
}
