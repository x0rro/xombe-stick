/* core 0: USB from interrupts (USB_POLL_US timer, USB events), so bytes keep
   moving during a codec call
   core 1: the chip model, with the 3000's codec calls inline
   3003: codec calls are jobs on one queue, run by both cores. the chip
   model keeps replies in packet order */
#include <string.h>
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "chip.h"
#include "port.h"
#include "../m33.h"

#ifndef DV3K_MODEL
#define DV3K_MODEL 3000
#endif

#define STALL_US    50000
#define RING        2048
#define STACK_WORDS (24 * 1024 / 4)
#define NJOBQ       8
#define USB_POLL_US 100

struct ring {
    uint8_t b[RING];
    volatile uint32_t head, tail; /* producer writes head, consumer tail */
};

static struct ring rx, tx; /* rx: core 0 -> 1, tx: core 1 -> 0 */
static uint32_t core1_stack[STACK_WORDS];
static volatile uint32_t warnings;

/* core 0 bumps reset_seq per break, then holds host bytes and drops replies
   until core 1 sets reset_done to it. core 1 sets tx_cut first, core 0 also
   drops replies before it */
static volatile uint32_t reset_seq, reset_done, tx_cut;

/* core 0 only */
static int dropping, tx_drop;
static uint32_t tx_left, tx_purge_to;

/* core 1 only */
static int muted;

static uint32_t ring_used(const struct ring *r) { return r->head - r->tail; }
static uint8_t ring_peek(const struct ring *r, uint32_t i) { return r->b[(r->tail + i) & (RING - 1)]; }

static uint32_t ring_put(struct ring *r, const uint8_t *p, uint32_t n)
{
    uint32_t h = r->head, k, i;
    __dmb();
    k = RING - (h - r->tail);
    if (n > k)
        n = k;
    for (i = 0; i < n; i++)
        r->b[(h + i) & (RING - 1)] = p[i];
    __dmb();
    r->head = h + n;
    __sev();
    return n;
}

static uint32_t ring_get(struct ring *r, uint8_t *p, uint32_t n)
{
    uint32_t t = r->tail, k, i;
    __dmb();
    k = r->head - t;
    if (n > k)
        n = k;
    for (i = 0; i < n; i++)
        p[i] = r->b[(t + i) & (RING - 1)];
    __dmb();
    r->tail = t + n;
    __sev();
    return n;
}

#if DV3K_MODEL == 3003
/* pushed by core 1 (chip model's submit), taken by either core */
static struct dv3k_job *jobq[NJOBQ];
static int njob;
static spin_lock_t *jlock;
static uint32_t core0_stack[STACK_WORDS];
static struct dv3k_job *core0_job;

static void submit(void *ctx, struct dv3k_job *j)
{
    uint32_t s = spin_lock_blocking(jlock);
    (void)ctx;
    jobq[njob++] = j;
    spin_unlock(jlock, s);
    __sev();
}

static struct dv3k_job *take_job(void)
{
    struct dv3k_job *j = NULL;
    uint32_t s = spin_lock_blocking(jlock);
    if (njob) {
        j = jobq[0];
        memmove(jobq, jobq + 1, (size_t)--njob * sizeof *jobq);
    }
    spin_unlock(jlock, s);
    return j;
}

static void run_core0_job(void) { dv3k_job_run(core0_job); }
#endif

/* core 1: replies go out whole, waiting while core 0 drains the ring */
static void emit(void *ctx, const char *dir, const uint8_t *p, size_t n, const char *desc)
{
    uint32_t k;
    (void)ctx, (void)desc;
    if (strcmp(dir, "tx") || muted)
        return;
    while (n) {
        k = ring_put(&tx, p, (uint32_t)n);
        p += k, n -= k;
        if (n)
            __wfe();
    }
}

static void warn(void *ctx, const char *msg)
{
    (void)ctx, (void)msg;
    warnings++;
}

/* core 1: jobs in flight finish before the chip is freed */
static struct dv3k_chip *line_reset(struct dv3k_chip *chip, const struct dv3k_chip_opts *o)
{
    static const uint8_t reset[] = { 0x61, 0x00, 0x01, 0x00, 0x33 };
    uint32_t seq = reset_seq;
    uint8_t junk[64];
#if DV3K_MODEL == 3003
    struct dv3k_job *j;
#endif
    muted = 1;
    while (ring_get(&rx, junk, sizeof junk))
        ;
#if DV3K_MODEL == 3003
    while (dv3k_chip_poll(chip))
        if ((j = take_job()))
            dv3k_job_run(j);
        else
            __wfe();
#endif
    dv3k_chip_free(chip);
    chip = dv3k_chip_new(o, emit, warn, NULL);
    tx_cut = tx.head;
    __dmb();
    reset_done = seq;
    __sev();
    muted = 0;
    dv3k_chip_feed(chip, reset, sizeof reset);
    return chip;
}

