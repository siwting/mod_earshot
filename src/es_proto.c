/*
 * es_proto — protocol adapters for Earshot.  Copyright (c) 2026 Varun Pratap Singh. MIT.
 */
#include "es_proto.h"
#include "es_pb.h"      /* Pipecat protobuf codec (pure C, unit-tested) */
#include "es_cmdguard.h" /* control-channel argument validation (pure C, unit-tested) */
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <pthread.h>

struct es_proto_ctx {
    es_proto_kind_t kind;
    es_codec_t      codec;      /* wire codec (twilio forces mu-law) */
    int             rate;       /* wire send rate (caller->agent) */
    int             rate_out;   /* wire receive rate (agent->caller); differs for gemini (24k) */
    int             chan_rate;  /* channel (media-bug) L16 rate */
    es_ws_t        *ws;         /* transport, for receive-path auto-responses (pong) */
    switch_audio_resampler_t *rs_out;  /* chan_rate -> rate (caller->agent), NULL if equal */
    switch_audio_resampler_t *rs_in;   /* rate_out -> chan_rate (agent->caller), NULL if equal */
    char            stream_sid[40];
    char            account_sid[40];
    char            call_sid[40];
    char            call_id[256];      /* real correlation id (SIP Call-ID) */
    char            channel_uuid[64];
    char           *cfg;        /* verbatim session-config JSON (openai/deepgram), or NULL */
    const char     *setvar_allow; /* commands setvar: operator allowlist of settable var names (setvars=); NULL=none */
    uint64_t        out_seq;    /* Twilio sequenceNumber */
    uint64_t        out_chunk;  /* Twilio media chunk */
    uint64_t        out_ts;     /* cumulative media timestamp (ms) */
    uint8_t         agg[8192];  /* assemblyai: coalesce ~20ms frames into >=50ms chunks (AAI err 3007) */
    size_t          agg_len;
    /* Serializes Doubao media, lifecycle and control paths. Never acquire this
     * while holding the module playback mutex. Sink callbacks only take play_mutex. */
    pthread_mutex_t duplex_mu;
    int duplex_state; /* 0 disconnected, 1 creating, 2 ready, 3 closing, 4 closed/failed */
    int duplex_muted;
    int cancel_pending;
    char session_id[256];
    char response_id[256], blocked_response[256];
};

/* ---- names -------------------------------------------------------------- */
es_proto_kind_t es_proto_from_name(const char *name)
{
    if (!name)                          return ES_PROTO_NATIVE;
    if (!strcasecmp(name, "doubao"))    return ES_PROTO_DOUBAO;
    if (!strcasecmp(name, "twilio"))    return ES_PROTO_TWILIO;
    if (!strcasecmp(name, "openai"))    return ES_PROTO_OPENAI;
    if (!strcasecmp(name, "deepgram"))  return ES_PROTO_DEEPGRAM;
    if (!strcasecmp(name, "elevenlabs"))return ES_PROTO_ELEVENLABS;
    if (!strcasecmp(name, "gemini"))    return ES_PROTO_GEMINI;
    if (!strcasecmp(name, "pipecat"))   return ES_PROTO_PIPECAT;
    if (!strcasecmp(name, "vapi"))      return ES_PROTO_VAPI;
    if (!strcasecmp(name, "assemblyai"))return ES_PROTO_ASSEMBLYAI;
    if (!strcasecmp(name, "cartesia"))  return ES_PROTO_CARTESIA;
    return ES_PROTO_NATIVE;
}

const char *es_proto_name(es_proto_kind_t k)
{
    switch (k) {
    case ES_PROTO_DOUBAO:     return "doubao";
    case ES_PROTO_TWILIO:     return "twilio";
    case ES_PROTO_OPENAI:     return "openai";
    case ES_PROTO_DEEPGRAM:   return "deepgram";
    case ES_PROTO_ELEVENLABS: return "elevenlabs";
    case ES_PROTO_GEMINI:     return "gemini";
    case ES_PROTO_PIPECAT:    return "pipecat";
    case ES_PROTO_VAPI:       return "vapi";
    case ES_PROTO_ASSEMBLYAI: return "assemblyai";
    case ES_PROTO_CARTESIA:   return "cartesia";
    default:                  return "native";
    }
}

es_codec_t es_proto_force_codec(es_proto_kind_t k, es_codec_t requested)
{
    switch (k) {
    case ES_PROTO_TWILIO:                             /* Twilio Media Streams = mu-law 8k */
        return ES_CODEC_PCMU;
    case ES_PROTO_DOUBAO:                             /* PCM16 16k in / 24k out */
    case ES_PROTO_GEMINI:                             /* pcm 16k in / 24k out */
    case ES_PROTO_PIPECAT:                            /* raw L16 protobuf */
    case ES_PROTO_ASSEMBLYAI:                         /* Universal-Streaming: raw PCM16 (linear) */
    case ES_PROTO_CARTESIA:                           /* ink-whisper STT: raw PCM16 (pcm_s16le) */
        return ES_CODEC_L16;
    case ES_PROTO_OPENAI:                             /* OpenAI Realtime: g711 8k, or pcm16 (resampled) */
    case ES_PROTO_DEEPGRAM:                           /* Deepgram Voice Agent: g711 or linear16 */
    case ES_PROTO_VAPI:                               /* Vapi WS: pcm_s16le or mulaw (set at create-call) */
    case ES_PROTO_ELEVENLABS:                         /* convai: ulaw_8000 default, or pcm16 (codec=l16) */
        return (requested == ES_CODEC_PCMU || requested == ES_CODEC_PCMA ||
                requested == ES_CODEC_L16) ? requested : ES_CODEC_PCMU;
    default:
        return requested;
    }
}

const char *es_proto_extra_header_name(es_proto_kind_t k)
{
    if (k == ES_PROTO_DOUBAO) return "X-Api-Key:";
    return NULL;   /* the GA Realtime endpoint rejects the old "OpenAI-Beta: realtime=v1"
                    * header with a session error; the handshake needs only Authorization. */
}
const char *es_proto_extra_header_value(es_proto_kind_t k)
{
    (void) k;
    return NULL;
}
const char *es_proto_subprotocol(es_proto_kind_t k)
{
    /* Some endpoints answer the upgrade with a specific Sec-WebSocket-Protocol and reject a
     * mismatch: OpenAI Realtime -> "realtime", ElevenLabs convai -> "convai". Advertise exactly
     * what the server expects; other agents use no subprotocol. */
    if (k == ES_PROTO_OPENAI)     return "realtime";
    if (k == ES_PROTO_ELEVENLABS) return "convai";
    return NULL;
}

/* codec -> the vendor's audio-format object for the session handshake.
 * OpenAI Realtime GA nests formats as {"type":"audio/pcmu"|"audio/pcma"} or
 * {"type":"audio/pcm","rate":N}; the pre-GA flat "g711_ulaw"/"pcm16" strings are
 * rejected by the GA endpoint, which then assumes pcm@24k and hears our u-law
 * bytes as noise — server VAD never finds an end of turn, so no response comes. */
static void es_openai_fmt(es_codec_t c, int rate, char *out, size_t olen)
{
    if (c == ES_CODEC_L16) snprintf(out, olen, "{\"type\":\"audio/pcm\",\"rate\":%d}", rate);
    else snprintf(out, olen, "{\"type\":\"audio/%s\"}", c == ES_CODEC_PCMA ? "pcma" : "pcmu");
}
static const char *es_deepgram_fmt(es_codec_t c)
{
    return c == ES_CODEC_PCMA ? "alaw" : c == ES_CODEC_L16 ? "linear16" : "mulaw";
}

