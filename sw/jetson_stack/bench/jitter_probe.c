/* jitter_probe.c - 100 Hz periodic loop on Linux (Jetson), same shape as the STM32 IMU task:
 * wake every 10 ms and measure the actual period. Two variants:
 *   rel : clock_nanosleep(relative 10 ms) - what a naive control loop / timer does
 *   abs : clock_nanosleep(TIMER_ABSTIME)  - best case for a normal (non-RT) Linux thread
 * usage: jitter_probe <rel|abs> <seconds>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static long long ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000000000LL+t.tv_nsec; }
static int cmp(const void*a,const void*b){ long long x=*(long long*)a,y=*(long long*)b; return x<y?-1:x>y; }
int main(int argc,char**argv){
  int abs_mode = argc>1 && !strcmp(argv[1],"abs");
  int secs = argc>2 ? atoi(argv[2]) : 30, n = secs*100;
  long long *err = malloc(sizeof(long long)*n), prev = ns();
  struct timespec next; clock_gettime(CLOCK_MONOTONIC,&next);
  for (int i=0;i<n;i++){
    if (abs_mode){ next.tv_nsec += 10000000; if(next.tv_nsec>=1000000000){next.tv_nsec-=1000000000;next.tv_sec++;}
      clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&next,NULL); }
    else { struct timespec d={0,10000000}; clock_nanosleep(CLOCK_MONOTONIC,0,&d,NULL); }
    long long t=ns(); err[i]=(t-prev)-10000000; prev=t;
  }
  qsort(err,n,sizeof(long long),cmp);
  double sum=0,sq=0; for(int i=0;i<n;i++){sum+=err[i];sq+=(double)err[i]*err[i];}
  printf("%s n=%d period-error ns: min %lld p50 %lld p99 %lld p99.9 %lld max %lld | mean %.0f rms %.0f\n",
    abs_mode?"abs":"rel", n, err[0], err[n/2], err[(int)(n*0.99)], err[(int)(n*0.999)], err[n-1], sum/n, (double)0+__builtin_sqrt(sq/n));
  return 0;
}
