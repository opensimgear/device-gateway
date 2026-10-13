#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "gateway.h"

/* Test-only transport: real production protocol/report state, no USB hardware.
   A new hello/status following a reconnect ACK commits the test layout. */
static uint32_t milliseconds(void){struct timespec value;clock_gettime(CLOCK_MONOTONIC,&value);return (uint32_t)((uint64_t)value.tv_sec*1000+(uint64_t)value.tv_nsec/1000000);}
int main(void){
  osg_gateway_t gateway;osg_gateway_init(&gateway,"OSG-PICO-MOCK-0123456789ABCDEF");
  char line[OSG_LINE_CAPACITY+1],response[OSG_RESPONSE_CAPACITY];size_t used=0;bool oversized=false;int c;
  while((c=getchar())!=EOF){
    if(c=='\n'){
      if(gateway.reconnecting)osg_gateway_commit_layout(&gateway);
      uint32_t now=milliseconds();osg_gateway_tick(&gateway,now,true,true);
      if(oversized)osg_gateway_error(&gateway,"Request exceeds 4096 bytes.",response,sizeof response);
      else {line[used]=0;osg_gateway_command(&gateway,line,used,now,response,sizeof response);}
      fputs(response,stdout);fflush(stdout);used=0;oversized=false;
    }else if(!oversized){if(used>=OSG_LINE_CAPACITY){oversized=true;used=0;}else line[used++]=(char)c;}
  }
  osg_gateway_disconnect(&gateway);return 0;
}
