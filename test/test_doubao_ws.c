/* Runs the actual es_ws + es_proto against a local protocol peer. */
#include "es_proto.h"
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
static es_proto_ctx_t *proto;
static es_ws_t *socket_;
static es_proto_sink_t sink;
static _Atomic int complete, connects, disconnects, errors, clears;
static _Atomic size_t samples;
static void audio(void *u,const int16_t *p,size_t n) { (void)u; for(size_t i=0;i<n;i++)if(p[i]!=0x1234)abort();samples+=n; }
static void clear(void *u) { (void)u; clears++; }
static void error(void *u,const char *code,int retry) { (void)u;(void)code;errors++;if(!retry)complete=1; }
static void json(void *u,const char *text) {
    (void)u;cJSON *root=cJSON_Parse(text),*type=cJSON_GetObjectItem(root,"type");
    if(cJSON_IsString(type)&&!strcmp(type->valuestring,"response.done"))complete=1;
    cJSON_Delete(root);
}
static void text(void *u,const char *data,size_t n) { (void)u;es_proto_on_text(proto,data,n,&sink); }
static void binary(void *u,const void *data,size_t n) { (void)u;es_proto_on_binary(proto,data,n,&sink); }
static void event(void *u,int connected,int code,const char *reason) {
    (void)u;(void)code;(void)reason;
    if(connected) { connects++;es_proto_send_start(proto,socket_); }
    else { disconnects++;es_proto_disconnected(proto); }
}
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    int reject=!strcmp(argv[2],"reject"), retry=!strcmp(argv[2],"retry");
    es_ws_opts_t o={0};o.url=argv[1];o.hdr_extra_name="X-Api-Key:";o.hdr_extra_value="local-test-key";
    o.on_event=event;o.on_text=text;o.on_binary=binary;o.reconnect=1;o.stop_on_http_4xx=1;
    sink.on_audio=audio;sink.on_clear=clear;sink.on_json=json;sink.on_error=error;
    proto=es_proto_create(ES_PROTO_DOUBAO,ES_CODEC_L16,16000,8000,"c","u",NULL);
    socket_=es_ws_create(&o);es_proto_set_ws(proto,socket_);
    if(!proto||!socket_||es_ws_start(socket_))return 3;
    int16_t pcm[160];for(int i=0;i<160;i++)pcm[i]=0x1234;
    for(int tick=0;tick<(reject?100:500)&&!complete;tick++) {
        es_proto_send_audio(proto,socket_,pcm,160);usleep(20000);
    }
    es_proto_send_stop(proto,socket_);
    for(int tick=0;tick<300&&es_proto_close_pending(proto);tick++)usleep(10000);
    int pending=es_proto_close_pending(proto);
    es_ws_destroy(socket_);es_proto_destroy(proto);es_ws_global_shutdown();
    fprintf(stderr,"mode=%s connects=%d disconnects=%d samples=%zu clears=%d errors=%d pending=%d\n",
            argv[2],connects,disconnects,samples,clears,errors,pending);
    if(reject)return connects!=0||disconnects!=1;
    return !complete||pending||connects!=(retry?2:1)||samples!=320||errors!=(retry?1:0);
}
