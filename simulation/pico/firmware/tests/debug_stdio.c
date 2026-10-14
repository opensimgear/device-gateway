#define _POSIX_C_SOURCE 200809L
#include "debug_gateway.h"
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
/* Same firmware core with an asynchronous clock and USB completion boundary.
 * Pipes are for protocol acceptance; this harness does not enumerate USB. */
static osg_debug_gateway_t gateway;
static char response[OSG_DEBUG_RESPONSE_CAPACITY],line[OSG_DEBUG_LINE_CAPACITY];
static uint32_t milliseconds(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint32_t)((uint64_t)t.tv_sec*1000+t.tv_nsec/1000000); }
int main(void) {
    uint32_t started=milliseconds();size_t used=0;bool overflow=false;
    if(!osg_debug_init(&gateway,"000000000123456789abcdef","OSG-PICO-0123456789abcdef",NULL)) return 1;
    osg_debug_connected(&gateway,true);
    for(;;) {
        uint32_t now=milliseconds()-started;
        osg_gateway_layout_event_t event=osg_debug_tick(&gateway,now,true);
        if(event==OSG_GATEWAY_LAYOUT_DETACH && !osg_debug_layout_activate(&gateway)) return 2;
        for(unsigned i=0;i<OSG_GW_MAX_MODULES;i++) {
            uint8_t report[64];osg_gateway_delivery_t delivery;
            size_t n=osg_debug_report_prepare(&gateway,i,report,sizeof report,&delivery);
            if(n) osg_debug_report_complete(&gateway,&delivery,report,n);
        }
        struct pollfd input={.fd=STDIN_FILENO,.events=POLLIN};
        if(poll(&input,1,1)<0) return 3;
        if(input.revents&(POLLIN|POLLHUP)) {
            char bytes[256];ssize_t n=read(STDIN_FILENO,bytes,sizeof bytes);
            if(n<=0) break;
            for(ssize_t i=0;i<n;i++) {
                if(bytes[i]=='\n') {
                    line[used]=0;
                    if(overflow) strcpy(line,"invalid");
                    if(!strcmp(line,"transport 0") || !strcmp(line,"transport 1")) {
                        osg_debug_connected(&gateway,line[10]=='1');strcpy(response,"{\"transport\":true}\n");
                    } else if(!osg_debug_command(&gateway,line,response,sizeof response)) return 4;
                    fputs(response,stdout);fflush(stdout);used=0;overflow=false;
                } else if(bytes[i]!='\r' && !overflow) {
                    if(used+1==sizeof line) overflow=true;else line[used++]=bytes[i];
                }
            }
        }
    }
    osg_debug_connected(&gateway,false);return 0;
}
