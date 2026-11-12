#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chip.h"

#define START  0x61
#define T_CTRL 0x00
#define T_CHAN 0x01
#define T_SPCH 0x02

enum {
    F_SPEECHD = 0x00, F_CHAND = 0x01, F_CMODE = 0x02, F_SAMPLES = 0x03, F_ECMODE = 0x05, F_DCMODE = 0x06,
    F_TONE = 0x08, F_RATET = 0x09, F_RATEP = 0x0a, F_INIT = 0x0b, F_LOWPOWER = 0x10, F_CHANFMT = 0x15,
    F_SPCHFMT = 0x16, F_CHAND4 = 0x17, F_CODECSTART = 0x2a, F_CODECSTOP = 0x2b, F_PARITY = 0x2f,
    F_PRODID = 0x30, F_VERSTRING = 0x31, F_COMPAND = 0x32, F_RESET = 0x33, F_RESETSOFTCFG = 0x34,
    F_HALT = 0x35, F_GETCFG = 0x36, F_READCFG = 0x37, F_CODECCFG = 0x38, F_READY = 0x39,
    F_PARITYMODE = 0x3f, F_CHANNEL0 = 0x40, F_CLRCODECRESET = 0x46, F_SETCODECRESET = 0x47,
    F_DISCARDCODEC = 0x48, F_DELAYNUS = 0x49, F_DELAYNNS = 0x4a, F_GAIN = 0x4b, F_RTSTHRESH = 0x4e,
};

#define NCH         3
#define MAX_SAMPLES 164
#define VERLEN      48
#define RXCAP       (4 * (4 + 0x160))
#define VAR         1000
#define NJOB        6

#define DEF_RATET   33
#define DEF_ECMODE  (DV3K_EC_NS_ENABLE | DV3K_EC_TD_ENABLE)
#define DEF_VERSION "V120.E100.XXXX.C106.G514.R009.B0010411.C0020208"

#define D_RATE 1
#define D_CFG  2

static const uint8_t ratet_bits[64] = {
    48, 72, 80, 96, 128, 160, 192, 80, 96, 96, 96, 128, 144, 160, 192, 192,
    48, 72, 80, 96, 128, 160, 192, 80, 96, 96, 96, 128, 144, 160, 192, 192, 40,
    72, 49, 68, 45, 72, 48, 60, 72, 80, 88, 96, 128, 144, 160, 192,
    80, 96, 96, 128, 144, 160, 192, 192, 72, 80, 96, 144, 160, 192, 0, 0,
};

struct chan {
    struct dv3k_chan_cfg cfg;
    void *inst;
    int bits;
    int dirty;
    int bitwarn;
};

struct dv3k_chip {
    int model, nch;
    int parity;
    uint8_t softcfg[3];
    char prodid[64];
    char version[VERLEN];
    const struct dv3k_backend_ops *be;
    struct chan ch[NCH];
    uint8_t rx[RXCAP];
    size_t rxn;
    dv3k_emit_fn emit;
    dv3k_warn_fn warn;
    dv3k_submit_fn submit;
    void *ctx;
    struct dv3k_job *jobs;
    int q0, nq;
    int blocked;
};

static void warnf(struct dv3k_chip *c, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    c->warn(c->ctx, buf);
}

static void cat(char *d, size_t n, const char *fmt, ...)
{
    size_t l = strlen(d);
    va_list ap;
    if (l + 1 >= n)
        return;
    va_start(ap, fmt);
    vsnprintf(d + l, n - l, fmt, ap);
    va_end(ap);
}

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

static uint8_t lin2ulaw(int16_t s)
{
    int sign = s < 0 ? 0x80 : 0, v = s < 0 ? -(int)s : s, e = 7, m;
    if (v > 32635)
        v = 32635;
    v += 0x84;
    for (m = 0x4000; !(v & m) && e > 0; m >>= 1)
        e--;
    return (uint8_t)~(sign | e << 4 | ((v >> (e + 3)) & 0x0f));
}

static int16_t ulaw2lin(uint8_t u)
{
    int t;
    u = (uint8_t)~u;
    t = (((u & 0x0f) << 3) + 0x84) << ((u & 0x70) >> 4);
    return (int16_t)((u & 0x80) ? 0x84 - t : t - 0x84);
}

