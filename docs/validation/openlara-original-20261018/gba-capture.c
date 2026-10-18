#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <stdio.h>
#include <stdlib.h>
static color_t pixels[240*160];
static void shot(const char *prefix,const char *name) {
 char path[512];snprintf(path,sizeof(path),"%s/%s.ppm",prefix,name);FILE*f=fopen(path,"wb");
 fprintf(f,"P6\n240 160\n255\n");
 for(unsigned i=0;i<240*160;++i){unsigned p=pixels[i];unsigned char c[]={p&255,(p>>8)&255,(p>>16)&255};fwrite(c,3,1,f);}fclose(f);
}
static void run(struct mCore*c,unsigned n,unsigned keys) {c->setKeys(c,keys);while(n--)c->runFrame(c);}
int main(int argc,char**argv) {
 if(argc!=3)return 2;struct mCore*c=mCoreFind(argv[1]);if(!c)return 3;
 mCoreInitConfig(c,"openlara-reference");if(!c->init(c))return 4;
 mCoreConfigSetIntValue(&c->config,"skipBios",1);mCoreConfigSetIntValue(&c->config,"mute",1);
 mCoreLoadConfig(c);c->setVideoBuffer(c,pixels,240);
 if(!mCoreLoadFile(c,argv[1]))return 5;c->reset(c);
 run(c,240,0);shot(argv[2],"official-title");
 run(c,8,1);run(c,120,0);shot(argv[2],"official-passport");
 run(c,8,1);run(c,300,0);shot(argv[2],"official-caves-idle");
 char filename[512];snprintf(filename,sizeof(filename),"%s/official-palette.bin",argv[2]);FILE*out=fopen(filename,"wb");
 for(unsigned i=0;i<256;i++){unsigned short v=c->busRead16(c,0x05000000+i*2);fwrite(&v,2,1,out);}fclose(out);
 snprintf(filename,sizeof(filename),"%s/official-vram.bin",argv[2]);out=fopen(filename,"wb");unsigned base=0x06000000+((c->busRead16(c,0x04000000)&16)?0xA000:0);
 for(unsigned i=0;i<240*160;i++){unsigned char v=c->busRead8(c,base+i);fwrite(&v,1,1,out);}fclose(out);
 run(c,60,16);run(c,120,0);shot(argv[2],"official-caves-turned");
 run(c,60,64);run(c,120,0);shot(argv[2],"official-caves-walked");
 c->deinit(c);return 0;
}
