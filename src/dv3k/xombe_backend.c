#include <stdlib.h>
#include <string.h>
#include "dv3k_backend.h"
#include "xombe.h"

enum mode { DMR_FEC, DMR_NOFEC, DSTAR };

static const uint16_t ratep_dmr[6] = { 0x0431, 0x0754, 0x2400, 0x0000, 0x0000, 0x6f48 };
static const uint16_t ratep_dstar[6] = { 0x0130, 0x0763, 0x4000, 0x0000, 0x0000, 0x0048 };

struct inst {
    enum mode mode;
    enum xombe_codec codec;
    xombe_enc_cfg ecfg;
    xombe_fec fec;
    xombe_enc *enc;
    xombe_dec *dec;
    void *enc_mem, *dec_mem;
    uint8_t cni[9];
};

static const uint8_t dstar_dtmf[16] = { 129, 132, 135, 142, 130, 133, 136, 128, 131, 134, 137, 143, 138, 139, 140, 141 };

static int tone_field(const struct inst *k, int field, xombe_enc_tone *t)
{
    int idx = field >> 8 & 0xff, dbm0 = (int8_t)(field & 0xff), ad;
    if (field < 0)
        return 0;
    if ((idx >= 0x05 && idx <= 0x7a) || (idx >= 0xa0 && idx <= 0xa2))
        t->id = idx;
    else if (idx >= 0x80 && idx <= 0x8f)
        t->id = k->mode == DSTAR ? dstar_dtmf[idx & 15] : idx;
    else
        return 0;
    ad = (int)((dbm0 + 86.66) / 0.711 + 0.5);
    t->ad = ad < 0 ? 0 : ad > 127 ? 127 : ad;
    return 1;
}

static void init_enc(struct inst *k) { k->enc = xombe_enc_init(k->enc_mem, &k->ecfg); }

static void init_dec(struct inst *k)
{
    k->dec = xombe_dec_init(k->dec_mem, k->codec);
    xombe_fec_init(&k->fec, k->codec);
}

static void *create(const struct dv3k_chan_cfg *cfg, int channel, int *bits)
{
    struct inst *k;
    enum mode mode;
    (void)channel;
    if (cfg->ratet == 33 || (cfg->ratet < 0 && !memcmp(cfg->ratep, ratep_dmr, sizeof ratep_dmr)))
        mode = DMR_FEC, *bits = 72;
    else if (cfg->ratet == 34)
        mode = DMR_NOFEC, *bits = 49;
    else if (cfg->ratet < 0 && !memcmp(cfg->ratep, ratep_dstar, sizeof ratep_dstar))
        mode = DSTAR, *bits = 72;
    else
        return NULL;

    if (!(k = calloc(1, sizeof *k)))
        return NULL;
    k->mode = mode;
    k->codec = mode == DSTAR ? XOMBE_DSTAR : XOMBE_DMR;
    xombe_enc_cfg_default(&k->ecfg);
    k->ecfg.ns = !!(cfg->ecmode & DV3K_EC_NS_ENABLE);
    k->ecfg.dmr_dtx = !!(cfg->ecmode & DV3K_EC_DTX_ENABLE);
    k->ecfg.tones = !!(cfg->ecmode & DV3K_EC_TD_ENABLE);
    k->ecfg.outputs = mode == DSTAR ? XOMBE_ENC_DSTAR : XOMBE_ENC_DMR;
    k->enc_mem = malloc(xombe_enc_size());
    k->dec_mem = malloc(xombe_dec_size());
    if (!k->enc_mem || !k->dec_mem) {
        free(k->enc_mem), free(k->dec_mem), free(k);
        return NULL;
    }
    init_enc(k);
    if (mode == DSTAR)
        xombe_enc_frame(k->enc, NULL, NULL, NULL, k->cni);
    else
        xombe_enc_frame_bits(k->enc, NULL, NULL, k->cni, NULL);
    init_enc(k);
    init_dec(k);
    return k;
}

static void destroy(void *p)
{
    struct inst *k = p;
    free(k->enc_mem), free(k->dec_mem), free(k);
}