/* ---- helpers ------------------------------------------------------------ */
static void es_gen_sid(char *out, size_t olen, const char *prefix)
{
    switch_uuid_t uuid;
    char buf[SWITCH_UUID_FORMATTED_LENGTH + 1] = {0}, hex[33] = {0};
    int i, j = 0;
    switch_uuid_get(&uuid);
    switch_uuid_format(buf, &uuid);
    for (i = 0; buf[i] && j < 32; i++) if (buf[i] != '-') hex[j++] = buf[i];
    hex[j] = 0;
    snprintf(out, olen, "%s%s", prefix, hex);
}

static void es_send_text_z(es_ws_t *ws, const char *s) { es_ws_send_text(ws, s, strlen(s)); }

/* Doubao Seeduplex 3.0, not the old binary S2S protocol. Input "pcm" means
 * int16, whereas output "pcm" means float32: explicitly request pcm_s16le. */
static const char *es_doubao_default =
    "{\"type\":\"session.create\",\"session\":{\"model\":\"1.2.6.1\","
    "\"audio\":{\"input\":{\"format\":{\"type\":\"pcm\",\"rate\":16000}},"
    "\"output\":{\"format\":{\"type\":\"pcm_s16le\",\"rate\":24000},"
    "\"voice\":\"zh_female_vv_jupiter_bigtts\"}}}}";