static uint8_t lin2alaw(int16_t s)
{
    int v = s >> 3, mask = 0xd5, seg;
    if (v < 0) {
        mask = 0x55;
        v = -v - 1;
    }
    for (seg = 0; seg < 8 && v > (0x1f << seg); seg++)
        ;
    if (seg >= 8)
        return (uint8_t)(0x7f ^ mask);
    return (uint8_t)(((seg << 4) | ((v >> (seg < 2 ? 1 : seg)) & 0x0f)) ^ mask);
}

static int16_t alaw2lin(uint8_t a)
{
    int t, seg;
    a ^= 0x55;
    t = (a & 0x0f) << 4;
    seg = (a & 0x70) >> 4;
    if (seg == 0)
        t += 8;
    else
        t = (t + 0x108) << (seg - 1);
    return (int16_t)((a & 0x80) ? t : -t);
}

static int16_t expand(uint8_t compand, uint8_t b) { return compand & DV3K_COMPAND_ALAW ? alaw2lin(b) : ulaw2lin(b); }
static uint8_t compress(uint8_t compand, int16_t s) { return compand & DV3K_COMPAND_ALAW ? lin2alaw(s) : lin2ulaw(s); }

static void apply_gain(int16_t *pcm, int n, int db)
{
    double g = 1.0, step = db > 0 ? 1.1220184543019633 : 1 / 1.1220184543019633;
    int i;
    if (!db)
        return;
    for (i = 0; i < abs(db); i++)
        g *= step;
    for (i = 0; i < n; i++) {
        double v = pcm[i] * g;
        pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v + (v < 0 ? -0.5 : 0.5));
    }
}

static int ctrl_len(int id)
{
    switch (id) {
    case F_PRODID: case F_VERSTRING: case F_RESET: case F_HALT: case F_GETCFG: case F_READCFG: case F_READY:
    case F_CODECSTOP: case F_CLRCODECRESET: case F_SETCODECRESET:
        return 0;
    case F_RATET: case F_INIT: case F_LOWPOWER: case F_CODECSTART: case F_COMPAND: case F_PARITYMODE:
        return 1;
    case F_ECMODE: case F_DCMODE: case F_CHANFMT: case F_SPCHFMT: case F_DISCARDCODEC: case F_DELAYNUS:
    case F_DELAYNNS: case F_GAIN:
        return 2;
    case F_RTSTHRESH:
        return 5;
    case F_RESETSOFTCFG:
        return 6;
    case F_RATEP:
        return 12;
    case F_CODECCFG:
        return VAR;
    }
    return -1;
}

static const char *fname(int type, int id)
{
    if (id == F_PARITY)
        return "PARITY";
    if (id >= F_CHANNEL0 && id < F_CHANNEL0 + NCH)
        return id == 0x40 ? "CHANNEL0" : id == 0x41 ? "CHANNEL1" : "CHANNEL2";
    if (type != T_CTRL)
        switch (id) {
        case F_SPEECHD: return type == T_SPCH ? "SPEECHD" : "?";
        case F_CHAND: return "CHAND";
        case F_CHAND4: return "CHAND4";
        case F_CMODE: return "CMODE";
        case F_SAMPLES: return "SAMPLES";
        case F_TONE: return "TONE";
        default: return "?";
        }
    switch (id) {
    case F_ECMODE: return "ECMODE";
    case F_DCMODE: return "DCMODE";
    case F_RATET: return "RATET";
    case F_RATEP: return "RATEP";
    case F_INIT: return "INIT";
    case F_LOWPOWER: return "LOWPOWER";
    case F_CHANFMT: return "CHANFMT";
    case F_SPCHFMT: return "SPCHFMT";
    case F_CODECSTART: return "CODECSTART";
    case F_CODECSTOP: return "CODECSTOP";
    case F_PRODID: return "PRODID";
    case F_VERSTRING: return "VERSTRING";
    case F_COMPAND: return "COMPAND";
    case F_RESET: return "RESET";
    case F_RESETSOFTCFG: return "RESETSOFTCFG";
    case F_HALT: return "HALT";
    case F_GETCFG: return "GETCFG";
    case F_READCFG: return "READCFG";
    case F_CODECCFG: return "CODECCFG";
    case F_READY: return "READY";
    case F_PARITYMODE: return "PARITYMODE";
    case F_CLRCODECRESET: return "CLRCODECRESET";
    case F_SETCODECRESET: return "SETCODECRESET";
    case F_DISCARDCODEC: return "DISCARDCODEC";
    case F_DELAYNUS: return "DELAYNUS";
    case F_DELAYNNS: return "DELAYNNS";
    case F_GAIN: return "GAIN";
    case F_RTSTHRESH: return "RTSTHRESH";
    }
    return "?";
}

