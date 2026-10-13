#include "gateway.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

/* Small bounded JSON parser. Parsing and validation finish before gateway mutation. */
typedef enum { J_OBJECT, J_ARRAY, J_STRING, J_NUMBER, J_BOOL, J_NULL } jkind;
typedef struct { jkind kind; unsigned start, end, next, children; } token;
typedef struct { const char *text; size_t length, position; token tokens[256]; unsigned count; } parser;
static void whitespace(parser *p) { while (p->position < p->length && p->text[p->position] && strchr(" \t\r\n", p->text[p->position])) p->position++; }
static int hex(char c) { return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : -1; }
static bool parse_value(parser *p, unsigned depth, unsigned *index) {
  whitespace(p);
  if (depth > 8 || p->position >= p->length || p->count >= 256) return false;
  unsigned n = p->count++; *index = n;
  token *t = &p->tokens[n]; t->start = (unsigned)p->position; t->children = 0;
  char c = p->text[p->position++];
  if (c == '{' || c == '[') {
    t->kind = c == '{' ? J_OBJECT : J_ARRAY;
    char close = c == '{' ? '}' : ']'; whitespace(p);
    if (p->position < p->length && p->text[p->position] == close) p->position++;
    else for (;;) {
      unsigned child;
      if (!parse_value(p, depth+1, &child)) return false;
      if (c == '{') {
        if (p->tokens[child].kind != J_STRING) return false;
        whitespace(p); if (p->position >= p->length || p->text[p->position++] != ':') return false;
        if (!parse_value(p, depth+1, &child)) return false;
      }
      t->children++; whitespace(p);
      if (p->position >= p->length) return false;
      char next = p->text[p->position++];
      if (next == close) break;
      if (next != ',') return false;
    }
  } else if (c == '"') {
    t->kind = J_STRING; bool ended = false;
    while (p->position < p->length) {
      unsigned char v = (unsigned char)p->text[p->position++];
      if (v == '"') { ended = true; break; }
      if (v < 0x20) return false;
      if (v == '\\') {
        if (p->position >= p->length) return false;
        char e = p->text[p->position++];
        if (e == 'u') { for (int i=0;i<4;i++) if (p->position >= p->length || hex(p->text[p->position++]) < 0) return false; }
        else if (!e || !strchr("\"\\/bfnrt", e)) return false;
      }
    }
    if (!ended) return false;
  } else if (c == '-' || (c >= '0' && c <= '9')) {
    t->kind = J_NUMBER;
    if (c == '-') { if (p->position >= p->length || p->text[p->position] < '0' || p->text[p->position] > '9') return false; c = p->text[p->position++]; }
    if (c != '0') while (p->position < p->length && p->text[p->position] >= '0' && p->text[p->position] <= '9') p->position++;
    if (p->position < p->length && strchr(".eE", p->text[p->position])) return false; /* protocol integers only */
  } else {
    const char *tail = NULL;
    if (c == 't') { t->kind=J_BOOL; tail="rue"; }
    if (c == 'f') { t->kind=J_BOOL; tail="alse"; }
    if (c == 'n') { t->kind=J_NULL; tail="ull"; }
    if (!tail || p->length-p->position < strlen(tail) || memcmp(p->text+p->position, tail, strlen(tail))) return false;
    p->position += strlen(tail);
  }
  t->end=(unsigned)p->position; t->next=p->count; return true;
}
static bool append_utf8(char *out, size_t cap, size_t *used, uint32_t cp) {
  unsigned char bytes[4]; unsigned count;
  if (cp < 0x80) { bytes[0]=(unsigned char)cp; count=1; }
  else if (cp < 0x800) { bytes[0]=0xc0|(cp>>6); bytes[1]=0x80|(cp&63); count=2; }
  else if (cp < 0x10000) { bytes[0]=0xe0|(cp>>12); bytes[1]=0x80|((cp>>6)&63); bytes[2]=0x80|(cp&63); count=3; }
  else { bytes[0]=0xf0|(cp>>18); bytes[1]=0x80|((cp>>12)&63); bytes[2]=0x80|((cp>>6)&63); bytes[3]=0x80|(cp&63); count=4; }
  if (*used+count >= cap) return false;
  memcpy(out+*used, bytes, count); *used+=count; return true;
}
static bool string_value(const parser *p, unsigned index, char *out, size_t cap, unsigned max_chars, bool printable) {
  const token *t=&p->tokens[index]; if (t->kind != J_STRING) return false;
  size_t used=0; unsigned chars=0;
  for (unsigned i=t->start+1;i<t->end-1;) {
    uint32_t cp=(unsigned char)p->text[i++];
    if (cp == '\\') {
      char escape=p->text[i++];
      switch(escape) {
        case 'b': cp=8;break; case 'f':cp=12;break; case 'n':cp=10;break; case 'r':cp=13;break; case 't':cp=9;break;
        case 'u': {
          cp=0; for (int h=0;h<4;h++) cp=(cp<<4)|(unsigned)hex(p->text[i++]);
          if (cp >= 0xd800 && cp <= 0xdbff) {
            if (i+6 > t->end-1 || p->text[i]!='\\' || p->text[i+1]!='u') return false;
            i+=2; uint32_t low=0; for (int h=0;h<4;h++) { int v=hex(p->text[i++]); if(v<0)return false; low=(low<<4)|(unsigned)v; }
            if(low<0xdc00||low>0xdfff)return false;
            cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);
          } else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
          break;
        }
        default: cp=(unsigned char)escape;break;
      }
    } else if (cp >= 0x80) {
      unsigned continuation; uint32_t minimum;
      if(cp>=0xc2&&cp<=0xdf){continuation=1;minimum=0x80;cp&=31;}
      else if(cp>=0xe0&&cp<=0xef){continuation=2;minimum=0x800;cp&=15;}
      else if(cp>=0xf0&&cp<=0xf4){continuation=3;minimum=0x10000;cp&=7;}
      else return false;
      for(unsigned j=0;j<continuation;j++) { if(i>=t->end-1)return false; unsigned char b=(unsigned char)p->text[i++];if((b&0xc0)!=0x80)return false;cp=(cp<<6)|(b&63); }
      if(cp<minimum||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return false;
    }
    if (cp == 0 || (printable && (cp<32 || (cp>=127 && cp<=159))) || ++chars > max_chars) return false;
    if (!append_utf8(out,cap,&used,cp)) return false;
  }
  out[used]=0;return true;
}
/* Returns -2 for duplicate or invalid keys, -1 for absent. */
static int property(const parser *p, unsigned object, const char *key) {
  if (p->tokens[object].kind != J_OBJECT) return -2;
  int result=-1; unsigned child=object+1;
  for(unsigned i=0;i<p->tokens[object].children;i++) {
    char name[100]; if(!string_value(p,child,name,sizeof name,99,true))return -2;
    unsigned value=child+1;
    if(!strcmp(name,key)){if(result>=0)return -2;result=(int)value;}
    child=p->tokens[value].next;
  }
  return result;
}
static bool integer(const parser *p,int index,int minimum,int maximum,int *out) {
  if(index<0||p->tokens[index].kind!=J_NUMBER)return false;
  token t=p->tokens[index]; if(t.end-t.start>11)return false;
  char s[12];memcpy(s,p->text+t.start,t.end-t.start);s[t.end-t.start]=0;
  char *end;long n=strtol(s,&end,10);if(*end||n<minimum||n>maximum)return false;*out=(int)n;return true;
}
static bool boolean(const parser *p,int index,bool *out) {if(index<0||p->tokens[index].kind!=J_BOOL)return false;*out=p->text[p->tokens[index].start]=='t';return true;}
static bool integer_array(const parser *p,int index,int minimum,int maximum,unsigned max_count,uint16_t *out,unsigned *count) {
  if(index<0||p->tokens[index].kind!=J_ARRAY||p->tokens[index].children>max_count)return false;
  unsigned child=(unsigned)index+1; *count=p->tokens[index].children;
  for(unsigned i=0;i<*count;i++){int n;if(!integer(p,(int)child,minimum,maximum,&n))return false;out[i]=(uint16_t)n;child=p->tokens[child].next;}return true;
}

