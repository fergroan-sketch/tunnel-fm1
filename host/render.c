/* SPDX-License-Identifier: MPL-2.0 */
/* render a factory preset without the app, for level matching and listening: render PRESET SECONDS OUT.wav [SOLO_TRACK] [TUNNEL]
 *   cc -O2 -DPLAT_HOST -I ../fm1-base/include -I app host/render.c app/tun.c -lm -o render (from tunnel-fm1/) */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include "tun.h"
static const tun_preset_t FACTORY[] = {
#include "factory.h"
};
int main(int c, char **v){ int pi=atoi(v[1]); float sec=atof(v[2]); FILE*f=fopen(v[3],"wb"); int solo=c>4?atoi(v[4]):-1, mo=c>5?atoi(v[5]):0;
 static float out[512]; int16_t s[512]; uint32_t n=(uint32_t)(sec*44100)/256*256, i, k; uint32_t hdr[11];
 tun_init(); tun_set_preset(&FACTORY[pi]); if(solo>=0) for(k=0;k<8;k++) tun_mute(k,(int)k!=solo); tun_morph(mo); tun_play(1);
 hdr[0]=0x46464952; hdr[1]=36+n*4; hdr[2]=0x45564157; hdr[3]=0x20746d66; hdr[4]=16; hdr[5]=0x00020001; hdr[6]=44100; hdr[7]=44100*4; hdr[8]=0x00100004; hdr[9]=0x61746164; hdr[10]=n*4;
 fwrite(hdr,4,11,f);
 for(i=0;i<n;i+=256){ tun_render(out,256); for(k=0;k<512;k++){ float x=out[k]*32767; s[k]=(int16_t)(x>32767?32767:x<-32768?-32768:x);} fwrite(s,2,512,f);} fclose(f); return 0;}