static int compand8(const struct dv3k_chip *c, int ch)
{
    return ch < c->nch && (c->ch[ch].cfg.compand & DV3K_COMPAND_ENABLE);
}

static int flen(const struct dv3k_chip *c, int type, int ch, const uint8_t *f, size_t avail)
{
    int id = f[0], n;
    if (id == F_PARITY)
        return 2;
    if (id >= F_CHANNEL0 && id < F_CHANNEL0 + NCH)
        return 1;
    if (type == T_CTRL) {
        n = ctrl_len(id);
        if (n == VAR)
            return avail < 2 ? 0 : 2 + f[1];
        return n < 0 ? -1 : 1 + n;
    }
    if (id == F_CMODE || id == F_TONE)
        return 3;
    if (type == T_SPCH) {
        if (id != F_SPEECHD)
            return -1;
        if (avail < 2)
            return 0;
        if (f[1] == 0 || f[1] > MAX_SAMPLES)
            return -1;
        return 2 + f[1] * (compand8(c, ch) ? 1 : 2);
    }
    if (id == F_SAMPLES)
        return 2;
    if (id == F_CHAND || id == F_CHAND4) {
        if (avail < 2)
            return 0;
        if (f[1] == 0)
            return -1;
        return 2 + (id == F_CHAND ? (f[1] + 7) / 8 : (f[1] + 1) / 2);
    }
    return -1;
}

static uint8_t xor_parity(const uint8_t *p, size_t at)
{
    uint8_t x = 0;
    size_t i;
    for (i = 1; i <= at; i++)
        x ^= p[i];
    return x;
}

enum { W_MORE, W_BAD, W_OK };

static int walk(struct dv3k_chip *c, const uint8_t *p, size_t have, size_t *unknown, const char **why)
{
    static const unsigned lo[3] = { 1, 3, 4 }, hi[3] = { 0x80, 0x90, 0x160 };
    unsigned len, type;
    size_t o, end;
    int ch = 0, l;

    *unknown = 0;
    *why = "";
    if (have < 4)
        return W_MORE;
    len = be16(p + 1);
    type = p[3];
    if (type > T_SPCH || len < lo[type] || len > hi[type]) {
        *why = "header";
        return W_BAD;
    }
    end = 4 + len;
    for (o = 4; o < end; o += l) {
        if (o >= have)
            return W_MORE;
        l = flen(c, type, ch, p + o, have - o);
        if (l == 0)
            return W_MORE;
        if (l < 0) {
            if (type != T_CTRL) {
                *why = "field";
                return W_BAD;
            }
            if (have < end)
                return W_MORE;
            if (o == 4 && !(p[end - 2] == F_PARITY && p[end - 1] == xor_parity(p, end - 2))) {
                *why = "unknown first field";
                return W_BAD;
            }
            *unknown = o;
            return W_OK;
        }
        if (o + l > end) {
            *why = "field overruns length";
            return W_BAD;
        }
        if (p[o] == F_PARITY) {
            if (have < o + 2)
                return W_MORE;
            if (p[o + 1] != xor_parity(p, o)) {
                *why = "parity";
                return W_BAD;
            }
        } else if (p[o] >= F_CHANNEL0 && p[o] < F_CHANNEL0 + NCH) {
            ch = p[o] - F_CHANNEL0 < c->nch ? p[o] - F_CHANNEL0 : 0;
        }
    }
    return have < end ? W_MORE : W_OK;
}

static void send_pkt(struct dv3k_chip *c, int type, uint8_t *r, size_t rn, int parity, const char *desc)
{
    if (parity) {
        r[rn] = F_PARITY;
        r[1] = (uint8_t)((rn + 2 - 4) >> 8);
        r[2] = (uint8_t)(rn + 2 - 4);
        r[3] = (uint8_t)type;
        r[rn + 1] = xor_parity(r, rn);
        rn += 2;
    }
    r[0] = START;
    r[1] = (uint8_t)((rn - 4) >> 8);
    r[2] = (uint8_t)(rn - 4);
    r[3] = (uint8_t)type;
    c->emit(c->ctx, "tx", r, rn, desc);
}

