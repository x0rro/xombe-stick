/* one instance's calls come in packet order, never at once, from any thread
   with async workers. different instances' calls can run at once */
#ifndef DV3K_BACKEND_H
#define DV3K_BACKEND_H

#include <stdint.h>

#define DV3K_BACKEND_ABI   1
#define DV3K_FRAME_SAMPLES 160
#define DV3K_MAX_BITS      255

#define DV3K_EC_NS_ENABLE  0x0040
#define DV3K_EC_DTX_ENABLE 0x0800
#define DV3K_EC_TD_ENABLE  0x1000
#define DV3K_EC_TS_ENABLE  0x4000
#define DV3K_DC_LOST_FRAME 0x0004
#define DV3K_DC_CNI_FRAME  0x0008
#define DV3K_DC_TS_ENABLE  0x4000

#define DV3K_COMPAND_ENABLE 0x01
#define DV3K_COMPAND_ALAW   0x02

struct dv3k_chan_cfg {
    int ratet;
    uint16_t ratep[6];
    int bits;
    uint16_t ecmode;
    uint16_t dcmode;
    uint8_t compand;
    int8_t gain_in;
    int8_t gain_out;
    uint16_t chanfmt;
    uint16_t spchfmt;
};

struct dv3k_frame {
    uint16_t cmode;
    int tone;
    const uint8_t *soft;
};

#define DV3K_BF_GAIN 0x1

struct dv3k_backend_ops {
    int abi;
    const char *name;
    unsigned flags;

    void *(*create)(const struct dv3k_chan_cfg *cfg, int channel, int *bits);
    void (*destroy)(void *inst);
    void (*config)(void *inst, const struct dv3k_chan_cfg *cfg);
    void (*init)(void *inst, int what);

    int (*encode)(void *inst, const struct dv3k_frame *f, const int16_t *pcm, uint8_t *out, int nbits);
    int (*decode)(void *inst, const struct dv3k_frame *f, const uint8_t *in, int nbits, int16_t *pcm);
};

typedef const struct dv3k_backend_ops *(*dv3k_backend_fn)(void);

const struct dv3k_backend_ops *dv3k_backend(void);

#endif