static void core1_main(void)
{
    struct dv3k_chip_opts o = { .model = DV3K_MODEL, .be = dv3k_backend() };
    struct dv3k_chip *chip;
    uint64_t last = 0;
    uint8_t buf[512];
    uint32_t n = 0, off = 0, k;
#if DV3K_MODEL == 3003
    struct dv3k_job *j;
    o.submit = submit;
#endif
    chip = dv3k_chip_new(&o, emit, warn, NULL);
    for (;;) {
        if (reset_seq != reset_done) {
            chip = line_reset(chip, &o);
            off = n = 0;
            continue;
        }
#if DV3K_MODEL == 3003
        dv3k_chip_poll(chip);
#endif
        if (off == n)
            off = 0, n = ring_get(&rx, buf, sizeof buf);
        k = 0;
        if (off < n) {
            k = (uint32_t)dv3k_chip_feed(chip, buf + off, n - off);
            off += k;
            if (k)
                last = time_us_64();
        } else if (dv3k_chip_pending(chip) && time_us_64() - last >= STALL_US) {
            dv3k_chip_stall(chip);
            last = time_us_64();
        }
#if DV3K_MODEL == 3003
        /* parse first, so later jobs reach core 0 while core 1 runs this one.
           a blocked chip waits on jobs, so run one */
        if ((off == n ? !ring_used(&rx) : !k) && (j = take_job())) {
            dv3k_job_run(j);
            k = 1;
        }
#endif
        if (!k && !dv3k_chip_pending(chip))
            __wfe();
    }
}

/* core 0, from the USB poll */
void host_drop(int closed)
{
    if (closed)
        dropping = 1;
    port_discard();
    if (tx_left)
        tx_drop = 1;
    tx_purge_to = tx.head;
}

void host_reset(void)
{
    host_drop(0);
    port_discard_rx();
    dropping = 0;
    reset_seq++;
    __sev();
}

void host_bootsel(void) { reset_usb_boot(0, 0); }

/* core 0: TinyUSB and both rings, from the timer and from a software
   interrupt raised on each USB event. both at default priority, so neither
   preempts the other */
static void usb_poll(void)
{
    uint8_t buf[256];
    uint32_t n;
    tud_task();
    while (reset_seq == reset_done) {
        n = RING - ring_used(&rx);
        if (n > sizeof buf)
            n = sizeof buf;
        if (!n || !(n = port_read(buf, n)))
            break;
        dropping = 0;
        ring_put(&rx, buf, n);
    }
    for (;;) {
        if (!tx_left) {
            if (ring_used(&tx) < 3)
                break;
            __dmb();
            tx_left = ring_peek(&tx, 0) == 0x61 ? 4 + (ring_peek(&tx, 1) << 8 | ring_peek(&tx, 2)) : 1;
            tx_drop = dropping || reset_seq != reset_done || (int32_t)(tx_purge_to - tx.tail) > 0 ||
                      (int32_t)(tx_cut - tx.tail) > 0;
        }
        n = tx_drop ? sizeof buf : port_write_room();
        if (n > sizeof buf)
            n = sizeof buf;
        if (n > tx_left)
            n = tx_left;
        if (!n || !(n = ring_get(&tx, buf, n)))
            break;
        tx_left -= n;
        if (!tx_drop)
            port_write(buf, n);
    }
    port_flush();
}

static int usb_irq;

static bool usb_timer(repeating_timer_t *t)
{
    (void)t;
    usb_poll();
    return true;
}

void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr)
{
    (void)rhport;
    if (in_isr) {
        port_event(eventid);
        irq_set_pending((uint)usb_irq);
    }
}

int main(void)
{
    static repeating_timer_t poll;
#if DV3K_MODEL == 3003
    jlock = spin_lock_init(spin_lock_claim_unused(true));
    if (vreg_get_voltage() == VREG_VOLTAGE_1_10)
        set_sys_clock_khz(200000, true);
#endif
    multicore_launch_core1_with_stack(core1_main, core1_stack, sizeof core1_stack);
    usb_irq = user_irq_claim_unused(true);
    irq_set_exclusive_handler((uint)usb_irq, usb_poll);
    irq_set_priority((uint)usb_irq, PICO_DEFAULT_IRQ_PRIORITY);
    irq_set_enabled((uint)usb_irq, true);
    tusb_init();
    add_repeating_timer_us(-USB_POLL_US, usb_timer, NULL, &poll);
    for (;;) {
#if DV3K_MODEL == 3003
        if ((core0_job = take_job())) {
            call_on_psp(run_core0_job, core0_stack + STACK_WORDS, core0_stack);
            __sev();
            continue;
        }
#endif
        __wfe();
    }
}