static void chan_build(struct dv3k_chip *c, int i)
{
    struct chan *k = &c->ch[i];
    int bits = k->cfg.bits;
    k->bitwarn = 0;
    if (k->inst)
        c->be->destroy(k->inst);
    k->inst = c->be->create(&k->cfg, i, &bits);
    if (!k->inst) {
        warnf(c, "backend %s refused channel %d rate (ratet %d, %d bits), sending zero bits / silence",
              c->be->name, i, k->cfg.ratet, k->cfg.bits);
        bits = k->cfg.bits;
    }
    if (bits < 1 || bits > DV3K_MAX_BITS) {
        warnf(c, "backend %s gave %d bits for channel %d, using %d", c->be->name, bits, i, k->cfg.bits);
        bits = k->cfg.bits;
    }
    k->bits = bits;
}

static void chan_flush(struct dv3k_chip *c, int i)
{
    struct chan *k = &c->ch[i];
    if ((k->dirty & D_RATE) || ((k->dirty & D_CFG) && !(k->inst && c->be->config)))
        chan_build(c, i);
    else if (k->dirty & D_CFG)
        c->be->config(k->inst, &k->cfg);
    k->dirty = 0;
}

static void chip_reset(struct dv3k_chip *c)
{
    int i;
    c->parity = 1;
    for (i = 0; i < c->nch; i++) {
        struct chan *k = &c->ch[i];
        memset(&k->cfg, 0, sizeof k->cfg);
        k->cfg.ratet = DEF_RATET;
        k->cfg.bits = ratet_bits[DEF_RATET];
        k->cfg.ecmode = DEF_ECMODE;
        k->dirty = D_RATE;
        chan_flush(c, i);
    }
}

static void send_ready(struct dv3k_chip *c)
{
    uint8_t r[8];
    r[4] = F_READY;
    send_pkt(c, T_CTRL, r, 5, c->model == 3003, "READY");
}

