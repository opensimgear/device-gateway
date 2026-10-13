#include "usb_layout.h"
#include <string.h>
size_t osg_usb_string_descriptor(const osg_gateway_t *g,uint8_t index,uint16_t *out,size_t capacity){
  if(capacity<2)return 0;
  if(index==0){out[0]=(3u<<8)|4;out[1]=0x0409;return 2;}
  const char *value=NULL;
  if(index==1)value="OpenSimGear";
  else if(index==2)value="OpenSimGear Pico Gateway";
  else if(index==3)value=g->serial;
  else if(index==4)value="Cockpit configuration";
  else if(index>=5&&index<5+g->layout_count)value=g->layout[index-5].label;
  if(!value)return 0;
  if(capacity>OSG_USB_STRING_CAPACITY)capacity=OSG_USB_STRING_CAPACITY;
  size_t units=1;const unsigned char *p=(const unsigned char *)value;
  while(*p){
    uint32_t cp=*p++;unsigned continuation=0;uint32_t minimum=0;
    if(cp>=0x80){
      if(cp>=0xc2&&cp<=0xdf){continuation=1;minimum=0x80;cp&=31;}
      else if(cp>=0xe0&&cp<=0xef){continuation=2;minimum=0x800;cp&=15;}
      else if(cp>=0xf0&&cp<=0xf4){continuation=3;minimum=0x10000;cp&=7;}
      else return 0;
      for(unsigned i=0;i<continuation;i++){unsigned char byte=*p;if((byte&0xc0)!=0x80)return 0;p++;cp=(cp<<6)|(byte&63);}
      if(cp<minimum||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return 0;
    }
    size_t needed=cp>0xffff?2:1;
    if(units+needed>capacity)break; /* Truncate before a complete Unicode code point. */
    if(needed==2){cp-=0x10000;out[units++]=(uint16_t)(0xd800|(cp>>10));out[units++]=(uint16_t)(0xdc00|(cp&1023));}
    else out[units++]=(uint16_t)cp;
  }
  out[0]=(uint16_t)((3u<<8)|(2*units));return units;
}
size_t osg_hid_descriptor(unsigned buttons,unsigned axes,uint8_t *out){
  const uint8_t prefix[]={0x05,0x01,0x09,0x04,0xa1,0x01};unsigned n=0;memcpy(out,prefix,sizeof prefix);n+=sizeof prefix;
  if(buttons){const uint8_t digital[]={0x05,0x09,0x19,0x01,0x29,(uint8_t)buttons,0x15,0x00,0x25,0x01,0x75,0x01,0x95,(uint8_t)buttons,0x81,0x02};memcpy(out+n,digital,sizeof digital);n+=sizeof digital;unsigned padding=(8-buttons%8)%8;if(padding){const uint8_t pad[]={0x75,0x01,0x95,(uint8_t)padding,0x81,0x03};memcpy(out+n,pad,sizeof pad);n+=sizeof pad;}}
  if(axes){out[n++]=0x05;out[n++]=0x01;for(unsigned a=0;a<axes;a++){out[n++]=0x09;out[n++]=(uint8_t)(0x30+a);}const uint8_t analog[]={0x15,0x00,0x27,0xff,0xff,0x00,0x00,0x75,0x10,0x95,(uint8_t)axes,0x81,0x02};memcpy(out+n,analog,sizeof analog);n+=sizeof analog;}
  out[n++]=0xc0;return n;
}
void osg_usb_layout_build(osg_usb_layout_t *layout,const osg_gateway_t *g){
  memset(layout,0,sizeof *layout);unsigned total=9+66+25*g->layout_count;
  uint8_t config[]={9,2,(uint8_t)total,(uint8_t)(total>>8),(uint8_t)(2+g->layout_count),1,0,0x80,50};
  memcpy(layout->configuration,config,sizeof config);unsigned p=sizeof config;
  /* CDC ACM: interface association, control, functional descriptors, notification,
     data interface, then bulk OUT/IN. Joysticks have interrupt IN only. */
  const uint8_t cdc[]={8,11,0,2,2,2,0,4,9,4,0,0,1,2,2,0,4,5,0x24,0,0x20,0x01,5,0x24,1,0,1,4,0x24,2,2,5,0x24,6,0,1,7,5,0x81,3,8,0,16,9,4,1,0,2,0x0a,0,0,0,7,5,0x02,2,64,0,0,7,5,0x82,2,64,0,0};
  memcpy(layout->configuration+p,cdc,sizeof cdc);p+=sizeof cdc;
  for(unsigned i=0;i<g->layout_count;i++){
    layout->report_lengths[i]=(uint16_t)osg_hid_descriptor(g->layout[i].button_count,g->layout[i].axis_count,layout->reports[i]);
    uint16_t length=layout->report_lengths[i];
    const uint8_t hid[]={9,4,(uint8_t)(2+i),0,1,3,0,0,(uint8_t)(5+i),9,0x21,0x11,0x01,0,1,0x22,(uint8_t)length,(uint8_t)(length>>8),7,5,(uint8_t)(0x83+i),3,16,0,8};
    memcpy(layout->configuration+p,hid,sizeof hid);p+=sizeof hid;
  }
  layout->configuration_length=(uint16_t)p;
}
