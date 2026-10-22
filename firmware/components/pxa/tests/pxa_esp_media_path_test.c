#include "../src/services/pxa_esp_media_path.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const char *valid[]={"PXA/media/lucinhu/pxa-nes", "PXA/media/lucinhu--abcd1234/pxa-game-boy",
        "PXA/media/author/app.v1", "PXA/media/author/app--abcdefgh"};
    const char *invalid[]={"", "PXA/media", "PXA/media/", "PXA/media/lucinhu",
        "PXA/media/f51ae392de0ca33cf8721d850d313d00911125b7796f3a456bde9ada789917fa",
        "PXA/media/lucinhu/", "PXA/media/lucinhu/pxa-nes/", "PXA/media/lucinhu/pxa-nes/roms",
        "PXA/media//pxa-nes", "PXA/media/../pxa-nes", "PXA/media/lucinhu/..",
        "PXA/media/author./app", "PXA/media/author/app.", "/PXA/media/lucinhu/pxa-nes",
        "PXA/media/lucinhu/pxa nes", "PXA/media/lucinhu/pxa\\nes"};
    assert(!pxa_esp_media_relative_valid(NULL));
    for(size_t i=0;i<sizeof(valid)/sizeof(valid[0]);++i)assert(pxa_esp_media_relative_valid(valid[i]));
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i)assert(!pxa_esp_media_relative_valid(invalid[i]));
    char maximum[141]="PXA/media/";
    memset(maximum+10,'a',64);maximum[74]='/';
    memset(maximum+75,'b',64);maximum[139]=0;
    assert(pxa_esp_media_relative_valid(maximum));
    maximum[139]='b';maximum[140]=0;
    assert(!pxa_esp_media_relative_valid(maximum));
    char managed[256]="PXA/media/";
    memset(managed+10,'a',64);strcpy(managed+74,"/quake/");
    memset(managed+81,'b',64);strcpy(managed+145,"/package");
    assert(pxa_esp_media_relative_valid(managed) && pxa_esp_media_relative_managed(managed));
    managed[81]='B';assert(!pxa_esp_media_relative_valid(managed));managed[81]='b';
    managed[149]='X';assert(!pxa_esp_media_relative_valid(managed));
    puts("PASS: media roots require publisher/app paths; AppKey-only roots rejected");
}