static void do_ctrl(struct dv3k_chip *c, const uint8_t *p, size_t end, size_t unknown)
{
    uint8_t r[512];
    char desc[512] = "CONTROL";
    size_t rn = 4, o;
    int ch = 0, l, i, touched = 0;

    for (o = 4; o < end; o += l) {
        int id = p[o], st = 0;
        const uint8_t *d = p + o + 1;
        struct chan *k = &c->ch[ch];
        if (unknown && o == unknown) {
            warnf(c, "unknown control field 0x%02x at offset %zu, replying status 1 and ignoring the rest", id, o);
            r[rn++] = (uint8_t)id;
            r[rn++] = 1;
            cat(desc, sizeof desc, " UNKNOWN(0x%02x)=1", id);
            break;
        }
        l = flen(c, T_CTRL, ch, p + o, end - o);
        if (rn + VERLEN + 8 > sizeof r)
            break;
        switch (id) {
        case F_PARITY:
            continue;
        case F_CHANNEL0: case F_CHANNEL0 + 1: case F_CHANNEL0 + 2:
            if (id - F_CHANNEL0 < c->nch) {
                ch = id - F_CHANNEL0;
            } else {
                warnf(c, "%s on a %d-channel model", fname(T_CTRL, id), c->nch);
                st = 1;
            }
            break;
        case F_ECMODE:
            k->cfg.ecmode = (uint16_t)be16(d);
            k->dirty |= D_CFG;
            break;
        case F_DCMODE:
            k->cfg.dcmode = (uint16_t)be16(d);
            k->dirty |= D_CFG;
            break;
        case F_RATET:
            if (d[0] < 64 && ratet_bits[d[0]]) {
                k->cfg.ratet = d[0];
                memset(k->cfg.ratep, 0, sizeof k->cfg.ratep);
                k->cfg.bits = ratet_bits[d[0]];
                k->dirty |= D_RATE;
            } else {
                warnf(c, "RATET %d not in rate table", d[0]);
                st = 1;
            }
            break;
        case F_RATEP:
            if (d[11] == 0) {
                warnf(c, "RATEP with 0 channel bits");
                st = 1;
                break;
            }
            k->cfg.ratet = -1;
            for (i = 0; i < 6; i++)
                k->cfg.ratep[i] = (uint16_t)be16(d + 2 * i);
            k->cfg.bits = d[11];
            k->dirty |= D_RATE;
            break;
        case F_INIT:
            chan_flush(c, ch);
            if (k->inst && c->be->init)
                c->be->init(k->inst, d[0] & 3);
            else
                chan_build(c, ch);
            break;
        case F_CHANFMT:
            k->cfg.chanfmt = (uint16_t)be16(d);
            k->dirty |= D_CFG;
            if (k->cfg.chanfmt)
                warnf(c, "CHANFMT 0x%04x accepted, extra output fields not emulated", k->cfg.chanfmt);
            break;
        case F_SPCHFMT:
            k->cfg.spchfmt = (uint16_t)be16(d);
            k->dirty |= D_CFG;
            if (k->cfg.spchfmt)
                warnf(c, "SPCHFMT 0x%04x accepted, extra output fields not emulated", k->cfg.spchfmt);
            break;
        case F_COMPAND:
            k->cfg.compand = d[0];
            k->dirty |= D_CFG;
            break;
        case F_GAIN:
            k->cfg.gain_in = (int8_t)(d[0] > 127 ? (int)d[0] - 256 : d[0]);
            k->cfg.gain_out = (int8_t)(d[1] > 127 ? (int)d[1] - 256 : d[1]);
            if (k->cfg.gain_in < -90) k->cfg.gain_in = -90;
            if (k->cfg.gain_in > 90) k->cfg.gain_in = 90;
            if (k->cfg.gain_out < -90) k->cfg.gain_out = -90;
            if (k->cfg.gain_out > 90) k->cfg.gain_out = 90;
            k->dirty |= D_CFG;
            break;
        case F_PARITYMODE:
            c->parity = d[0] & 1;
            break;
        case F_PRODID:
            r[rn++] = (uint8_t)id;
            memcpy(r + rn, c->prodid, strlen(c->prodid) + 1);
            rn += strlen(c->prodid) + 1;
            cat(desc, sizeof desc, " PRODID");
            continue;
        case F_VERSTRING:
            r[rn++] = (uint8_t)id;
            memcpy(r + rn, c->version, VERLEN);
            rn += VERLEN;
            cat(desc, sizeof desc, " VERSTRING");
            continue;
        case F_GETCFG: case F_READCFG:
            r[rn++] = (uint8_t)id;
            memcpy(r + rn, c->softcfg, 3);
            rn += 3;
            cat(desc, sizeof desc, " %s", fname(T_CTRL, id));
            continue;
        case F_RESETSOFTCFG:
            for (i = 0; i < 3; i++)
                c->softcfg[i] = (uint8_t)((c->softcfg[i] & ~d[3 + i]) | (d[i] & d[3 + i]));
            warnf(c, "RESETSOFTCFG cfg %02x %02x %02x mask %02x %02x %02x stored, not interpreted",
                  d[0], d[1], d[2], d[3], d[4], d[5]);
            /* fall through */
        case F_RESET:
            chip_reset(c);
            send_ready(c);
            return;
        case F_HALT:
            warnf(c, "HALT ignored, chip keeps running");
            cat(desc, sizeof desc, " HALT");
            continue;
        case F_READY:
            continue;
        case F_LOWPOWER: case F_CODECSTART: case F_CODECSTOP: case F_CODECCFG: case F_CLRCODECRESET:
        case F_SETCODECRESET: case F_DISCARDCODEC: case F_DELAYNUS: case F_DELAYNNS: case F_RTSTHRESH:
            warnf(c, "%s accepted, not emulated", fname(T_CTRL, id));
            break;
        }
        r[rn++] = (uint8_t)id;
        r[rn++] = (uint8_t)st;
        cat(desc, sizeof desc, " %s=%d", fname(T_CTRL, id), st);
        touched = 1;
    }
    if (touched)
        for (i = 0; i < c->nch; i++)
            if (c->ch[i].dirty)
                chan_flush(c, i);
    if (rn > 4) {
        if (c->parity)
            cat(desc, sizeof desc, " +parity");
        send_pkt(c, T_CTRL, r, rn, c->parity, desc);
    }
}

struct slot {
    const uint8_t *f;
    int has_cmode;
    uint16_t cmode;
    int tone, samples;
};

/* prepared and finished on the chip's thread, run on any */
struct dv3k_job {
    struct dv3k_chip *c;
    int ch, enc;
    int nbits, ns;
    int rc;
    int submitted;  /* handed to a worker, after its channel's earlier jobs */
    int done;       /* __atomic, set by dv3k_job_run */
    struct dv3k_frame fr;
    int16_t pcm[MAX_SAMPLES];
    uint8_t bits[DV3K_MAX_BITS / 8 + 1], soft[DV3K_MAX_BITS];
};

