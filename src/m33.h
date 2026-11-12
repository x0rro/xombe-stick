#ifndef XS_M33_H
#define XS_M33_H

#include <stdint.h>

static void __attribute__((naked, noinline, unused)) call_on_psp(void (*fn)(void) __attribute__((unused)),
                                                                uint32_t *top __attribute__((unused)),
                                                                uint32_t *bottom __attribute__((unused)))
{
    __asm volatile("push {r4, lr}\n"
                   "movs r3, #0\n"
                   "msr psplim, r3\n"
                   "msr psp, r1\n"
                   "msr psplim, r2\n"
                   "mrs r4, control\n"
                   "orr r3, r4, #2\n"
                   "msr control, r3\n"
                   "isb\n"
                   "blx r0\n"
                   "msr control, r4\n"
                   "isb\n"
                   "pop {r4, pc}\n");
}

#endif
