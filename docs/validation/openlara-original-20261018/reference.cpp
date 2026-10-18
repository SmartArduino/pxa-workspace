#include "game.h"
static uint8 data[2700000];
static uint16 palette[256];
static uint32 clock_ms;
int32 fps;
int32 osGetSystemTimeMS(){return clock_ms;}
bool osSaveSettings(){return false;} bool osLoadSettings(){return false;}
bool osCheckSave(){return false;} bool osSaveGame(){return false;} bool osLoadGame(){return false;}
void osJoyVibrate(int32,int32,int32){}
void osSetPalette(const uint16*p){memcpy(palette,p,sizeof(palette));}
const void*osLoadScreen(LevelID){return data;} const void*osLoadLevel(LevelID){return data;}
#ifndef __GBA_WIN__
void renderInit(){} void renderFree(){} void renderSwap(){} void renderLevelInit(){} void renderLevelFree(){}
#endif
void sndInit(){} void sndInitSamples(){} void sndFreeSamples(){} void sndFill(int8*){}
void*sndPlaySample(int32,int32,int32,int32){return nullptr;}
void sndPlayTrack(int32){} bool sndTrackIsPlaying(){return false;}
void sndStopTrack(){} void sndStopSample(int32){} void sndStop(){}
extern "C" {
uint8* ref_data(){return data;}
void ref_start(int id){clock_ms=1000;gLevelID=LevelID(id);gameInit();gSettings.video_fps=0;}
void ref_tick(uint32 buttons){clock_ms+=33;keys=buttons;gameUpdate(1);gameRender();}
uint8*ref_pixels(){return (uint8*)fb;}
uint16*ref_palette(){return palette;}
int32*ref_state(){static int32 state[8];auto*p=players[0];state[0]=p->pos.x;state[1]=p->pos.y;state[2]=p->pos.z;state[3]=p->angle.y;state[4]=p->extraL->camera.view.pos.x;state[5]=p->extraL->camera.view.pos.y;state[6]=p->extraL->camera.view.pos.z;state[7]=gSettings.video_gamma;return state;}
}