static void job_prepare(struct dv3k_chip *c, struct dv3k_job *j, int i, int enc, const struct slot *s)
{
    struct chan *k = &c->ch[i];
    int n = s->f[1], nbits = s->f[1], b;
    const uint8_t *d = s->f + 2;

    memset(j, 0, sizeof *j);
    j->c = c;
    j->ch = i;
    j->enc = enc;
    j->fr.cmode = s->has_cmode ? s->cmode : enc ? k->cfg.ecmode : k->cfg.dcmode;
    j->fr.tone = s->tone;
    if (enc) {
        for (b = 0; b < n && b < DV3K_FRAME_SAMPLES; b++)
            j->pcm[b] = k->cfg.compand & DV3K_COMPAND_ENABLE ? expand(k->cfg.compand, d[b])
                                                             : (int16_t)(d[2 * b] << 8 | d[2 * b + 1]);
        if (!(c->be->flags & DV3K_BF_GAIN))
            apply_gain(j->pcm, DV3K_FRAME_SAMPLES, k->cfg.gain_in);
        j->nbits = k->bits;
        return;
    }
    if (s->f[0] == F_CHAND4) {
        for (b = 0; b < nbits; b++) {
            uint8_t v = d[b / 2];
            j->soft[b] = b & 1 ? v & 0x0f : v >> 4;
            if (j->soft[b] >= 8)
                j->bits[b / 8] |= (uint8_t)(0x80 >> (b % 8));
        }
        j->fr.soft = j->soft;
    } else {
        memcpy(j->bits, d, (size_t)(nbits + 7) / 8);
    }
    if (nbits != k->bits && !k->bitwarn) {
        warnf(c, "channel %d got %d bits, rate is %d bits (reported once)", i, nbits, k->bits);
        k->bitwarn = 1;
    }
    if (nbits > k->bits)
        nbits = k->bits;
    j->nbits = nbits;
    j->ns = s->samples > 0 ? s->samples : DV3K_FRAME_SAMPLES;
    if (j->ns > MAX_SAMPLES)
        j->ns = MAX_SAMPLES;
}

void dv3k_job_run(struct dv3k_job *j)
{
    struct dv3k_chip *c = j->c;
    void *inst = c->ch[j->ch].inst;
    j->rc = 0;
    if (j->enc && inst)
        j->rc = c->be->encode(inst, &j->fr, j->pcm, j->bits, j->nbits);
    else if (inst)
        j->rc = c->be->decode(inst, &j->fr, j->bits, j->nbits, j->pcm);
    __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
}

static void job_finish(struct dv3k_chip *c, struct dv3k_job *j)
{
    struct chan *k = &c->ch[j->ch];
    uint8_t r[16 + 2 * MAX_SAMPLES];
    int nb = (j->nbits + 7) / 8, b;
    size_t rn = 4;
    char desc[64];

    if (c->model == 3003)
        r[rn++] = (uint8_t)(F_CHANNEL0 + j->ch);
    if (j->enc) {
        if (j->rc < 0) {
            warnf(c, "backend encode failed on channel %d", j->ch);
            memset(j->bits, 0, (size_t)nb);
        }
        if (j->nbits % 8)
            j->bits[nb - 1] &= (uint8_t)(0xff << (8 - j->nbits % 8));
        r[rn++] = F_CHAND;
        r[rn++] = (uint8_t)j->nbits;
        memcpy(r + rn, j->bits, (size_t)nb);
        rn += (size_t)nb;
        snprintf(desc, sizeof desc, "CHANNEL ch%d CHAND %d%s", j->ch, j->nbits, c->parity ? " +parity" : "");
        send_pkt(c, T_CHAN, r, rn, c->parity, desc);
        return;
    }
    if (j->rc < 0) {
        warnf(c, "backend decode failed on channel %d", j->ch);
        memset(j->pcm, 0, sizeof j->pcm);
    }
    if (!(c->be->flags & DV3K_BF_GAIN))
        apply_gain(j->pcm, DV3K_FRAME_SAMPLES, k->cfg.gain_out);
    r[rn++] = F_SPEECHD;
    r[rn++] = (uint8_t)j->ns;
    for (b = 0; b < j->ns; b++) {
        if (k->cfg.compand & DV3K_COMPAND_ENABLE) {
            r[rn++] = compress(k->cfg.compand, j->pcm[b]);
        } else {
            r[rn++] = (uint8_t)((uint16_t)j->pcm[b] >> 8);
            r[rn++] = (uint8_t)j->pcm[b];
        }
    }
    snprintf(desc, sizeof desc, "SPEECH ch%d SPEECHD %d%s", j->ch, j->ns, c->parity ? " +parity" : "");
    send_pkt(c, T_SPCH, r, rn, c->parity, desc);
}

