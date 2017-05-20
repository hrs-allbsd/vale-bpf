/*
 * Copyright 2017 Yutaro Hayakawa
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <linux/kernel.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/rwlock.h>
#include <linux/string.h>
#include <linux/types.h>

#include <bsd_glue.h> /* from netmap-release */
#include <net/netmap.h>
#include <dev/netmap/netmap_kern.h> /* XXX Provide path in Makefile */

#include <vale_bpf.h>
#include <vale_bpf_extern_func.h>
#include <vale_bpf_int.h>
#include <vale_bpf_kern.h>

static struct vale_bpf_vm *vm;
static rwlock_t vmlock;
static int jit_mode;

static u_int vale_bpf_lookup(struct nm_bdg_fwd *ft, uint8_t *hint,
                             struct netmap_vp_adapter *vpna) {
  uint64_t ret = NM_BDG_NOPORT;

  /* set metadata for external function calls */
  unsigned int me = smp_processor_id();
  vale_bpf_meta[me].pkt_len = ft->ft_len;
  vale_bpf_meta[me].src_port = netmap_bdg_idx(vpna);

  read_lock(&vmlock);

  if (jit_mode) {
    ret = vm->jitted(ft->ft_buf, ft->ft_len);
  } else {
    ret = vale_bpf_exec(vm, ft->ft_buf, ft->ft_len);
  }

  if (ret == (uint64_t)-1) {
    read_unlock(&vmlock);
    RD(1, "vale_bpf_exec failed.");
    return NM_BDG_NOPORT;
  }

  if (ret > NM_BDG_NOPORT) {
    read_unlock(&vmlock);
    return NM_BDG_NOPORT;
  }

  read_unlock(&vmlock);

  RD(1, "dst: %llu", ret);

  return (u_int)ret;
}

static int vale_bpf_load_prog(void *code, size_t code_len, int jit) {
  int ret;
  bool elf = code_len >= SELFMAG && !memcmp(code, ELFMAG, SELFMAG);
  struct vale_bpf_vm *tmpvm = NULL;
  struct vale_bpf_vm *newvm = NULL;

  if (code == NULL) {
    D("code is NULL");
    return -1;
  }

  void *tmp = kmalloc(code_len, GFP_KERNEL);
  if (tmp == NULL) {
    return -1;
  }

  size_t err = copy_from_user(tmp, code, code_len);
  if (err != 0) {
    kfree(tmp);
    return -1;
  }

  newvm = vale_bpf_create();
  if (newvm == NULL) {
    kfree(tmp);
    return -1;
  }

  vale_bpf_register_func(newvm);

  if (elf) {
    ret = vale_bpf_load_elf(newvm, tmp, code_len);
    if (ret < 0) {
      kfree(tmp);
      return -1;
    }
  } else {
    ret = vale_bpf_load(newvm, tmp, code_len);
    if (ret < 0) {
      kfree(tmp);
      return -1;
    }
  }

  if (jit) {
    vale_bpf_jit_fn fn = vale_bpf_compile(newvm);
    if (fn == NULL) {
      D("Failed to compile");
      vale_bpf_destroy(newvm);
      kfree(tmp);
      return -1;
    }
  }

  write_lock(&vmlock);

  /* swap vm instance */
  tmpvm = vm;
  vm = newvm;

  write_unlock(&vmlock);

  /* set jit flag */
  jit_mode = jit;

  /* Cleanup old vm and temporary code */
  vale_bpf_destroy(tmpvm);
  kfree(tmp);

  D("Successfully loaded ebpf program, JIT: %s", jit ? "true" : "false");

  return 0;
}

static int vale_bpf_config(struct nm_ifreq *req) {
  int ret;
  struct vale_bpf_req *r = (struct vale_bpf_req *)req->data;

  switch (r->method) {
    case LOAD_PROG:
      ret = vale_bpf_load_prog(r->prog_data.code,
          r->prog_data.code_len, r->prog_data.jit);
      break;
    default:
      ret = -1;
      break;
  }
  return ret;
}

static struct netmap_bdg_ops vale_bpf_ops = {vale_bpf_lookup, vale_bpf_config,
                                             NULL};

static int vale_bpf_init(void) {
  struct nmreq nmr;

  /* initialize vm */
  vm = vale_bpf_create();
  if (vm == NULL) {
    return -ENOMEM;
  }

  // TODO Load default eBPF program

  vale_bpf_register_func(vm);

  /* prepare metadata for each core */
  vale_bpf_meta = kmalloc(sizeof(struct vale_bpf_metadata) * num_present_cpus(),
                          GFP_KERNEL);
  if (vale_bpf_meta == NULL) {
    vale_bpf_destroy(vm);
    return -ENOMEM;
  }
  memset(vale_bpf_meta, 0,
         sizeof(struct vale_bpf_metadata) * num_present_cpus());

  rwlock_init(&vmlock);  // initialize rwlock for vm

  bzero(&nmr, sizeof(nmr));
  nmr.nr_version = NETMAP_API;
  strlcpy(nmr.nr_name, VALE_NAME, sizeof(nmr.nr_name));
  strlcat(nmr.nr_name, ":", sizeof(nmr.nr_name));
  nmr.nr_cmd = NETMAP_BDG_REGOPS;
  if (netmap_bdg_ctl(&nmr, &vale_bpf_ops)) {
    D("create a bridge named %s beforehand using vale-ctl", nmr.nr_name);
    return -ENOENT;
  }

  D("Loaded vale-bpf");

  return 0;
}

static void vale_bpf_fini(void) {
  struct nmreq nmr;
  int error;
  struct netmap_bdg_ops tmp = {netmap_bdg_learning, NULL, NULL};

  bzero(&nmr, sizeof(nmr));
  nmr.nr_version = NETMAP_API;
  strlcpy(nmr.nr_name, VALE_NAME, sizeof(nmr.nr_name));
  strlcat(nmr.nr_name, ":", sizeof(nmr.nr_name));
  nmr.nr_cmd = NETMAP_BDG_REGOPS;
  error = netmap_bdg_ctl(&nmr, &tmp);

  if (error) {
    D("failed to release VALE bridge %d", error);
  }

  D("Unloaded vale-bpf");

  vale_bpf_destroy(vm);

  kfree(vale_bpf_meta);
}

module_init(vale_bpf_init);
module_exit(vale_bpf_fini);
MODULE_AUTHOR("Yutaro Hayakawa");
MODULE_DESCRIPTION("VALE BPF Module");
MODULE_LICENSE("Dual BSD/GPL");
