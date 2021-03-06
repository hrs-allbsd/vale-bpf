#ifndef _VALE_BPF_EXTERN_FUNC_
#define _VALE_BPF_EXTERN_FUNC_

#if defined(linux)
#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/types.h>
#elif defined(__FreeBSD__)
#include <sys/types.h>
#else
#error Unsupported platform
#endif

#include "vale_bpf_kern.h"

/*
 * This file defines several external functions for
 * our eBPF VM. If you would like to add new external
 * function, please edit this file and
 * include/vale_bpf_extern_func.h
 */

/*
 * Structure for storing metadata such as
 * packet length and source port. This information
 * will be refered from inside of the eBPF context.
 */
struct vale_bpf_metadata {
  uint16_t *pkt_len;
};

/* metadata should be prepared for each cores */
static struct vale_bpf_metadata *vale_bpf_meta;

static void set_pkt_len(uint16_t len) {
  int me = vale_bpf_os_cur_cpu();
  *(vale_bpf_meta[me].pkt_len) = len;
}

static void vale_bpf_register_func(struct vale_bpf_vm *vm) {
  vale_bpf_register(vm, 0, "set_pkt_len", set_pkt_len);
}

#endif