static const char *es_string(cJSON *o, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

/* Fail before opening the socket if an override would make PCM bytes be decoded
 * as Opus/float32 or at the wrong rate. Keep prompt/tools/extension verbatim. */
static int es_doubao_config_ok(cJSON *root)
{
    cJSON *session = cJSON_GetObjectItem(root, "session");
    cJSON *audio = cJSON_GetObjectItem(session, "audio");
    cJSON *input = cJSON_GetObjectItem(audio, "input");
    cJSON *output = cJSON_GetObjectItem(audio, "output");
    cJSON *fi = cJSON_GetObjectItem(input, "format");
    cJSON *fo = cJSON_GetObjectItem(output, "format");
    cJSON *ri = cJSON_GetObjectItem(fi, "rate"), *ro = cJSON_GetObjectItem(fo, "rate");
    return (!strcmp(es_string(root, "type"), "session.create") ||
            !strcmp(es_string(root, "type"), "session.update")) &&
        !strcmp(es_string(session, "model"), "1.2.6.1") &&
        !strcmp(es_string(fi, "type"), "pcm") && cJSON_IsNumber(ri) && ri->valuedouble == 16000 &&
        !strcmp(es_string(fo, "type"), "pcm_s16le") && cJSON_IsNumber(ro) && ro->valuedouble == 24000 &&
        *es_string(output, "voice");
}

/* ---- lifecycle ---------------------------------------------------------- */
es_proto_ctx_t *es_proto_create(es_proto_kind_t kind, es_codec_t codec,
                                int wire_rate, int chan_rate,
                                const char *call_id, const char *channel_uuid, const char *cfg)
{
    es_proto_ctx_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    pthread_mutex_init(&p->duplex_mu, NULL);
    p->kind      = kind;
    p->codec     = codec;
    p->rate      = wire_rate > 0 ? wire_rate : 8000;
    p->rate_out  = p->rate;
    p->chan_rate = chan_rate > 0 ? chan_rate : 8000;
    if (kind == ES_PROTO_GEMINI) { p->rate = 16000; p->rate_out = 24000; }  /* Gemini: 16k in / 24k out */
    if (kind == ES_PROTO_PIPECAT && wire_rate <= 8000) { p->rate = 16000; p->rate_out = 16000; }  /* pipecat default 16k */
    p->cfg       = (cfg && *cfg) ? strdup(cfg) : NULL;
    if (kind == ES_PROTO_DOUBAO) {
        cJSON *config;
        p->codec = ES_CODEC_L16;
        p->rate = 16000; p->rate_out = 24000;
        if ((!cfg || !*cfg) && !p->cfg) p->cfg = strdup(es_doubao_default);
        config = p->cfg ? cJSON_ParseWithOpts(p->cfg, NULL, 1) : NULL;
        if (!config || !es_doubao_config_ok(config) || strcmp(es_string(config, "type"), "session.create")) {
            cJSON_Delete(config); es_proto_destroy(p); return NULL;
        }
        cJSON_Delete(config);
    }
    switch_copy_string(p->call_id, call_id ? call_id : "", sizeof p->call_id);
    switch_copy_string(p->channel_uuid, channel_uuid ? channel_uuid : "", sizeof p->channel_uuid);
    es_gen_sid(p->stream_sid,  sizeof p->stream_sid,  "MZ");
    es_gen_sid(p->account_sid, sizeof p->account_sid, "AC");
    es_gen_sid(p->call_sid,    sizeof p->call_sid,    "CA");

    /* transparent resampling per direction when channel/wire rates differ (e.g. gemini 8k<->16k/24k) */
    if (p->rate != p->chan_rate)
        switch_resample_create(&p->rs_out, p->chan_rate, p->rate, 8192, SWITCH_RESAMPLE_QUALITY, 1);
    if (p->rate_out != p->chan_rate)
        switch_resample_create(&p->rs_in,  p->rate_out, p->chan_rate, 8192, SWITCH_RESAMPLE_QUALITY, 1);
    if (kind == ES_PROTO_DOUBAO &&
        ((p->rate != p->chan_rate && !p->rs_out) || (p->rate_out != p->chan_rate && !p->rs_in))) {
        es_proto_destroy(p); return NULL;
    }
    return p;
}

void es_proto_set_ws(es_proto_ctx_t *p, es_ws_t *ws) { if (p) p->ws = ws; }

/* Operator allowlist of channel-variable names the agent may set via the `setvar` command.
 * The pointer is borrowed (must outlive the ctx); NULL/empty means no variable is settable. */
void es_proto_set_setvar_allow(es_proto_ctx_t *p, const char *csv) { if (p) p->setvar_allow = csv; }

void es_proto_destroy(es_proto_ctx_t *p)
{
    if (!p) return;
    if (p->rs_out) switch_resample_destroy(&p->rs_out);
    if (p->rs_in)  switch_resample_destroy(&p->rs_in);
    free(p->cfg);
    pthread_mutex_destroy(&p->duplex_mu);
    free(p);
}

void es_proto_send_start(es_proto_ctx_t *p, es_ws_t *ws)
{
    char msg[1200];
    if (!p || !ws) return;

    if (p->kind == ES_PROTO_DOUBAO) {
        cJSON *config, *session;
        char *msg;
        pthread_mutex_lock(&p->duplex_mu);
        p->duplex_state = 1; p->agg_len = 0; p->cancel_pending = 0;
        p->response_id[0] = p->blocked_response[0] = 0;
        es_ws_discard_pending(ws); /* no audio from an earlier connection before session.create */
        config = cJSON_Parse(p->cfg);
        session = cJSON_GetObjectItem(config, "session");
        cJSON_DeleteItemFromObject(config, "type");
        cJSON_AddStringToObject(config, "type", "session.create");
        if (p->session_id[0]) {
            cJSON_DeleteItemFromObject(session, "id");
            cJSON_AddStringToObject(session, "id", p->session_id);
        }
        msg = cJSON_PrintUnformatted(config);
        if (msg) { es_send_text_z(ws, msg); free(msg); }
        else p->duplex_state = 4;
        cJSON_Delete(config);
        pthread_mutex_unlock(&p->duplex_mu);
        return;
    }

    if (p->kind == ES_PROTO_TWILIO) {
        es_send_text_z(ws, "{\"event\":\"connected\",\"protocol\":\"Call\",\"version\":\"1.0.0\"}");
        snprintf(msg, sizeof msg,
            "{\"event\":\"start\",\"sequenceNumber\":\"%llu\",\"streamSid\":\"%s\","
            "\"start\":{\"streamSid\":\"%s\",\"accountSid\":\"%s\",\"callSid\":\"%s\","
            "\"tracks\":[\"inbound\"],"
            "\"customParameters\":{\"earshot_call_id\":\"%s\",\"earshot_channel_uuid\":\"%s\"},"
            "\"mediaFormat\":{\"encoding\":\"audio/x-mulaw\",\"sampleRate\":8000,\"channels\":1}}}",
            (unsigned long long)++p->out_seq, p->stream_sid, p->stream_sid,
            p->account_sid, p->call_sid, p->call_id, p->channel_uuid);
        es_send_text_z(ws, msg);
        return;
    }

    /* openai/deepgram: a full user-supplied config wins; else send an audio-format default */
    if (p->kind == ES_PROTO_OPENAI) {
        char fin[64], fout[64];
        if (p->cfg) { es_send_text_z(ws, p->cfg); return; }
        es_openai_fmt(p->codec, p->rate, fin, sizeof fin);
        es_openai_fmt(p->codec, p->rate_out, fout, sizeof fout);
        snprintf(msg, sizeof msg,
            "{\"type\":\"session.update\",\"session\":{\"type\":\"realtime\","
            "\"output_modalities\":[\"audio\"],"
            "\"audio\":{\"input\":{\"format\":%s,\"turn_detection\":{\"type\":\"server_vad\"}},"
            "\"output\":{\"format\":%s}}}}",
            fin, fout);
        es_send_text_z(ws, msg);
        return;
    }

    if (p->kind == ES_PROTO_DEEPGRAM) {
        if (p->cfg) { es_send_text_z(ws, p->cfg); return; }
        snprintf(msg, sizeof msg,
            "{\"type\":\"Settings\",\"audio\":{"
            "\"input\":{\"encoding\":\"%s\",\"sample_rate\":%d},"
            "\"output\":{\"encoding\":\"%s\",\"sample_rate\":%d,\"container\":\"none\"}},"
            "\"agent\":{\"language\":\"en\","
            "\"listen\":{\"provider\":{\"type\":\"deepgram\",\"model\":\"nova-3\"}},"
            "\"think\":{\"provider\":{\"type\":\"open_ai\",\"model\":\"gpt-4o-mini\"}},"
            "\"speak\":{\"provider\":{\"type\":\"deepgram\",\"model\":\"aura-2-thalia-en\"}}}}",
            es_deepgram_fmt(p->codec), p->rate, es_deepgram_fmt(p->codec), p->rate);
        es_send_text_z(ws, msg);
        return;
    }

    if (p->kind == ES_PROTO_GEMINI) {                 /* setup must be the first message */
        if (p->cfg) { es_send_text_z(ws, p->cfg); return; }
        /* Live model names churn fast; the "…-latest" alias is the most stable default. Operators
         * normally supply model/voice/system-prompt via EARSHOT_SESSION_CONFIG (sent verbatim above). */
        es_send_text_z(ws,
            "{\"setup\":{\"model\":\"models/gemini-2.5-flash-native-audio-latest\","
            "\"generationConfig\":{\"responseModalities\":[\"AUDIO\"]}}}");
        return;
    }

    if (p->kind == ES_PROTO_ELEVENLABS) {             /* optional init override */
        if (p->cfg) es_send_text_z(ws, p->cfg);       /* else the agent uses its configured defaults */
        return;
    }

    /* native / pipecat: no preamble */
}

void es_proto_send_stop(es_proto_ctx_t *p, es_ws_t *ws)
{
    char msg[512];
    if (!p || !ws) return;
    if (p->kind == ES_PROTO_DOUBAO) {
        pthread_mutex_lock(&p->duplex_mu);
        if (p->duplex_state == 1) {
            /* Keep the unsent create frame. Send close when its acknowledgement arrives. */
            p->duplex_state = 3; p->agg_len = 0;
        } else if (p->duplex_state == 2) {
            p->duplex_state = 3; p->agg_len = 0;
            es_ws_discard_pending(ws);
            es_send_text_z(ws, "{\"type\":\"session.close\"}");
        }
        pthread_mutex_unlock(&p->duplex_mu);
        return;
    }
    if (p->kind != ES_PROTO_TWILIO) return;
    snprintf(msg, sizeof msg,
        "{\"event\":\"stop\",\"sequenceNumber\":\"%llu\",\"streamSid\":\"%s\","
        "\"stop\":{\"accountSid\":\"%s\",\"callSid\":\"%s\"}}",
        (unsigned long long)++p->out_seq, p->stream_sid, p->account_sid, p->call_sid);
    es_send_text_z(ws, msg);
}

void es_proto_send_mark(es_proto_ctx_t *p, es_ws_t *ws, const char *name)
{
    char msg[256];
    if (!p || !ws || p->kind != ES_PROTO_TWILIO || !name) return;
    snprintf(msg, sizeof msg,
        "{\"event\":\"mark\",\"streamSid\":\"%s\",\"mark\":{\"name\":\"%s\"}}",
        p->stream_sid, name);
    es_send_text_z(ws, msg);
}

void es_proto_send_dtmf(es_proto_ctx_t *p, es_ws_t *ws, const char *digit)
{
    char msg[256];
    if (!p || !ws || !digit || p->kind == ES_PROTO_DOUBAO) return;
    if (p->kind == ES_PROTO_TWILIO)
        snprintf(msg, sizeof msg,
                 "{\"event\":\"dtmf\",\"streamSid\":\"%s\",\"dtmf\":{\"track\":\"inbound\",\"digit\":\"%s\"}}",
                 p->stream_sid, digit);
    else
        snprintf(msg, sizeof msg, "{\"type\":\"dtmf\",\"digit\":\"%s\"}", digit);
    es_send_text_z(ws, msg);
}

void es_proto_disconnected(es_proto_ctx_t *p)
{
    if (!p || p->kind != ES_PROTO_DOUBAO) return;
    pthread_mutex_lock(&p->duplex_mu);
    p->duplex_state = 0; p->agg_len = 0; p->cancel_pending = 0;
    if (p->ws) es_ws_discard_pending(p->ws);
    pthread_mutex_unlock(&p->duplex_mu);
}

void es_proto_set_muted(es_proto_ctx_t *p, es_ws_t *ws, int muted)
{
    if (!p || p->kind != ES_PROTO_DOUBAO) return;
    pthread_mutex_lock(&p->duplex_mu);
    if (p->duplex_muted != !!muted) {
        p->duplex_muted = !!muted; p->agg_len = 0;
        if (ws && p->duplex_state == 2)
            es_send_text_z(ws, muted ? "{\"type\":\"input_audio_mute.commit\"}" :
                                      "{\"type\":\"input_audio_unmute.commit\"}");
    }
    pthread_mutex_unlock(&p->duplex_mu);
}

void es_proto_cancel(es_proto_ctx_t *p, es_ws_t *ws)
{
    if (!p || !ws || p->kind != ES_PROTO_DOUBAO) return;
    pthread_mutex_lock(&p->duplex_mu);
    if (p->duplex_state == 2 && !p->cancel_pending) {
        p->cancel_pending = 1;
        switch_copy_string(p->blocked_response, p->response_id, sizeof p->blocked_response);
        es_send_text_z(ws, "{\"type\":\"response.cancel\"}");
    }
    pthread_mutex_unlock(&p->duplex_mu);
}

int es_proto_close_pending(es_proto_ctx_t *p)
{
    int pending;
    if (!p || p->kind != ES_PROTO_DOUBAO) return 0;
    pthread_mutex_lock(&p->duplex_mu);
    pending = p->duplex_state == 3;
    pthread_mutex_unlock(&p->duplex_mu);
    return pending;
}

int es_proto_send_control(es_proto_ctx_t *p, es_ws_t *ws, const char *text)
{
    cJSON *root;
    const char *type;
    int rc = -1;
    if (!p || !ws || !text) return -1;
    if (p->kind != ES_PROTO_DOUBAO) return es_ws_send_text(ws, text, strlen(text));
    root = cJSON_ParseWithOpts(text, NULL, 1);
    if (!root) return -1;
    type = es_string(root, "type");
    /* Session format/lifecycle changes require a fresh start; tools and prompt can
     * be supplied at start. Expose documented business events without an agent service. */
    if (!strcmp(type, "response.cancel")) {
        es_proto_cancel(p, ws); rc = 0;
    } else if (!strcmp(type, "session.update") && es_doubao_config_ok(root)) {
        char *cfg = cJSON_PrintUnformatted(root);
        pthread_mutex_lock(&p->duplex_mu);
        if (cfg && p->duplex_state == 2) {
            rc = es_ws_send_text(ws, cfg, strlen(cfg));
            if (!rc) { free(p->cfg); p->cfg = cfg; cfg = NULL; }
        }
        pthread_mutex_unlock(&p->duplex_mu);
        free(cfg);
    } else if (!strcmp(type, "conversation.item.create") ||
               !strcmp(type, "conversation.item.update") ||
               !strcmp(type, "conversation.item.retrieve") ||
               !strcmp(type, "conversation.item.delete") ||
               !strcmp(type, "speech_text_buffer.commit") ||
               !strcmp(type, "speech_text_buffer.replacement.append") ||
               !strcmp(type, "speech_text_buffer.replacement.commit") ||
               !strcmp(type, "input_audio_buffer.commit")) {
        pthread_mutex_lock(&p->duplex_mu);
        if (p->duplex_state == 2) rc = es_ws_send_text(ws, text, strlen(text));
        pthread_mutex_unlock(&p->duplex_mu);
    }
    cJSON_Delete(root);
    return rc;
}

/* Accumulate exactly 20ms at 16kHz. Called only by the media thread, with the
 * duplex mutex held; lifecycle transitions cannot race the partial packet. */
static void es_doubao_send_audio(es_proto_ctx_t *p, es_ws_t *ws, const int16_t *pcm, size_t ns)
{
    while (ns) {
        size_t take = (640 - p->agg_len) / 2;
        if (take > ns) take = ns;
        es_encode(ES_CODEC_L16, pcm, take, p->agg + p->agg_len);
        p->agg_len += take * 2; pcm += take; ns -= take;
        if (p->agg_len == 640) {
            char b64[860], msg[960];
            switch_b64_encode(p->agg, 640, (unsigned char *)b64, sizeof b64);
            snprintf(msg, sizeof msg, "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}", b64);
            es_send_text_z(ws, msg);
            p->agg_len = 0;
        }
    }
}

/* ---- outbound audio (caller -> agent) ----------------------------------- */
void es_proto_send_audio(es_proto_ctx_t *p, es_ws_t *ws, const int16_t *pcm, size_t nsamples)
{
    uint8_t enc[SWITCH_RECOMMENDED_BUFFER_SIZE];
    size_t  nb;
    if (!p || !ws || !pcm || !nsamples) return;

    if (p->kind == ES_PROTO_DOUBAO) {
        pthread_mutex_lock(&p->duplex_mu);
        if (p->duplex_state == 2 && !p->duplex_muted) {
            /* Bound resampler input, but never truncate a larger captured frame. */
            while (nsamples) {
                size_t n = nsamples > 4096 ? 4096 : nsamples;
                if (p->rs_out) {
                    uint32_t count = switch_resample_process(p->rs_out, (int16_t *)pcm, (uint32_t)n);
                    es_doubao_send_audio(p, ws, p->rs_out->to, count);
                } else es_doubao_send_audio(p, ws, pcm, n);
                pcm += n; nsamples -= n;
            }
        }
        pthread_mutex_unlock(&p->duplex_mu);
        return;
    }

    /* channel-rate L16 -> wire-rate L16 (no-op when the rates match) */
    if (p->rs_out) {
        uint32_t on = switch_resample_process(p->rs_out, (int16_t *) pcm, (uint32_t) nsamples);
        pcm = p->rs_out->to;
        nsamples = on;
        if (!nsamples) return;
    }
    /* bound the encode: enc[] holds at most sizeof(enc)/2 L16 samples (the worst case) */
    if (nsamples > sizeof(enc) / 2) nsamples = sizeof(enc) / 2;

    if (p->kind == ES_PROTO_TWILIO) {
        char b64[2 * SWITCH_RECOMMENDED_BUFFER_SIZE];
        char msg[3 * SWITCH_RECOMMENDED_BUFFER_SIZE];
        int  n;
        nb = es_encode(ES_CODEC_PCMU, pcm, nsamples, enc);
        if (nb == (size_t) -1) return;
        switch_b64_encode(enc, nb, (unsigned char *) b64, sizeof b64);
        n = snprintf(msg, sizeof msg,
            "{\"event\":\"media\",\"sequenceNumber\":\"%llu\",\"streamSid\":\"%s\","
            "\"media\":{\"track\":\"inbound\",\"chunk\":\"%llu\",\"timestamp\":\"%llu\",\"payload\":\"%s\"}}",
            (unsigned long long)++p->out_seq, p->stream_sid,
            (unsigned long long)++p->out_chunk, (unsigned long long)p->out_ts, b64);
        if (n > 0) es_ws_send_text(ws, msg, (size_t) n);
        p->out_ts += (p->rate >= 1000) ? nsamples / (p->rate / 1000) : 0;   /* advance ms */
        return;
    }

    if (p->kind == ES_PROTO_OPENAI || p->kind == ES_PROTO_ELEVENLABS || p->kind == ES_PROTO_GEMINI) {
        char b64[2 * SWITCH_RECOMMENDED_BUFFER_SIZE];
        char msg[3 * SWITCH_RECOMMENDED_BUFFER_SIZE];
        int  n;
        nb = es_encode(p->codec, pcm, nsamples, enc);   /* codec: openai g711/l16, 11labs ulaw|l16, gemini l16 */
        if (nb == (size_t) -1) return;
        switch_b64_encode(enc, nb, (unsigned char *) b64, sizeof b64);
        if (p->kind == ES_PROTO_OPENAI)
            n = snprintf(msg, sizeof msg, "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}", b64);
        else if (p->kind == ES_PROTO_ELEVENLABS)
            n = snprintf(msg, sizeof msg, "{\"user_audio_chunk\":\"%s\"}", b64);
        else  /* gemini: current Live API uses realtimeInput.audio (the old mediaChunks[] is dropped
               * by 2.5+ models); automatic activity detection handles turn boundaries on silence */
            n = snprintf(msg, sizeof msg,
                "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=%d\",\"data\":\"%s\"}}}",
                p->rate, b64);
        if (n > 0) es_ws_send_text(ws, msg, (size_t) n);
        return;
    }

    if (p->kind == ES_PROTO_PIPECAT) {                    /* protobuf Frame{ audio: AudioRawFrame } */
        uint8_t frame[2 * SWITCH_RECOMMENDED_BUFFER_SIZE + 64];
        size_t flen;
        nb = es_encode(ES_CODEC_L16, pcm, nsamples, enc);       /* raw L16 */
        if (nb == (size_t) -1) return;
        flen = es_pb_encode_audio(frame, sizeof frame, enc, nb, (uint32_t) p->rate);
        if (flen) es_ws_send_binary(ws, frame, flen);
        return;
    }

    if (p->kind == ES_PROTO_ASSEMBLYAI || p->kind == ES_PROTO_CARTESIA) {
        /* Both STT vendors want >=50ms (Cartesia ~100ms) per message; our media frames are ~20ms,
         * so coalesce to ~100ms before sending (AssemblyAI else closes with error 3007). */
        size_t target = (size_t) p->rate / 10 * 2;         /* ~100ms of L16 @ wire rate (bytes) */
        nb = es_encode(ES_CODEC_L16, pcm, nsamples, enc);  /* PCM16 */
        if (nb == (size_t) -1) return;
        if (p->agg_len + nb > sizeof(p->agg)) {            /* never overflow the coalesce buffer */
            es_ws_send_binary(ws, p->agg, p->agg_len);
            p->agg_len = 0;
        }
        memcpy(p->agg + p->agg_len, enc, nb);
        p->agg_len += nb;
        if (p->agg_len >= target) {
            es_ws_send_binary(ws, p->agg, p->agg_len);
            p->agg_len = 0;
        }
        return;
    }

    /* native + deepgram + vapi: raw codec-coded binary frame */
    nb = es_encode(p->codec, pcm, nsamples, enc);
    if (nb != (size_t) -1) es_ws_send_binary(ws, enc, nb);
}

/* ---- inbound (agent -> caller) ------------------------------------------ */
/* deliver wire-rate L16 to the sink, resampling down to the channel rate first if needed */
static void es_emit_audio(es_proto_ctx_t *p, int16_t *pcm, size_t ns, const es_proto_sink_t *sink)
{
    if (!ns || !sink || !sink->on_audio) return;
    if (p->rs_in) {
        uint32_t cn = switch_resample_process(p->rs_in, pcm, (uint32_t) ns);
        if (cn) sink->on_audio(sink->user, p->rs_in->to, cn);
    } else {
        sink->on_audio(sink->user, pcm, ns);
    }
}

/* Decode coded audio of any length and hand it to the sink in pieces that fit the
 * fixed decode buffer. Earlier versions clamped the input to one buffer (8 KB of
 * L16 = 256 ms at 16 kHz) and silently dropped the rest, which chopped any agent
 * that sends a whole utterance in one frame. */
static void es_decode_chunked(es_proto_ctx_t *p, es_codec_t codec, const uint8_t *in, size_t nbytes,
                              const es_proto_sink_t *sink)
{
    int16_t pcm[SWITCH_RECOMMENDED_BUFFER_SIZE];
    size_t step = es_encoded_size(codec, sizeof(pcm) / sizeof(pcm[0]));   /* bytes that fill pcm[] */
    if (!step || step == (size_t) -1) return;
    while (nbytes) {
        size_t take = nbytes < step ? nbytes : step, ns;
        if (codec == ES_CODEC_L16) take &= ~(size_t) 1;        /* whole samples only */
        if (!take) break;                                      /* a dangling odd byte */
        ns = es_decode(codec, in, take, pcm);
        if (ns == (size_t) -1 || !ns) break;
        es_emit_audio(p, pcm, ns, sink);
        in += take; nbytes -= take;
    }
}

static void es_deliver_ulaw_b64(es_proto_ctx_t *p, const char *b64, es_codec_t codec,
                                const es_proto_sink_t *sink)
{
    char    stack_raw[2 * SWITCH_RECOMMENDED_BUFFER_SIZE];
    char   *raw = stack_raw;
    switch_size_t rawsz = sizeof stack_raw, nb, blen, pad = 0, expected;
    if (!b64 || !sink || !sink->on_audio) return;
    blen = strlen(b64);
    if (blen / 4 * 3 + 4 > rawsz) {                /* a long utterance in one message: heap it */
        rawsz = blen / 4 * 3 + 4;
        raw = malloc(rawsz);
        if (!raw) return;
    }
    nb = switch_b64_decode((char *) b64, raw, rawsz);
    if (!nb) { if (raw != stack_raw) free(raw); return; }
    /* switch_b64_decode's length convention varies by version; derive the exact
     * decoded size from the base64 string so we never keep a spurious trailing
     * byte (which would grow the buffer over a long call and click as loud mu-law). */
    if (blen >= 1 && b64[blen - 1] == '=') pad++;
    if (blen >= 2 && b64[blen - 2] == '=') pad++;
    expected = blen / 4 * 3 - pad;
    if (nb > expected) nb = expected;
    es_decode_chunked(p, codec, (const uint8_t *) raw, nb, sink);
    if (raw != stack_raw) free(raw);
}

static es_codec_t es_codec_from_wire(const char *name, es_codec_t deflt)
{
    if (!name) return deflt;
    if (!strcasecmp(name, "audio/x-mulaw") || !strcasecmp(name, "mulaw") || !strcasecmp(name, "pcmu"))
        return ES_CODEC_PCMU;
    if (!strcasecmp(name, "audio/x-alaw") || !strcasecmp(name, "alaw") || !strcasecmp(name, "pcma"))
        return ES_CODEC_PCMA;
    if (!strcasecmp(name, "l16") || !strcasecmp(name, "linear")) return ES_CODEC_L16;
    return deflt;
}

static const char *es_json_str(cJSON *o, const char *k)
{
    cJSON *i = cJSON_GetObjectItem(o, k);
    return (i && i->valuestring) ? i->valuestring : "";
}

/* Reject an argument that would escalate a whitelisted uuid_* call into code execution,
 * a path escape, or a privileged side effect. Reports the action blocked (api==NULL) with
 * a reason the sink surfaces in the earshot::command audit. */
#define ES_CMD_BLOCK(reason) do { sink->on_command(sink->user, action, NULL, (reason), id); return; } while (0)

/* Translate an agent {"type":"command",...} into a whitelisted uuid_* API call and hand it to
 * the sink. Only these call-control actions are allowed (anything else is reported unsupported,
 * api==NULL); every argument is validated (es_cmdguard) because the action whitelist is not a
 * security boundary on its own. The uuid is the channel this stream is tapping. */
static void es_dispatch_command(es_proto_ctx_t *p, cJSON *root, const es_proto_sink_t *sink)
{
    const char *action = es_json_str(root, "action");
    const char *id     = es_json_str(root, "id");
    const char *uuid   = p->channel_uuid;
    char api[32] = "", arg[1024] = "";
    if (!sink->on_command) return;

    if (!strcasecmp(action, "transfer")) {
        const char *to = *es_json_str(root, "to") ? es_json_str(root, "to") : es_json_str(root, "extension");
        const char *dp = es_json_str(root, "dialplan");
        const char *cx = es_json_str(root, "context");
        int n;
        if (!es_cmd_token_ok(to))        ES_CMD_BLOCK("rejected: transfer destination");
        if (!es_cmd_dialplan_ok(dp))     ES_CMD_BLOCK("rejected: transfer dialplan"); /* blocks the `inline` app-exec */
        if (*cx && !es_cmd_token_ok(cx)) ES_CMD_BLOCK("rejected: transfer context");
        n = snprintf(arg, sizeof arg, "%s %s", uuid, to);
        if (n < 0 || n >= (int) sizeof arg) n = (int) sizeof arg - 1;   /* clamp against oversized fields */
        if (*dp) n += snprintf(arg + n, sizeof arg - n, " %s", dp);
        if (n < 0 || n >= (int) sizeof arg) n = (int) sizeof arg - 1;
        if (*cx) snprintf(arg + n, sizeof arg - n, " %s", cx);
        snprintf(api, sizeof api, "uuid_transfer");
    } else if (!strcasecmp(action, "hangup")) {
        const char *c = es_json_str(root, "cause");
        if (!es_cmd_text_ok(c)) ES_CMD_BLOCK("rejected: hangup cause");
        snprintf(api, sizeof api, "uuid_kill");
        snprintf(arg, sizeof arg, "%s %s", uuid, *c ? c : "NORMAL_CLEARING");
    } else if (!strcasecmp(action, "send_dtmf") || !strcasecmp(action, "dtmf")) {
        const char *digits = es_json_str(root, "digits");
        if (!es_cmd_token_ok(digits)) ES_CMD_BLOCK("rejected: dtmf digits");
        snprintf(api, sizeof api, "uuid_send_dtmf");
        snprintf(arg, sizeof arg, "%s %s", uuid, digits);
    } else if (!strcasecmp(action, "play") || !strcasecmp(action, "broadcast")) {
        const char *file = es_json_str(root, "file");
        const char *leg  = es_json_str(root, "leg");
        if (!es_cmd_token_ok(file)) ES_CMD_BLOCK("rejected: play file (app-exec/empty)");
        if (!es_cmd_leg_ok(leg))    ES_CMD_BLOCK("rejected: play leg");
        snprintf(api, sizeof api, "uuid_broadcast");
        snprintf(arg, sizeof arg, "%s %s %s", uuid, file, *leg ? leg : "aleg");
    } else if (!strcasecmp(action, "stop_play") || !strcasecmp(action, "break")) {
        snprintf(api, sizeof api, "uuid_break");
        snprintf(arg, sizeof arg, "%s", uuid);
    } else if (!strcasecmp(action, "record")) {
        const char *state = es_json_str(root, "state");
        const char *file  = es_json_str(root, "file");
        if (*state && strcasecmp(state, "start") && strcasecmp(state, "stop") &&
            strcasecmp(state, "mask") && strcasecmp(state, "unmask")) ES_CMD_BLOCK("rejected: record state");
        if (!es_cmd_recpath_ok(file)) ES_CMD_BLOCK("rejected: record path (app-exec/traversal)");
        snprintf(api, sizeof api, "uuid_record");
        snprintf(arg, sizeof arg, "%s %s %s", uuid, *state ? state : "start", file);
    } else if (!strcasecmp(action, "setvar")) {
        const char *name  = es_json_str(root, "name");
        const char *value = es_json_str(root, "value");
        /* A name denylist cannot secure setvar: identifier-clean variables like
         * transfer_after_bridge reach the `inline` dialplan (RCE) through their VALUE. So the
         * settable names are an operator allowlist (setvars=), fail-closed when unset. */
        if (!es_cmd_varname_ok(name))                    ES_CMD_BLOCK("rejected: setvar name");
        if (!es_cmd_name_in_list(p->setvar_allow, name)) ES_CMD_BLOCK("rejected: setvar not in setvars= allowlist");
        if (!es_cmd_text_ok(value))                      ES_CMD_BLOCK("rejected: setvar value");
        snprintf(api, sizeof api, "uuid_setvar");
        snprintf(arg, sizeof arg, "%s %s %s", uuid, name, value);
    } else if (!strcasecmp(action, "hold")) {
        const char *state = es_json_str(root, "state");
        snprintf(api, sizeof api, "uuid_hold");
        if (!strcasecmp(state, "off") || !strcasecmp(state, "unhold")) snprintf(arg, sizeof arg, "off %s", uuid);
        else snprintf(arg, sizeof arg, "%s", uuid);
    } else if (!strcasecmp(action, "bridge")) {
        const char *peer = es_json_str(root, "peer_uuid");
        if (!es_cmd_token_ok(peer)) ES_CMD_BLOCK("rejected: bridge peer_uuid");
        snprintf(api, sizeof api, "uuid_bridge");
        snprintf(arg, sizeof arg, "%s %s", uuid, peer);
    } else if (!strcasecmp(action, "park")) {
        snprintf(api, sizeof api, "uuid_park");
        snprintf(arg, sizeof arg, "%s", uuid);
    } else if (!strcasecmp(action, "mask")) {
        snprintf(api, sizeof api, "__mask__");    /* earshot-internal; handled by the sink */
        snprintf(arg, sizeof arg, "%s", es_json_str(root, "state"));
    } else {
        sink->on_command(sink->user, action, NULL, NULL, id);   /* unsupported */
        return;
    }
    sink->on_command(sink->user, action, api, arg, id);
}

#undef ES_CMD_BLOCK

/* All downlink state transitions and playback delivery are serialized with
 * local cancellation, so queued old audio cannot slip in after a flush. */
static void es_doubao_on_text(es_proto_ctx_t *p, cJSON *root, const char *data, const es_proto_sink_t *sink)
{
    const char *type = es_string(root, "type");
    const char *rid = es_string(root, "response_id");
    int publish = strcmp(type, "response.output_audio.delta") != 0;
    pthread_mutex_lock(&p->duplex_mu);
    if (!strcmp(type, "session.created") && p->duplex_state == 3) {
        if (p->ws) es_send_text_z(p->ws, "{\"type\":\"session.close\"}");
    } else if (!strcmp(type, "session.created") && p->duplex_state == 1) {
        cJSON *session = cJSON_GetObjectItem(root, "session");
        switch_copy_string(p->session_id, es_string(session, "id"), sizeof p->session_id);
        p->duplex_state = 2;
        if (p->duplex_muted && p->ws)
            es_send_text_z(p->ws, "{\"type\":\"input_audio_mute.commit\"}");
    } else if (!strcmp(type, "session.closed")) {
        p->duplex_state = 4;
        if (p->ws) es_ws_request_close(p->ws, 0);
    } else if (!strcmp(type, "error")) {
        cJSON *err = cJSON_GetObjectItem(root, "error");
        cJSON *code = cJSON_GetObjectItem(err ? err : root, "code");
        char buf[32]; const char *value;
        if (!code) code = cJSON_GetObjectItem(err ? err : root, "status_code");
        snprintf(buf, sizeof buf, "%d", cJSON_IsNumber(code) ? code->valueint : 0);
        value = cJSON_IsString(code) ? code->valuestring : buf;
        p->duplex_state = 4;
        if (sink->on_clear) sink->on_clear(sink->user);
        if (sink->on_error) sink->on_error(sink->user, value, value[0] == '5');
        if (p->ws) es_ws_request_close(p->ws, value[0] == '5');
    } else if (p->duplex_state == 2) {
        if (!strcmp(type, "conversation.item.input_audio_transcription.started")) {
            switch_copy_string(p->blocked_response, p->response_id, sizeof p->blocked_response);
            if (sink->on_clear) sink->on_clear(sink->user);
        } else if (!strcmp(type, "response.canceled")) {
            p->cancel_pending = 0;
            if (sink->on_clear) sink->on_clear(sink->user);
        } else if (!strcmp(type, "response.output_audio.started")) {
            if (*rid) switch_copy_string(p->response_id, rid, sizeof p->response_id);
        } else if (!strcmp(type, "response.output_audio.delta")) {
            const char *b64 = es_string(root, "delta");
            if (*rid && strlen(rid) < sizeof p->response_id && !p->cancel_pending &&
                (!p->blocked_response[0] || strcmp(rid, p->blocked_response))) {
                switch_copy_string(p->response_id, rid, sizeof p->response_id);
                /* Reject malformed base64 and incomplete PCM samples instead of playing noise. */
                size_t n = strlen(b64), pad = 0, i;
                if (n && b64[n-1] == '=') pad++;
                if (n > 1 && b64[n-2] == '=') pad++;
                for (i = 0; i < n - pad; i++)
                    if (!strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", b64[i])) break;
                if (n && !(n % 4) && i == n - pad && !((n / 4 * 3 - pad) % 2))
                    es_deliver_ulaw_b64(p, b64, ES_CODEC_L16, sink);
            }
        } else if (!strcmp(type, "conversation.item.input_audio_transcription.delta")) {
            if (sink->on_transcript) sink->on_transcript(sink->user, es_string(root, "delta"), 0);
        } else if (!strcmp(type, "conversation.item.input_audio_transcription.completed")) {
            if (sink->on_transcript) sink->on_transcript(sink->user, es_string(root, "transcript"), 1);
        }
    }
    pthread_mutex_unlock(&p->duplex_mu);
    if (publish && sink->on_json) sink->on_json(sink->user, data);
}

void es_proto_on_text(es_proto_ctx_t *p, const char *data, size_t len, const es_proto_sink_t *sink)
{
    cJSON *root, *ev, *node;
    const char *e;
    (void) len;
    if (!p || !data || !sink) return;
    if (!(root = cJSON_Parse(data))) return;

    if (p->kind == ES_PROTO_DOUBAO) {
        es_doubao_on_text(p, root, data, sink);
        cJSON_Delete(root);
        return; /* Never execute vendor function-call text as a FreeSWITCH command. */
    }

    /* Control channel: {"type":"command",...} is honored in every protocol (earshot-specific;
     * a vendor never sends this shape, so it's a safe universal escape hatch). */
    node = cJSON_GetObjectItem(root, "type");
    if (node && node->valuestring && !strcasecmp(node->valuestring, "command")) {
        es_dispatch_command(p, root, sink);
        cJSON_Delete(root);
        return;
    }

    if (p->kind == ES_PROTO_TWILIO) {
        ev = cJSON_GetObjectItem(root, "event");
        e  = (ev && ev->valuestring) ? ev->valuestring : "";
        if (!strcasecmp(e, "media")) {
            cJSON *media = cJSON_GetObjectItem(root, "media");
            cJSON *pl = media ? cJSON_GetObjectItem(media, "payload") : NULL;
            if (pl && pl->valuestring) es_deliver_ulaw_b64(p, pl->valuestring, ES_CODEC_PCMU, sink);
        } else if (!strcasecmp(e, "clear")) {
            if (sink->on_clear) sink->on_clear(sink->user);
        } else if (!strcasecmp(e, "mark")) {
            cJSON *m = cJSON_GetObjectItem(root, "mark");
            cJSON *nm = m ? cJSON_GetObjectItem(m, "name") : NULL;
            if (nm && nm->valuestring && sink->on_mark) sink->on_mark(sink->user, nm->valuestring);
        } else if (!strcasecmp(e, "dtmf")) {
            cJSON *d = cJSON_GetObjectItem(root, "dtmf");
            cJSON *dg = d ? cJSON_GetObjectItem(d, "digit") : NULL;
            if (dg && dg->valuestring && sink->on_dtmf) sink->on_dtmf(sink->user, dg->valuestring);
        }
        cJSON_Delete(root);
        return;
    }

    if (p->kind == ES_PROTO_OPENAI) {
        node = cJSON_GetObjectItem(root, "type");
        e = (node && node->valuestring) ? node->valuestring : "";
        if (!strcmp(e, "response.output_audio.delta") ||       /* agent audio (base64), GA name */
            !strcmp(e, "response.audio.delta")) {              /* legacy beta name */
            cJSON *d = cJSON_GetObjectItem(root, "delta");
            if (d && d->valuestring) es_deliver_ulaw_b64(p, d->valuestring, p->codec, sink);
        } else if (!strcmp(e, "input_audio_buffer.speech_started")) {
            if (sink->on_clear) sink->on_clear(sink->user);    /* server VAD: caller talked = barge-in */
        }
        cJSON_Delete(root);
        return;
    }

    if (p->kind == ES_PROTO_DEEPGRAM) {                        /* audio arrives as binary; text = control */
        node = cJSON_GetObjectItem(root, "type");
        e = (node && node->valuestring) ? node->valuestring : "";
        if (!strcmp(e, "UserStartedSpeaking")) {
            if (sink->on_clear) sink->on_clear(sink->user);    /* barge-in */
        }
        cJSON_Delete(root);
        return;
    }

    if (p->kind == ES_PROTO_VAPI) {                           /* audio arrives as binary; text = control JSON */
        node = cJSON_GetObjectItem(root, "type");
        e = (node && node->valuestring) ? node->valuestring : "";
        /* Vapi drives interruption server-side; flush queued agent audio when it signals a barge.
         * (Message set finalised against live traffic; module VAD vad_barge is the vendor-agnostic
         * fallback.) */
        if (!strcmp(e, "user-interrupted")) {
            if (sink->on_clear) sink->on_clear(sink->user);
        } else if (!strcmp(e, "speech-update")) {
            const char *st = es_json_str(root, "status"), *role = es_json_str(root, "role");
            if (!strcmp(st, "started") && !strcmp(role, "user") && sink->on_clear)
                sink->on_clear(sink->user);                    /* caller started talking = barge-in */
        }
        cJSON_Delete(root);
        return;
    }

    if (p->kind == ES_PROTO_ASSEMBLYAI) {                    /* STT: a "transcript" field marks a Turn result */
        cJSON *tr = cJSON_GetObjectItem(root, "transcript"); /* Begin/Termination carry no transcript */
        if (tr && tr->valuestring && sink->on_transcript) {
            cJSON *eot = cJSON_GetObjectItem(root, "end_of_turn");
            sink->on_transcript(sink->user, tr->valuestring, eot && cJSON_IsTrue(eot));
        }
        cJSON_Delete(root);
        return;
    }

    if (p->kind == ES_PROTO_CARTESIA) {                      /* STT: {type:transcript, text, is_final} */
        node = cJSON_GetObjectItem(root, "type");            /* flush_done / done / error are ignored */
        e = (node && node->valuestring) ? node->valuestring : "";
        if (!strcmp(e, "transcript") && sink->on_transcript) {
            cJSON *tr  = cJSON_GetObjectItem(root, "text");
            cJSON *fin = cJSON_GetObjectItem(root, "is_final");
            if (tr && tr->valuestring)
                sink->on_transcript(sink->user, tr->valuestring, fin && cJSON_IsTrue(fin));
        }
        cJSON_Delete(root);
        return;
    }

    if (p->kind == ES_PROTO_ELEVENLABS) {
        node = cJSON_GetObjectItem(root, "type");
        e = (node && node->valuestring) ? node->valuestring : "";
        if (!strcmp(e, "audio")) {
            cJSON *ae = cJSON_GetObjectItem(root, "audio_event");
            cJSON *b  = ae ? cJSON_GetObjectItem(ae, "audio_base_64") : NULL;
            if (b && b->valuestring) es_deliver_ulaw_b64(p, b->valuestring, p->codec, sink);   /* ulaw_8000 */
        } else if (!strcmp(e, "interruption")) {
            if (sink->on_clear) sink->on_clear(sink->user);    /* barge-in */
        } else if (!strcmp(e, "ping")) {                       /* keepalive: must pong back */
            cJSON *pe = cJSON_GetObjectItem(root, "ping_event");
            cJSON *id = pe ? cJSON_GetObjectItem(pe, "event_id") : NULL;
            if (p->ws) {
                char msg[96];
                snprintf(msg, sizeof msg, "{\"type\":\"pong\",\"event_id\":%d}", id ? id->valueint : 0);
                es_send_text_z(p->ws, msg);
            }
        }
        cJSON_Delete(root);
        return;
    }

    if (p->kind == ES_PROTO_GEMINI) {
        cJSON *sc = cJSON_GetObjectItem(root, "serverContent");
        if (sc) {
            cJSON *interrupted = cJSON_GetObjectItem(sc, "interrupted");
            cJSON *mt = cJSON_GetObjectItem(sc, "modelTurn");
            cJSON *parts = mt ? cJSON_GetObjectItem(mt, "parts") : NULL;
            if (interrupted && cJSON_IsTrue(interrupted) && sink->on_clear)
                sink->on_clear(sink->user);                    /* barge-in */
            if (parts) {
                cJSON *part;
                cJSON_ArrayForEach(part, parts) {
                    cJSON *idata = cJSON_GetObjectItem(part, "inlineData");
                    cJSON *data  = idata ? cJSON_GetObjectItem(idata, "data") : NULL;
                    if (data && data->valuestring)
                        es_deliver_ulaw_b64(p, data->valuestring, ES_CODEC_L16, sink);   /* 24k pcm */
                }
            }
        }
        cJSON_Delete(root);
        return;
    }

    /* native: accept {"type":"playAudio","data":{"audio":b64,"codec":..}}, {"type":"clear"},
     * and tolerate twilio-shaped {"event":"media",...} for convenience. */
    node = cJSON_GetObjectItem(root, "type");
    e = (node && node->valuestring) ? node->valuestring : "";
    if (!*e) { node = cJSON_GetObjectItem(root, "event"); e = (node && node->valuestring) ? node->valuestring : ""; }

    /* mod_audio_stream dialect, so an agent written for amigniter/mod_audio_stream works
     * unchanged behind the uuid_audio_stream compat API:
     *   {"type":"streamAudio","data":{"audioDataType":"raw","sampleRate":N,"audioData":b64}}
     *     -> L16 at N Hz, played to the caller (resampled to the channel rate if needed)
     *   {"control":{"command":"clear"|"pickCall"|"transfer",...}}
     *     -> "clear" is a barge-in; every control message is also handed up verbatim
     *        (sink->on_json) for the mod_audio_stream::json event controllers listen to. */
    if (!strcasecmp(e, "streamAudio")) {
        cJSON *d     = cJSON_GetObjectItem(root, "data");
        cJSON *audio = d ? cJSON_GetObjectItem(d, "audioData") : NULL;
        cJSON *kind  = d ? cJSON_GetObjectItem(d, "audioDataType") : NULL;
        cJSON *rate  = d ? cJSON_GetObjectItem(d, "sampleRate") : NULL;
        const char *k = (kind && kind->valuestring) ? kind->valuestring : "raw";
        if (audio && audio->valuestring && !strcasecmp(k, "raw")) {
            /* raw = L16 at data.sampleRate; rebuild the inbound resampler on a change,
             * as the pipecat path does (rs_in is only touched on this receive thread). */
            int hz = rate ? rate->valueint : 0;
            if (hz > 0 && p->rate_out != hz) {
                p->rate_out = hz;
                if (p->rs_in) switch_resample_destroy(&p->rs_in);
                if (p->rate_out != p->chan_rate)
                    switch_resample_create(&p->rs_in, p->rate_out, p->chan_rate, 8192, SWITCH_RESAMPLE_QUALITY, 1);
            }
            es_deliver_ulaw_b64(p, audio->valuestring, ES_CODEC_L16, sink);
        }
        /* wav/mp3/ogg payloads are not decoded here: the compat path asks for raw L16 */
        cJSON_Delete(root);
        return;
    }
    {
        cJSON *ctl = cJSON_GetObjectItem(root, "control");
        if (ctl) {
            cJSON *cmd = cJSON_GetObjectItem(ctl, "command");
            if (cmd && cmd->valuestring && !strcasecmp(cmd->valuestring, "clear") && sink->on_clear)
                sink->on_clear(sink->user);
            if (sink->on_json) sink->on_json(sink->user, data);
            cJSON_Delete(root);
            return;
        }
    }

    if (!strcasecmp(e, "playAudio") || !strcasecmp(e, "media")) {
        cJSON *d = cJSON_GetObjectItem(root, "data");
        cJSON *m = cJSON_GetObjectItem(root, "media");
        cJSON *audio = d ? cJSON_GetObjectItem(d, "audio") : NULL;
        cJSON *pl    = m ? cJSON_GetObjectItem(m, "payload") : NULL;
        cJSON *cod   = d ? cJSON_GetObjectItem(d, "codec") : NULL;
        const char *b64 = audio && audio->valuestring ? audio->valuestring :
                          (pl && pl->valuestring ? pl->valuestring : NULL);
        es_codec_t c = es_codec_from_wire(cod ? cod->valuestring : NULL, p->codec);
        if (b64) es_deliver_ulaw_b64(p, b64, c, sink);
    } else if (!strcasecmp(e, "clear")) {
        if (sink->on_clear) sink->on_clear(sink->user);
    } else if (!strcasecmp(e, "mark")) {
        cJSON *m = cJSON_GetObjectItem(root, "mark");
        cJSON *nm = m ? cJSON_GetObjectItem(m, "name") : NULL;
        if (nm && nm->valuestring && sink->on_mark) sink->on_mark(sink->user, nm->valuestring);
    }
    cJSON_Delete(root);
}

/* Pipecat: parse a protobuf Frame; deliver audio, treat an interruption as barge-in. */
static void es_pipecat_on_binary(es_proto_ctx_t *p, const uint8_t *data, size_t len, const es_proto_sink_t *sink)
{
    es_pb_frame_t f;
    if (es_pb_decode_frame(data, len, &f) != 0) return;        /* malformed -> drop safely */
    if (f.interrupted && sink->on_clear) sink->on_clear(sink->user);
    if (f.has_audio && f.audio_len) {
        /* Honor the frame's declared rate: if the agent streams at a rate other than the
         * one we assumed, rebuild the inbound resampler for it (only on change; rs_in is
         * touched solely on this receive thread, so this is safe). */
        if (f.sample_rate && (uint32_t) p->rate_out != f.sample_rate) {
            p->rate_out = (int) f.sample_rate;
            if (p->rs_in) switch_resample_destroy(&p->rs_in);
            if (p->rate_out != p->chan_rate)
                switch_resample_create(&p->rs_in, p->rate_out, p->chan_rate, 8192, SWITCH_RESAMPLE_QUALITY, 1);
        }
        es_decode_chunked(p, ES_CODEC_L16, f.audio, f.audio_len, sink);
    }
}

void es_proto_on_binary(es_proto_ctx_t *p, const void *data, size_t len, const es_proto_sink_t *sink)
{
    if (!p || !data || !len || !sink) return;
    if (p->kind == ES_PROTO_GEMINI) {   /* Gemini Live delivers its JSON (control + audio) as BINARY
                                         * frames, not text — parse them through the text path */
        es_proto_on_text(p, (const char *) data, len, sink);
        return;
    }
    if (!sink->on_audio) return;
    if (p->kind == ES_PROTO_DOUBAO || p->kind == ES_PROTO_TWILIO || p->kind == ES_PROTO_OPENAI ||
        p->kind == ES_PROTO_ELEVENLABS) return;  /* audio is text-framed */
    if (p->kind == ES_PROTO_PIPECAT) { es_pipecat_on_binary(p, (const uint8_t *) data, len, sink); return; }
    /* native + deepgram + vapi: raw codec-coded audio, any length */
    es_decode_chunked(p, p->codec, (const uint8_t *) data, len, sink);
}