typedef struct {char *data;size_t capacity,used;bool failed;} writer;
static void put(writer *w,const char *format,...) {if(w->failed)return;va_list args;va_start(args,format);int n=vsnprintf(w->data+w->used,w->capacity-w->used,format,args);va_end(args);if(n<0||(size_t)n>=w->capacity-w->used){w->failed=true;return;}w->used+=(size_t)n;}
static void quoted(writer *w,const char *s){put(w,"\"");for(;*s;s++){unsigned char c=(unsigned char)*s;if(c=='"'||c=='\\')put(w,"\\%c",c);else if(c<32)put(w,"\\u%04x",c);else put(w,"%c",c);}put(w,"\"");}
static int inventory_index(const osg_gateway_t *g,const char *id){for(unsigned i=0;i<g->module_count;i++)if(!strcmp(id,g->modules[i].id))return (int)i;return -1;}
static bool active(const osg_gateway_t *g,const osg_module_t *m){return g->running&&!g->reconnecting&&g->mounted&&g->connected&&m->registered;}
bool osg_gateway_needs_reconnect(const osg_gateway_t *g){
  if(g->module_count!=g->layout_count)return true;
  for(unsigned i=0;i<g->module_count;i++)if(!g->modules[i].registered||strcmp(g->modules[i].id,g->layout[i].id)||strcmp(g->modules[i].label,g->layout[i].label))return true;
  return false;
}
static size_t respond(const osg_gateway_t *g,const char *id,const char *error,char *response,size_t cap){
  writer w={response,cap,0,false};put(&w,"{\"id\":");quoted(&w,id);put(&w,",\"ok\":%s,\"state\":{\"backend\":\"pico\",\"protocolVersion\":%d,\"firmwareVersion\":\"%s\",\"gatewaySerial\":",error?"false":"true",OSG_PROTOCOL_VERSION,OSG_FIRMWARE_VERSION);quoted(&w,g->serial);
  put(&w,",\"running\":%s,\"reconnecting\":%s,\"needsReconnect\":%s,\"modules\":[",g->running?"true":"false",g->reconnecting?"true":"false",osg_gateway_needs_reconnect(g)?"true":"false");
  for(unsigned i=0;i<g->module_count;i++){
    const osg_module_t *m=&g->modules[i];if(i)put(&w,",");put(&w,"{\"id\":");quoted(&w,m->id);int slot=-1;for(unsigned si=0;si<g->layout_count;si++)if(!strcmp(g->layout[si].id,m->id))slot=(int)si;put(&w,",\"hidInterface\":");if(slot>=0)put(&w,"%d",2+slot);else put(&w,"null");put(&w,",\"usbSerialNumber\":");quoted(&w,g->serial);put(&w,",\"name\":");quoted(&w,m->name);put(&w,",\"kind\":");quoted(&w,m->kind);
    put(&w,",\"online\":%s,\"registered\":%s,\"active\":%s,\"buttons\":[",m->online?"true":"false",m->registered?"true":"false",active(g,m)?"true":"false");bool comma=false;for(unsigned b=0;b<m->button_count;b++)if(m->buttons&((uint32_t)1<<b)){put(&w,"%s%u",comma?",":"",b+1);comma=true;}
    put(&w,"],\"axes\":[");for(unsigned a=0;a<m->axis_count;a++)put(&w,"%s%u",a?",":"",m->axes[a]);put(&w,"],\"buttonCount\":%u,\"axisCount\":%u,\"serialNumber\":\"OSG-SIM-%s\",\"settings\":{\"label\":",m->button_count,m->axis_count,m->id);quoted(&w,m->label);put(&w,",\"debounce\":%u,\"inverted\":%s}}",m->debounce,m->inverted?"true":"false");
  }
  put(&w,"]}");if(error){put(&w,",\"error\":");quoted(&w,error);}put(&w,"}\n");
  if(w.failed){if(cap)response[0]=0;return 0;}return w.used;
}
static void neutral(osg_module_t *m){m->buttons=m->applied_buttons=0;memset(m->axes,0,sizeof m->axes);m->pending_buttons=false;m->axes_valid=false;}
static void initialize_module(osg_module_t *m,const char *id,const char *name,const char *kind,unsigned buttons,unsigned axes,bool online){
  memset(m,0,sizeof *m);snprintf(m->id,sizeof m->id,"%s",id);snprintf(m->name,sizeof m->name,"%s",name);snprintf(m->kind,sizeof m->kind,"%s",kind);m->button_count=(uint8_t)buttons;m->axis_count=(uint8_t)axes;m->online=online;m->debounce=10;snprintf(m->label,sizeof m->label,"%s",name);
}
void osg_gateway_init(osg_gateway_t *g,const char *serial){
  memset(g,0,sizeof *g);g->module_count=4;snprintf(g->serial,sizeof g->serial,"%s",serial);
  const char *ids[]={"OSG-SQ-0042","OSG-PD-0018","OSG-BP-0007","OSG-HB-0023"};
  const char *names[]={"OSG Sequential shifter","OSG Pedal set","OSG Button panel","OSG Handbrake"};
  const char *kinds[]={"shifter","pedals","panel","handbrake"};
  const uint8_t buttons[]={3,0,16,0},axes[]={0,3,0,1};
  for(unsigned i=0;i<4;i++)initialize_module(&g->modules[i],ids[i],names[i],kinds[i],buttons[i],axes[i],i!=3);
  osg_gateway_commit_layout(g);
}
void osg_gateway_commit_layout(osg_gateway_t *g){g->layout_count=g->module_count;for(unsigned i=0;i<g->module_count;i++){g->modules[i].registered=true;neutral(&g->modules[i]);g->layout[i]=g->modules[i];}g->reconnecting=false;}
void osg_gateway_disconnect(osg_gateway_t *g){g->running=false;g->watchdog_armed=false;for(unsigned i=0;i<g->module_count;i++)neutral(&g->modules[i]);}
void osg_gateway_tick(osg_gateway_t *g,uint32_t now,bool mounted,bool connected){
  g->mounted=mounted;g->connected=connected;
  if(!mounted||!connected||(g->watchdog_armed&&(uint32_t)(now-g->last_command)>=OSG_WATCHDOG_MS))osg_gateway_disconnect(g);
  for(unsigned i=0;i<g->module_count;i++){osg_module_t *m=&g->modules[i];if(m->pending_buttons&&(int32_t)(now-m->button_deadline)>=0){m->applied_buttons=m->buttons;m->pending_buttons=false;}}
}
size_t osg_gateway_report(const osg_gateway_t *g,unsigned index,uint8_t report[OSG_MAX_REPORT_SIZE]){
  if(index>=g->layout_count)return 0;
  const osg_module_t *slot=&g->layout[index];int inv=inventory_index(g,slot->id);const osg_module_t *m=inv>=0?&g->modules[inv]:slot;
  memset(report,0,OSG_MAX_REPORT_SIZE);bool enabled=inv>=0&&active(g,m)&&m->online;
  uint32_t bits=enabled?m->applied_buttons:0;
  if(!strcmp(m->kind,"shifter")&&m->inverted&&m->button_count>=3)bits=(bits&~(uint32_t)5)|((bits&1)<<2)|((bits&4)>>2);
  unsigned bytes=(slot->button_count+7)/8;for(unsigned i=0;i<bytes;i++)report[i]=(uint8_t)(bits>>(8*i));
  for(unsigned a=0;a<slot->axis_count;a++){uint16_t value=enabled&&m->axes_valid?(m->inverted?(uint16_t)(65535-m->axes[a]):m->axes[a]):0;report[bytes++]=(uint8_t)value;report[bytes++]=(uint8_t)(value>>8);}return bytes;
}
size_t osg_gateway_error(const osg_gateway_t *g,const char *message,char *response,size_t cap){return respond(g,"",message,response,cap);}
bool osg_gateway_command(osg_gateway_t *g,const char *line,size_t length,uint32_t now,char *response,size_t cap){
  /* Only the serial task calls this; keep the 5 KiB tokenizer off the MCU stack. */
  static parser p;memset(&p,0,sizeof p);p.text=line;p.length=length;unsigned root;char id[401]="",command[32]="",module_id[80]="";const char *error=NULL;
  if(length>OSG_LINE_CAPACITY||!parse_value(&p,0,&root)){error="Invalid or oversized JSON request.";goto done;}
  whitespace(&p);if(p.position!=length||p.tokens[root].kind!=J_OBJECT){error="Expected one JSON object.";goto done;}
  int id_token=property(&p,root,"id");if(id_token<0||!string_value(&p,(unsigned)id_token,id,sizeof id,100,true)||!id[0]){error="A request ID of 1-100 printable characters is required.";id[0]=0;goto done;}
  int command_token=property(&p,root,"command");if(command_token<0||!string_value(&p,(unsigned)command_token,command,sizeof command,31,true)){error="A command string is required.";goto done;}
  const char *keys[]={"moduleId","buttons","axes","online","settings","module"};int fields[6];for(unsigned i=0;i<6;i++){fields[i]=property(&p,root,keys[i]);if(fields[i]==-2){error="Duplicate or invalid request field.";goto done;}}
  int index=-1;
  if(fields[0]>=0){if(!string_value(&p,(unsigned)fields[0],module_id,sizeof module_id,79,true)){error="Invalid module ID.";goto done;}for(unsigned i=0;i<g->module_count;i++)if(!strcmp(module_id,g->modules[i].id))index=(int)i;}
  if(!strcmp(command,"hello")||!strcmp(command,"status")){}
  else if(!strcmp(command,"stop")){osg_gateway_disconnect(g);}
  else if(!strcmp(command,"start")){
    if(!g->mounted||!g->connected){error="USB serial is disconnected.";goto done;}
    if(g->reconnecting){error="Gateway is reconnecting.";goto done;}
    if(!g->running){for(unsigned i=0;i<g->module_count;i++)neutral(&g->modules[i]);g->running=true;}
  }
  else if(!strcmp(command,"reconnect")){osg_gateway_disconnect(g);g->reconnecting=true;}
  else if(!strcmp(command,"register")){
    if(module_id[0]&&strcmp(module_id,"OSG-BP-0012")){error="Use add to create a custom simulated module.";goto done;}
    if(inventory_index(g,"OSG-BP-0012")>=0){error="This module is already registered or awaiting reconnect.";goto done;}
    if(g->module_count>=OSG_MODULE_COUNT){error="The gateway supports at most 8 modules.";goto done;}
    initialize_module(&g->modules[g->module_count++],"OSG-BP-0012","OSG Auxiliary panel","panel",8,0,true);
  }
  else if(!strcmp(command,"add")){
    if(g->module_count>=OSG_MODULE_COUNT){error="The gateway supports at most 8 modules.";goto done;}
    int object=fields[5];if(object<0||p.tokens[object].kind!=J_OBJECT){error="Add requires a module definition.";goto done;}
    char new_id[41],name[321],kind[16];int buttons,axes;
    int ti=property(&p,(unsigned)object,"id"),tn=property(&p,(unsigned)object,"name"),tk=property(&p,(unsigned)object,"kind"),tb=property(&p,(unsigned)object,"buttonCount"),ta=property(&p,(unsigned)object,"axisCount");
    if(ti<0||!string_value(&p,(unsigned)ti,new_id,sizeof new_id,40,true)||!new_id[0]){error="Module ID requires 1-40 letters, digits, underscores or hyphens.";goto done;}
    for(const char *c=new_id;*c;c++)if(!((*c>='A'&&*c<='Z')||(*c>='a'&&*c<='z')||(*c>='0'&&*c<='9')||*c=='_'||*c=='-')){error="Invalid module ID.";goto done;}
    if(inventory_index(g,new_id)>=0){error="A module with this ID already exists.";goto done;}
    for(unsigned i=0;i<g->layout_count;i++)if(!strcmp(g->layout[i].id,new_id)){error="Reconnect after removal before reusing a module ID.";goto done;}
    if(tn<0||!string_value(&p,(unsigned)tn,name,sizeof name,80,true)||!name[0]||strspn(name," ")==strlen(name)){error="Module name requires 1-80 printable characters.";goto done;}
    if(tk<0||!string_value(&p,(unsigned)tk,kind,sizeof kind,15,true)||(strcmp(kind,"shifter")&&strcmp(kind,"pedals")&&strcmp(kind,"panel")&&strcmp(kind,"handbrake")&&strcmp(kind,"wheel"))){error="Invalid module kind.";goto done;}
    if(!integer(&p,tb,0,32,&buttons)||!integer(&p,ta,0,6,&axes)||buttons+axes==0){error="Module requires 0-32 buttons and 0-6 axes, with at least one control.";goto done;}
    initialize_module(&g->modules[g->module_count++],new_id,name,kind,(unsigned)buttons,(unsigned)axes,true);
  }
  else if(!strcmp(command,"remove")){
    if(index<0){error="Unknown module ID.";goto done;}
    neutral(&g->modules[index]);memmove(&g->modules[index],&g->modules[index+1],(g->module_count-(unsigned)index-1)*sizeof(osg_module_t));g->module_count--;
  }
  else if(!strcmp(command,"presence")||!strcmp(command,"configure")||!strcmp(command,"input")){
    if(index<0){error="Unknown module ID.";goto done;}osg_module_t *m=&g->modules[index];
    if(!strcmp(command,"presence")){bool online;if(!boolean(&p,fields[3],&online)){error="Presence requires an online boolean.";goto done;}m->online=online;neutral(m);}
    else if(!strcmp(command,"configure")){
      if(fields[4]<0||p.tokens[fields[4]].kind!=J_OBJECT){error="Configure requires settings.";goto done;}
      int label_token=property(&p,(unsigned)fields[4],"label"),debounce_token=property(&p,(unsigned)fields[4],"debounce"),inverted_token=property(&p,(unsigned)fields[4],"inverted");char label[321];int debounce;bool inverted;
      if(label_token<0||!string_value(&p,(unsigned)label_token,label,sizeof label,80,true)){error="Label must contain 1-80 printable characters.";goto done;}
      size_t label_start=0,label_end=strlen(label);while(label[label_start]==' ')label_start++;while(label_end>label_start&&label[label_end-1]==' ')label_end--;if(label_end==label_start){error="Label must contain 1-80 printable characters.";goto done;}label[label_end]=0;
      if(!integer(&p,debounce_token,0,100,&debounce)){error="Debounce must be 0-100 ms.";goto done;}
      if(!boolean(&p,inverted_token,&inverted)){error="Inverted must be a boolean.";goto done;}
      snprintf(m->label,sizeof m->label,"%s",label+label_start);m->debounce=(uint8_t)debounce;m->inverted=inverted;m->applied_buttons=m->buttons;m->pending_buttons=false;
    } else {
      if(!active(g,m)){error="Start the gateway and reconnect registered modules before sending HID inputs.";goto done;}if(!m->online){error="Reconnect this module to the simulated CAN chain before sending inputs.";goto done;}if(fields[1]<0&&fields[2]<0){error="Input requires buttons or axes.";goto done;}
      uint32_t bits=m->buttons;uint16_t axes[6];memcpy(axes,m->axes,sizeof axes);uint16_t values[32];unsigned count;
      if(fields[1]>=0){if(!integer_array(&p,fields[1],1,m->button_count,m->button_count,values,&count)){error="Invalid held button indices.";goto done;}bits=0;for(unsigned i=0;i<count;i++){uint32_t bit=(uint32_t)1<<(values[i]-1);if(bits&bit){error="Buttons must be unique.";goto done;}bits|=bit;}}
      if(fields[2]>=0){if(!integer_array(&p,fields[2],0,65535,m->axis_count,values,&count)||count!=m->axis_count){error="Expected the module's exact number of unsigned 16-bit axes.";goto done;}for(unsigned i=0;i<count;i++)axes[i]=values[i];}
      if(bits!=m->buttons){m->buttons=bits;m->pending_buttons=m->debounce>0;m->button_deadline=now+m->debounce;if(!m->pending_buttons)m->applied_buttons=bits;}memcpy(m->axes,axes,sizeof axes);if(fields[2]>=0)m->axes_valid=true;
    }
  } else {error="Unknown simulator command.";goto done;}
  g->last_command=now;g->watchdog_armed=g->running;
done:
  respond(g,id,error,response,cap);return error==NULL;
}