static int slots(struct dv3k_chip *c, const uint8_t *p, size_t end, int type, struct slot *s, int *order, int warn)
{
    int no = 0, ch = 0, l, i;
    size_t o;

    for (i = 0; i < NCH; i++) {
        memset(&s[i], 0, sizeof s[i]);
        s[i].tone = s[i].samples = -1;
    }
    for (o = 4; o < end; o += l) {
        int id = p[o];
        l = flen(c, type, ch, p + o, end - o);
        if (l <= 0)
            break;
        if (id >= F_CHANNEL0 && id < F_CHANNEL0 + NCH) {
            ch = id - F_CHANNEL0;
            if (ch >= c->nch) {
                if (warn)
                    warnf(c, "%s on a %d-channel model, using channel 0", fname(type, id), c->nch);
                ch = 0;
            }
        } else if (id == F_SPEECHD || id == F_CHAND || id == F_CHAND4) {
            if (s[ch].f && warn)
                warnf(c, "second %s for channel %d in one packet ignored", fname(type, id), ch);
            else if (!s[ch].f)
                order[no++] = ch;
            if (!s[ch].f)
                s[ch].f = p + o;
        } else if (id == F_CMODE) {
            s[ch].has_cmode = 1;
            s[ch].cmode = (uint16_t)be16(p + o + 1);
        } else if (id == F_TONE) {
            s[ch].tone = (int)be16(p + o + 1);
        } else if (id == F_SAMPLES) {
            s[ch].samples = p[o + 1];
        }
    }
    return no;
}

/* async: a control packet waits for every job, a data packet for room in
   the job ring, which all channels share */
static int can_process(struct dv3k_chip *c, const uint8_t *p, size_t end)
{
    struct slot s[NCH];
    int order[NCH];

    if (!c->submit)
        return 1;
    if (p[3] == T_CTRL)
        return c->nq == 0;
    return c->nq + slots(c, p, end, p[3], s, order, 0) <= NJOB;
}

/* async: submits queued jobs whose channel has nothing in flight, so an
   instance never runs twice at once */
static void submit_ready(struct dv3k_chip *c)
{
    int q, e;
    for (q = 0; q < c->nq; q++) {
        struct dv3k_job *j = &c->jobs[(c->q0 + q) % NJOB];
        if (j->submitted)
            continue;
        for (e = 0; e < q; e++) {
            struct dv3k_job *k = &c->jobs[(c->q0 + e) % NJOB];
            if (k->ch == j->ch && !__atomic_load_n(&k->done, __ATOMIC_ACQUIRE))
                break;
        }
        if (e == q) {
            j->submitted = 1;
            c->submit(c->ctx, j);
        }
    }
}

static void do_data(struct dv3k_chip *c, const uint8_t *p, size_t end, int type)
{
    struct slot s[NCH];
    int order[NCH], no, i;
    struct dv3k_job *j;

    no = slots(c, p, end, type, s, order, 1);
    if (!no)
        warnf(c, "%s packet without data field", type == T_SPCH ? "speech" : "channel");
    for (i = 0; i < no; i++) {
        if (c->submit) {
            j = &c->jobs[(c->q0 + c->nq++) % NJOB];
            job_prepare(c, j, order[i], type == T_SPCH, &s[order[i]]);
            submit_ready(c);
        } else {
            j = &c->jobs[0];
            job_prepare(c, j, order[i], type == T_SPCH, &s[order[i]]);
            dv3k_job_run(j);
            job_finish(c, j);
        }
    }
}

static void describe(const struct dv3k_chip *c, const uint8_t *p, size_t end, size_t unknown, char *d, size_t n)
{
    static const char *tn[3] = { "CONTROL", "CHANNEL", "SPEECH" };
    int type = p[3], ch = 0, l;
    size_t o;
    snprintf(d, n, "%s", tn[type]);
    for (o = 4; o < end; o += l) {
        int id = p[o];
        if (unknown && o == unknown) {
            cat(d, n, " UNKNOWN(0x%02x)", id);
            break;
        }
        l = flen(c, type, ch, p + o, end - o);
        if (id >= F_CHANNEL0 && id < F_CHANNEL0 + NCH)
            ch = id - F_CHANNEL0;
        if (id == F_SPEECHD && type == T_SPCH)
            cat(d, n, " SPEECHD %d", p[o + 1]);
        else if ((id == F_CHAND || id == F_CHAND4) && type == T_CHAN)
            cat(d, n, " %s %d", fname(type, id), p[o + 1]);
        else if (id == F_RATET && type == T_CTRL)
            cat(d, n, " RATET %d", p[o + 1]);
        else if (id == F_PARITY)
            cat(d, n, " +parity");
        else
            cat(d, n, " %s", fname(type, id));
    }
}

