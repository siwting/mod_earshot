/* Real adapter + real cJSON; test-only transport and rate-count resampler.
 * These are protocol regressions, not claims of a live Doubao/SIP acceptance. */
#include <switch.h>
#include "es_proto.h"
#include <pthread.h>

extern char test_ws_text[131072];
extern int test_ws_sends, test_ws_discards, test_ws_close, test_ws_binary;
static int failures, clears, jsons, errors, transcripts, final_transcripts, commands;
static size_t samples;
static int16_t last_sample;
static es_ws_t *ws = (es_ws_t *)1;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
static void audio(void *u, const int16_t *p, size_t n) { (void)u; samples += n; if(n) last_sample=p[n-1]; }
static void clear(void *u) { (void)u; clears++; }
static void json(void *u, const char *s) { (void)u; CHECK(s && *s); jsons++; }
static void error(void *u, const char *s, int retry) { (void)u; CHECK(s && *s); errors++; CHECK(retry == (*s == '5')); }
static void transcript(void *u, const char *s, int final) { (void)u; CHECK(!strcmp(s,"hello")); transcripts++; final_transcripts+=final; }
static void command(void *u,const char *a,const char *b,const char *c,const char *d) { (void)u;(void)a;(void)b;(void)c;(void)d;commands++; }
static es_proto_sink_t sink;
static void receive(es_proto_ctx_t *p, const char *s) { es_proto_on_text(p,s,strlen(s),&sink); }
static cJSON *sent(void) { cJSON *v=cJSON_Parse(test_ws_text); CHECK(v != NULL); return v; }
static int sent_type(const char *type) {
    cJSON *v=sent(), *t=cJSON_GetObjectItem(v,"type");
    int ok=cJSON_IsString(t)&&!strcmp(t->valuestring,type); cJSON_Delete(v); return ok;
}
static void ready(es_proto_ctx_t *p) { receive(p,"{\"type\":\"session.created\",\"session\":{\"id\":\"dialog-test\"}}"); }
static void delta(es_proto_ctx_t *p,const char *id,size_t n) {
    unsigned char *raw=malloc(n*2); char *b64=malloc(n*3+8), *msg=malloc(n*3+256);
    for(size_t i=0;i<n;i++) { raw[i*2]=0x34;raw[i*2+1]=0x12; }
    switch_b64_encode(raw,n*2,(unsigned char *)b64,n*3+8);
    snprintf(msg,n*3+256,"{\"type\":\"response.output_audio.delta\",\"response_id\":\"%s\",\"delta\":\"%s\"}",id,b64);
    receive(p,msg);free(raw);free(b64);free(msg);
}
static es_proto_ctx_t *create(int chan) { return es_proto_create(ES_PROTO_DOUBAO,ES_CODEC_PCMU,8000,chan,"call","uuid",NULL); }
static void *produce(void *arg) {
    int16_t pcm[160]={0};
    for(int i=0;i<10000;i++) es_proto_send_audio(arg,ws,pcm,160);
    return NULL;
}
int main(void) {
    int16_t pcm[160]; for(int i=0;i<160;i++)pcm[i]=0x1234;
    sink.on_audio=audio;sink.on_clear=clear;sink.on_json=json;sink.on_error=error;
    sink.on_transcript=transcript;sink.on_command=command;
    CHECK(es_proto_from_name("doubao")==ES_PROTO_DOUBAO);
    CHECK(!strcmp(es_proto_name(ES_PROTO_DOUBAO),"doubao"));
    CHECK(es_proto_force_codec(ES_PROTO_DOUBAO,ES_CODEC_PCMU)==ES_CODEC_L16);
    CHECK(!strcmp(es_proto_extra_header_name(ES_PROTO_DOUBAO),"X-Api-Key:"));
    CHECK(es_proto_subprotocol(ES_PROTO_DOUBAO)==NULL);
    es_proto_ctx_t *p=create(8000); CHECK(p); if(!p)return 1; es_proto_set_ws(p,ws);
    es_proto_send_audio(p,ws,pcm,160); CHECK(test_ws_sends==0);
    es_proto_send_start(p,ws); CHECK(sent_type("session.create")); CHECK(test_ws_discards==1);
    cJSON *cfg=sent(), *session=cJSON_GetObjectItem(cfg,"session"), *a=cJSON_GetObjectItem(session,"audio");
    CHECK(!strcmp(cJSON_GetObjectItem(session,"model")->valuestring,"1.2.6.1"));
    cJSON *f=cJSON_GetObjectItem(cJSON_GetObjectItem(a,"output"),"format");
    CHECK(!strcmp(cJSON_GetObjectItem(f,"type")->valuestring,"pcm_s16le"));
    CHECK(cJSON_GetObjectItem(f,"rate")->valueint==24000);
    char *saved=cJSON_PrintUnformatted(cfg);cJSON_Delete(cfg);
    es_proto_send_audio(p,ws,pcm,160); CHECK(test_ws_sends==1); /* wait for ack */
    delta(p,"pre-ack",480);CHECK(samples==0);
    ready(p); es_proto_send_audio(p,ws,pcm,160);CHECK(sent_type("input_audio_buffer.append"));
    cfg=sent();const char *b64=cJSON_GetObjectItem(cfg,"audio")->valuestring;
    char bytes[641];CHECK(switch_b64_decode((char *)b64,bytes,sizeof bytes)==640);
    CHECK((unsigned char)bytes[0]==0x34&&(unsigned char)bytes[1]==0x12);cJSON_Delete(cfg);
    int n=test_ws_sends;es_proto_send_audio(p,ws,pcm,80);CHECK(test_ws_sends==n);
    es_proto_send_audio(p,ws,pcm,80);CHECK(test_ws_sends==n+1); /* two 10ms chunks */
    delta(p,"r1",480);CHECK(samples==160);CHECK(last_sample==0x1234); /* 24k -> 8k */
    int j=jsons;delta(p,"r1",480);CHECK(jsons==j); /* no audio bodies on ESL */
    receive(p,"{\"type\":\"conversation.item.input_audio_transcription.started\"}");CHECK(clears==1);
    size_t old=samples;delta(p,"r1",480);CHECK(samples==old); /* stale tail */
    delta(p,"r2",480);CHECK(samples==old+160); /* may reply while caller still speaks */
    receive(p,"{\"type\":\"conversation.item.input_audio_transcription.delta\",\"delta\":\"hello\"}");
    receive(p,"{\"type\":\"conversation.item.input_audio_transcription.completed\",\"transcript\":\"hello\"}");
    CHECK(transcripts==2&&final_transcripts==1);
    es_proto_cancel(p,ws);CHECK(sent_type("response.cancel"));n=test_ws_sends;
    es_proto_cancel(p,ws);CHECK(test_ws_sends==n);old=samples;delta(p,"r3",480);CHECK(samples==old);
    receive(p,"{\"type\":\"response.canceled\"}");delta(p,"r2",480);CHECK(samples==old);
    delta(p,"r3",480);CHECK(samples==old+160);
    es_proto_set_muted(p,ws,1);CHECK(sent_type("input_audio_mute.commit"));n=test_ws_sends;
    es_proto_set_muted(p,ws,1);es_proto_send_audio(p,ws,pcm,160);CHECK(test_ws_sends==n);
    es_proto_set_muted(p,ws,0);CHECK(sent_type("input_audio_unmute.commit"));
    es_proto_send_audio(p,ws,pcm,160);CHECK(sent_type("input_audio_buffer.append"));
    n=test_ws_sends;es_proto_send_dtmf(p,ws,"1");CHECK(test_ws_sends==n);
    receive(p,"{\"type\":\"command\",\"action\":\"hangup\"}");CHECK(commands==0);
    CHECK(es_proto_send_control(p,ws,"{\"type\":\"conversation.item.create\",\"items\":[]}")==0);
    CHECK(sent_type("conversation.item.create"));
    CHECK(es_proto_send_control(p,ws,"{\"type\":\"input_audio_buffer.append\",\"audio\":\"bad\"}")==-1);
    CHECK(es_proto_send_control(p,ws,"{\"type\":\"session.close\"}")==-1);
    CHECK(es_proto_send_control(p,ws,"not JSON")==-1);
    char *update=strdup(saved);char *t=strstr(update,"session.create");memcpy(t,"session.update",14);
    CHECK(es_proto_send_control(p,ws,update)==0); CHECK(sent_type("session.update"));free(update);
    es_proto_disconnected(p);n=test_ws_sends;es_proto_send_audio(p,ws,pcm,160);CHECK(test_ws_sends==n);
    es_proto_send_start(p,ws);CHECK(sent_type("session.create"));
    cfg=sent();session=cJSON_GetObjectItem(cfg,"session");CHECK(!strcmp(cJSON_GetObjectItem(session,"id")->valuestring,"dialog-test"));cJSON_Delete(cfg);
    ready(p);old=samples;
    receive(p,"{\"type\":\"response.output_audio.delta\",\"response_id\":\"bad\",\"delta\":\"!!!!\"}");
    receive(p,"{\"type\":\"response.output_audio.delta\",\"response_id\":\"bad\",\"delta\":\"AA==\"}");
    es_proto_on_binary(p,pcm,sizeof pcm,&sink);CHECK(samples==old);
    receive(p,"{\"type\":\"error\",\"error\":{\"code\":42000020,\"message\":\"invalid\"}}");
    CHECK(errors==1&&test_ws_close==1);n=test_ws_sends;es_proto_send_audio(p,ws,pcm,160);CHECK(test_ws_sends==n);
    es_proto_send_start(p,ws);ready(p);
    receive(p,"{\"type\":\"error\",\"status_code\":\"55000001\"}");CHECK(errors==2&&test_ws_close==2);
    es_proto_send_start(p,ws);ready(p);es_proto_send_stop(p,ws);CHECK(sent_type("session.close"));CHECK(es_proto_close_pending(p));
    old=samples;delta(p,"late",480);CHECK(samples==old);
    receive(p,"{\"type\":\"session.closed\"}");CHECK(!es_proto_close_pending(p));CHECK(test_ws_close==1);
    /* Stop during startup must retain session.create until acknowledged. */
    es_proto_send_start(p,ws);n=test_ws_sends;int discards=test_ws_discards;
    es_proto_send_stop(p,ws);CHECK(test_ws_sends==n&&test_ws_discards==discards);
    ready(p);CHECK(sent_type("session.close"));CHECK(es_proto_close_pending(p));
    receive(p,"{\"type\":\"session.closed\"}");
    es_proto_destroy(p);
    /* Strict config validation prevents float/Opus/rate mismatch and old protocol. */
    p=es_proto_create(ES_PROTO_DOUBAO,ES_CODEC_L16,16000,16000,"c","u",saved);CHECK(p);es_proto_destroy(p);
    CHECK(!es_proto_create(ES_PROTO_DOUBAO,ES_CODEC_L16,16000,16000,"c","u","{}"));
    CHECK(!es_proto_create(ES_PROTO_DOUBAO,ES_CODEC_L16,16000,16000,"c","u","{broken"));
    char *bad=strdup(saved);t=strstr(bad,"24000");memcpy(t,"16000",5);
    CHECK(!es_proto_create(ES_PROTO_DOUBAO,ES_CODEC_L16,16000,16000,"c","u",bad));free(bad);
    cfg=cJSON_Parse(saved);session=cJSON_GetObjectItem(cfg,"session");a=cJSON_GetObjectItem(session,"audio");
    f=cJSON_GetObjectItem(cJSON_GetObjectItem(a,"output"),"format");
    cJSON_ReplaceItemInObject(f,"type",cJSON_CreateString("ogg_opus"));bad=cJSON_PrintUnformatted(cfg);
    CHECK(!es_proto_create(ES_PROTO_DOUBAO,ES_CODEC_L16,16000,16000,"c","u",bad));free(bad);cJSON_Delete(cfg);free(saved);
    /* Whole long downlink frames, no truncation (native 24k channel bypasses RX resampler). */
    p=create(24000);es_proto_set_ws(p,ws);es_proto_send_start(p,ws);ready(p);samples=0;
    delta(p,"long",30000);CHECK(samples==30000);es_proto_destroy(p);
    /* Concurrent capture + reconnect/mute state changes must not race partial PCM or cfg. */
    p=create(8000);es_proto_set_ws(p,ws);es_proto_send_start(p,ws);ready(p);
    pthread_t thread;pthread_create(&thread,NULL,produce,p);
    for(int i=0;i<100;i++) { es_proto_set_muted(p,ws,1);es_proto_disconnected(p);es_proto_send_start(p,ws);ready(p);es_proto_set_muted(p,ws,0); }
    pthread_join(thread,NULL);es_proto_destroy(p);
    CHECK(test_ws_binary==0);
    printf("doubao: %s\n",failures?"FAILED":"all passed");return !!failures;
}
