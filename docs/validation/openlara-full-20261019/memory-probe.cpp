#include <cstdio>
#include "game.h"
static char *borrow_begin,*borrow_end;
bool olClassicBorrow(const void *p,size_t n) {
    auto v=reinterpret_cast<uintptr_t>(p),b=reinterpret_cast<uintptr_t>(borrow_begin),e=reinterpret_cast<uintptr_t>(borrow_end);
    return borrow_begin && v>=b && v<e && n<=e-v;
}
void *operator new(size_t n) {void *p=malloc(n?n:1);if(!p)__builtin_trap();return p;}
void *operator new[](size_t n) {return ::operator new(n);}
void operator delete(void *p) noexcept {if(!olClassicBorrow(p,0))free(p);}
void operator delete[](void *p) noexcept {::operator delete(p);}
void operator delete(void *p,size_t) noexcept {::operator delete(p);}
void operator delete[](void *p,size_t) noexcept {::operator delete(p);}
static int milliseconds;
int osGetTimeMS() {return milliseconds;}
bool osJoyReady(int i) {return i==0;}
void osJoyVibrate(int,float,float) {}
const char *osFixFileName(const char *name) {
    FILE *f=fopen(name,"rb");if(!f)return nullptr;fclose(f);return name;
}
static void memory(const char *phase) {
    printf("MEMORY %s linear=%u\n",phase,unsigned(__builtin_wasm_memory_size(0))*65536);
}
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    Core::width=320;Core::height=240;Core::defLang=0;
    static uint16 pixels[320*240];
    FILE *input=fopen(argv[1],"rb");if(!input)return 3;
    fseek(input,0,SEEK_END);int size=ftell(input);rewind(input);
    char *data=new char[size];if(fread(data,1,size,input)!=size)return 4;fclose(input);
    borrow_begin=data;borrow_end=data+size;
    memory("input");inventory=new Inventory();Game::init(new Stream(argv[1],data,size));memory("loaded");
    GAPI::resize();GAPI::swColor=pixels;
    for(int i=0;i<90;++i) {milliseconds+=34;if(Game::update())Game::render();}
    FILE *output=fopen(argv[2],"wb");if(!output)return 5;fwrite(pixels,2,320*240,output);fclose(output);
    memory("90-frames");Game::deinit();borrow_begin=borrow_end=nullptr;delete[]data;memory("freed");
}