static void process(struct dv3k_chip *c, const uint8_t *p, size_t end, size_t unknown)
{
    char desc[512];
    describe(c, p, end, unknown, desc, sizeof desc);
    c->emit(c->ctx, "rx", p, end, desc);
    if (p[3] == T_CTRL)
        do_ctrl(c, p, end, unknown);
    else
        do_data(c, p, end, p[3]);
}

static void junk(struct dv3k_chip *c, size_t from, size_t to)
{
    char desc[32];
    if (to <= from)
        return;
    snprintf(desc, sizeof desc, "junk %zu bytes", to - from);
    c->emit(c->ctx, "rx", c->rx + from, to - from, desc);
}

static void parse(struct dv3k_chip *c)
{
    size_t i = 0, js = 0, unknown;
    const char *why;

    c->blocked = 0;
    for (;;) {
        int r;
        while (i < c->rxn && c->rx[i] != START)
            i++;
        if (i >= c->rxn)
            break;
        r = walk(c, c->rx + i, c->rxn - i, &unknown, &why);
        if (r == W_MORE)
            break;
        if (r == W_BAD) {
            if (!strcmp(why, "parity"))
                warnf(c, "parity error, packet dropped");
            i++;
            continue;
        }
        if (!can_process(c, c->rx + i, 4 + be16(c->rx + i + 1))) {
            c->blocked = 1;
            break;
        }
        junk(c, js, i);
        process(c, c->rx + i, 4 + be16(c->rx + i + 1), unknown);
        i += 4 + be16(c->rx + i + 1);
        js = i;
    }
    junk(c, js, i);
    memmove(c->rx, c->rx + i, c->rxn - i);
    c->rxn -= i;
}

size_t dv3k_chip_feed(struct dv3k_chip *c, const uint8_t *p, size_t n)
{
    size_t took = 0;
    while (n) {
        size_t k = RXCAP - c->rxn;
        if (k > n)
            k = n;
        if (!k)
            break;
        memcpy(c->rx + c->rxn, p, k);
        c->rxn += k;
        p += k;
        n -= k;
        took += k;
        parse(c);
    }
    return took;
}

int dv3k_chip_poll(struct dv3k_chip *c)
{
    int any = 0;
    submit_ready(c);
    while (c->nq && __atomic_load_n(&c->jobs[c->q0].done, __ATOMIC_ACQUIRE)) {
        job_finish(c, &c->jobs[c->q0]);
        c->q0 = (c->q0 + 1) % NJOB;
        c->nq--;
        any = 1;
    }
    if (any && c->blocked)
        parse(c);
    return c->nq;
}

int dv3k_chip_pending(const struct dv3k_chip *c) { return c->rxn > 0 && !c->blocked; }

void dv3k_chip_stall(struct dv3k_chip *c)
{
    if (!c->rxn || c->blocked)
        return;
    warnf(c, "host stalled mid-packet (%zu bytes buffered), dropping that start byte", c->rxn);
    junk(c, 0, 1);
    memmove(c->rx, c->rx + 1, --c->rxn);
    parse(c);
}

struct dv3k_chip *dv3k_chip_new(const struct dv3k_chip_opts *o, dv3k_emit_fn emit, dv3k_warn_fn warn, void *ctx)
{
    struct dv3k_chip *c = calloc(1, sizeof *c);
    const char *v;
    if (!c)
        return NULL;
    c->model = o->model == 3003 ? 3003 : 3000;
    c->nch = c->model == 3003 ? 3 : 1;
    c->be = o->be;
    c->emit = emit;
    c->warn = warn;
    c->submit = o->submit;
    c->ctx = ctx;
    if (!(c->jobs = calloc(NJOB, sizeof *c->jobs))) {
        free(c);
        return NULL;
    }
    snprintf(c->prodid, sizeof c->prodid, "%s", o->prodid ? o->prodid : c->model == 3003 ? "AMBE3003" : "AMBE3000R");
    v = o->version ? o->version : DEF_VERSION;
    memcpy(c->version, v, strnlen(v, VERLEN - 1));
    chip_reset(c);
    return c;
}

void dv3k_chip_free(struct dv3k_chip *c)
{
    int i;
    if (!c)
        return;
    for (i = 0; i < c->nch; i++)
        if (c->ch[i].inst)
            c->be->destroy(c->ch[i].inst);
    free(c->jobs);
    free(c);
}