static void config(void *p, const struct dv3k_chan_cfg *cfg)
{
    struct inst *k = p;
    int ns = !!(cfg->ecmode & DV3K_EC_NS_ENABLE), dtx = !!(cfg->ecmode & DV3K_EC_DTX_ENABLE);
    int tones = !!(cfg->ecmode & DV3K_EC_TD_ENABLE);
    if (ns != k->ecfg.ns || dtx != k->ecfg.dmr_dtx || tones != k->ecfg.tones) {
        k->ecfg.ns = ns;
        k->ecfg.dmr_dtx = dtx;
        k->ecfg.tones = tones;
        init_enc(k);
    }
}

static void init(void *p, int what)
{
    if (what & 1)
        init_enc(p);
    if (what & 2)
        init_dec(p);
}

static int encode(void *p, const struct dv3k_frame *f, const int16_t *pcm, uint8_t *out, int nbits)
{
    struct inst *k = p;
    uint8_t *dmr = k->codec == XOMBE_DMR ? out : NULL, *dstar = k->codec == XOMBE_DSTAR ? out : NULL;
    xombe_enc_tone t;
    const xombe_enc_tone *tp = (f->cmode & DV3K_EC_TS_ENABLE) && tone_field(k, f->tone, &t) ? &t : NULL;
    int r;
    (void)nbits;
    if (!k->enc)
        return -1;
    if (k->mode == DMR_NOFEC)
        r = xombe_enc_frame_bits(k->enc, pcm, tp, dmr, dstar);
    else
        r = xombe_enc_frame(k->enc, pcm, tp, dmr, dstar);
    return r < 0 ? -1 : 0;
}

static void dec_in(struct inst *k, const uint8_t *in, int fec, unsigned flags, int16_t *pcm,
                   uint8_t bits[XOMBE_BITS_BYTES])
{
    xombe_fec_stats st;
    memset(bits, 0, XOMBE_BITS_BYTES);
    if (!(flags & XOMBE_FLAG_LOST)) {
        if (fec) {
            flags = xombe_fec_decode(&k->fec, in, bits, &st);
            xombe_dec_set_errors(k->dec, st.eps_t, st.eps1);
        } else {
            memcpy(bits, in, XOMBE_BITS_BYTES);
            bits[6] &= 0x80;
        }
    }
    xombe_dec_frame_bits(k->dec, bits, flags, pcm);
}

static int decode(void *p, const struct dv3k_frame *f, const uint8_t *in, int nbits, int16_t *pcm)
{
    struct inst *k = p;
    int lost = !!(f->cmode & DV3K_DC_LOST_FRAME), dstar = k->mode == DSTAR;
    uint8_t bits[XOMBE_BITS_BYTES];
    xombe_decision dd;
    xombe_enc_tone t;
    uint8_t tf[XOMBE_FRAME_BYTES];

    if ((f->cmode & DV3K_DC_TS_ENABLE) && !lost && tone_field(k, f->tone, &t)) {
        if (!xombe_tone_frame(k->codec, t.id, t.ad, tf)) {
            if (!dstar) {
                xombe_fec clean;
                xombe_fec_init(&clean, XOMBE_DMR);
                xombe_fec_decode(&clean, tf, bits, NULL);
                memcpy(tf, bits, XOMBE_BITS_BYTES);
            }
            dec_in(k, tf, dstar, 0, pcm, bits);
        }
        return 0;
    }

    if ((f->cmode & DV3K_DC_CNI_FRAME) && !lost) {
        dec_in(k, k->cni, dstar, 0, pcm, bits);
        return 0;
    }
    if (nbits < (k->mode == DMR_NOFEC ? 49 : 72) && !lost)
        return -1;
    dec_in(k, in, k->mode != DMR_NOFEC, lost ? XOMBE_FLAG_LOST : 0, pcm, bits);
    if (dstar)
        return 0;
    xombe_dec_decision_get(k->dec, &dd);
    if (dd.cls == XOMBE_CLASS_SILENCE && dd.conceal == XOMBE_CONCEAL_NONE)
        memcpy(k->cni, bits, XOMBE_BITS_BYTES);
    return 0;
}

static const struct dv3k_backend_ops ops = {
    .abi = DV3K_BACKEND_ABI, .name = "xombe", .create = create, .destroy = destroy, .config = config, .init = init,
    .encode = encode, .decode = decode,
};

__attribute__((visibility("default"))) const struct dv3k_backend_ops *dv3k_backend(void) { return &ops; }
