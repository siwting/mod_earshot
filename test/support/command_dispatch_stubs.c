/* Test-only FreeSWITCH shims: deterministic rate-count resampler, base64, and
 * captured transport output. Tests link real vendored cJSON separately.
 * EARSHOT_REAL_WS uses the real libwebsockets transport instead of capture stubs. */
#include <switch.h>
#include "es_ws.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>

size_t switch_copy_string(char *d, const char *s, size_t n)
{ size_t i = 0; if (!n) return 0; for (; s && s[i] && i < n - 1; i++) d[i] = s[i]; d[i] = 0; return i; }
void switch_uuid_get(switch_uuid_t *u) { if (u) memset(u, 0, sizeof *u); }
void switch_uuid_format(char *b, switch_uuid_t *u) { (void) u; if (b) strcpy(b, "00000000-0000-0000-0000-000000000000"); }
int switch_resample_create(switch_audio_resampler_t **r, int a, int b, int c, int d, int e)
{
    (void)c; (void)d; (void)e;
    *r = calloc(1, sizeof **r); if (!*r) return -1;
    (*r)->from_rate = a; (*r)->to_rate = b; return 0;
}
void switch_resample_destroy(switch_audio_resampler_t **r) { if (r && *r) { free((*r)->to); free(*r); *r = 0; } }
/* Nearest-neighbor TEST resampler: validates direction/count, not Speex quality. */
int switch_resample_process(switch_audio_resampler_t *r, int16_t *x, int n) {
    unsigned count = (unsigned)((uint64_t)n * r->to_rate / r->from_rate);
    r->to = realloc(r->to, count * 2); r->to_len = count;
    for (unsigned i = 0; i < count; i++) r->to[i] = x[(uint64_t)i * r->from_rate / r->to_rate];
    return count;
}
switch_size_t switch_b64_encode(unsigned char *s, switch_size_t sl, unsigned char *d, switch_size_t dl)
{ return switch_b64_encode_real(s, sl, (char *)d, dl); }
/* real base64 decode (RFC 4648, '=' padding, no line breaks), FreeSWITCH-style: returns bytes
 * written and NUL-terminates when room allows; stops at the output capacity */
switch_size_t switch_b64_decode(char *s, char *d, switch_size_t dl)
{
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned v = 0; int bits = 0; switch_size_t n = 0;
    if (!s || !d || !dl) return 0;
    for (; *s && *s != '='; s++) {
        const char *q = strchr(tbl, *s);
        if (!q) continue;
        v = (v << 6) | (unsigned) (q - tbl); bits += 6;
        if (bits >= 8) { bits -= 8; if (n + 1 >= dl) break; d[n++] = (char) ((v >> bits) & 0xFF); }
    }
    if (n < dl) d[n] = 0;
    return n;
}
switch_size_t switch_b64_encode_real(const unsigned char *in, switch_size_t il, char *out, switch_size_t ol)
{
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    switch_size_t i, o = 0;
    for (i = 0; i + 2 < il; i += 3) {
        if (o + 4 >= ol) return 0;
        out[o++] = tbl[in[i] >> 2]; out[o++] = tbl[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        out[o++] = tbl[((in[i + 1] & 15) << 2) | (in[i + 2] >> 6)]; out[o++] = tbl[in[i + 2] & 63];
    }
    if (i < il) {
        if (o + 4 >= ol) return 0;
        out[o++] = tbl[in[i] >> 2];
        if (i + 1 < il) { out[o++] = tbl[((in[i] & 3) << 4) | (in[i + 1] >> 4)]; out[o++] = tbl[(in[i + 1] & 15) << 2]; }
        else            { out[o++] = tbl[(in[i] & 3) << 4]; out[o++] = '='; }
        out[o++] = '=';
    }
    out[o] = 0; return o;
}

#ifndef EARSHOT_REAL_WS
char test_ws_text[131072];
int test_ws_sends, test_ws_discards, test_ws_close, test_ws_binary;
int es_ws_send_text(es_ws_t *w, const char *d, size_t l) {
    (void)w; if (l >= sizeof test_ws_text) abort();
    memcpy(test_ws_text, d, l); test_ws_text[l] = 0; test_ws_sends++; return 0;
}
void es_ws_discard_pending(es_ws_t *w) { (void)w; test_ws_discards++; }
void es_ws_request_close(es_ws_t *w, int retry) { (void)w; test_ws_close = retry ? 2 : 1; }
int es_ws_send_binary(es_ws_t *w, const void *d, size_t l) { (void) w;(void) d;(void) l; test_ws_binary++; return 0; }


#endif /* EARSHOT_REAL_WS */
