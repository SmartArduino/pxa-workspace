/* SPDX-License-Identifier: MIT. Bounded integer-only guest log formatting. */
#include <aster/app.h>
#include <stdint.h>
static char line[240];
static unsigned used;
static void text(const char *p){while(*p && used<sizeof(line)-1)line[used++]=*p++;}
static void number(uint32_t n){char digits[10];unsigned count=0;do{digits[count++]=(char)('0'+n%10);n/=10;}while(n);while(count)line[used++]=digits[--count];}
static void begin(const char *test){used=0;text("BENCH system=aster test=");text(test);}
static void field(const char *name,uint32_t n){text(" ");text(name);text("=");number(n);}
static void send(void){text("\n");aster_log(2,line,used);}
void bench_report_cpu(unsigned kind,unsigned sample,uint32_t us,uint32_t sum)
{begin("cpu");field("kind",kind);field("sample",sample);field("us",us);field("sum",sum);send();}
void bench_report_pixels(uint32_t sum){begin("pixels");field("sum",sum);send();}
void bench_report_begin(unsigned mode,unsigned round){begin("frame_begin");field("mode",mode);field("round",round);send();}
void bench_report_frames(unsigned mode,unsigned round,uint32_t elapsed,unsigned frames,uint32_t paint,uint32_t submit)
{begin("frames");field("mode",mode);field("round",round);field("elapsed_us",elapsed);field("frames",frames);field("paint_us",paint);field("submit_us",submit);send();}
