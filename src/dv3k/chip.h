#ifndef DV3K_CHIP_H
#define DV3K_CHIP_H

#include <stddef.h>
#include <stdint.h>
#include "dv3k_backend.h"

struct dv3k_chip;
struct dv3k_job;

/* async: hands j to a worker, which calls dv3k_job_run(j) once, any thread */
typedef void (*dv3k_submit_fn)(void *ctx, struct dv3k_job *j);

struct dv3k_chip_opts {
    int model;
    const char *prodid;
    const char *version;
    const struct dv3k_backend_ops *be;
    /* NULL: codec calls run inside dv3k_chip_feed. set: they become jobs,
       submitted once their channel's earlier jobs finish */
    dv3k_submit_fn submit;
};

typedef void (*dv3k_emit_fn)(void *ctx, const char *dir, const uint8_t *p, size_t n, const char *desc);
typedef void (*dv3k_warn_fn)(void *ctx, const char *msg);

struct dv3k_chip *dv3k_chip_new(const struct dv3k_chip_opts *o, dv3k_emit_fn emit, dv3k_warn_fn warn, void *ctx);
void dv3k_chip_free(struct dv3k_chip *c);
size_t dv3k_chip_feed(struct dv3k_chip *c, const uint8_t *p, size_t n);
/* async, chip's thread: submits free channels' jobs, sends finished replies
   in packet order, then parses waiting packets, which can emit, submit, and
   reconfigure backends. returns jobs in flight, call after any job finishes */
int dv3k_chip_poll(struct dv3k_chip *c);
/* any thread, then dv3k_chip_poll sends the reply */
void dv3k_job_run(struct dv3k_job *j);
int dv3k_chip_pending(const struct dv3k_chip *c);
void dv3k_chip_stall(struct dv3k_chip *c);

#endif
