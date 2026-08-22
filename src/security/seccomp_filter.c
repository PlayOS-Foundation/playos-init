/*
 * playos-init/src/security/seccomp_filter.c — S12-T3 seccomp deny-list
 *
 * Constructs a classic seccomp-BPF filter and installs it with
 * prctl(PR_SET_SECCOMP). The filter:
 *
 *   - verifies AUDIT_ARCH_X86_64 and kills any other ABI,
 *   - returns EPERM for privileged/credential syscalls (mount, module
 *     load, ptrace, reboot, setuid/setgid/capset, keyring, bpf, …),
 *   - returns EPERM for prctl(PR_SET_SECCOMP) specifically (games cannot
 *     replace or weaken this filter), while allowing other prctl calls,
 *   - allows everything else; path-based restrictions are enforced by
 *     the Landlock ruleset (S12-T2), not by pointer-dereferencing BPF.
 *
 * The credential syscalls must be denied *after* the caller has already
 * dropped privileges — this filter is applied last for exactly that
 * reason (see security.h ordering).
 */
#define _GNU_SOURCE
#include "playos-init/security.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/prctl.h>

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

/* A classic-BPF program builder with a fixed instruction budget. */
#define BPF_BUDGET 160

struct bpf_builder {
    struct sock_filter insn[BPF_BUDGET];
    size_t             n;
};

static void
emit(struct bpf_builder *b, uint16_t code, uint8_t jt, uint8_t jf, uint32_t k)
{
    if (b->n >= BPF_BUDGET)
        return;
    b->insn[b->n].code = code;
    b->insn[b->n].jt   = jt;
    b->insn[b->n].jf   = jf;
    b->insn[b->n].k    = k;
    b->n++;
}

static void
deny_syscall(struct bpf_builder *b, uint32_t nr)
{
    /* if nr == syscall: fall through to RET EPERM, else skip it */
    emit(b, BPF_LD | BPF_W | BPF_ABS, 0, 0,
         (uint32_t)offsetof(struct seccomp_data, nr));
    emit(b, BPF_JMP | BPF_JEQ | BPF_K, 0, 1, nr);
    emit(b, BPF_RET | BPF_K, 0, 0,
         SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
}

int
playos_security_apply_seccomp(void)
{
    struct bpf_builder    b;
    struct sock_fprog     prog;
    static const uint32_t denied[] = {
        /* Mounts, filesystems, modules */
        165,  /* mount        */
        166,  /* umount2      */
        155,  /* pivot_root   */
        161,  /* chroot       */
        163,  /* acct         */
        167,  /* swapon       */
        168,  /* swapoff      */
        179,  /* quotactl     */
        156,  /* _sysctl      */
        134,  /* uselib       */
        175,  /* init_module  */
        176,  /* delete_module*/
        313,  /* finit_module */
        /* Credentials */
        105,  /* setuid       */
        106,  /* setgid       */
        113,  /* setreuid     */
        114,  /* setregid     */
        117,  /* setresuid    */
        119,  /* setresgid    */
        122,  /* setfsuid     */
        123,  /* setfsgid     */
        126,  /* capset       */
        /* Debug / process control */
        101,  /* ptrace       */
        310,  /* process_vm_readv  */
        311,  /* process_vm_writev */
        /* System control */
        169,  /* reboot       */
        246,  /* kexec_load   */
        320,  /* kexec_file_load */
        164,  /* settimeofday */
        227,  /* clock_settime*/
        159,  /* adjtimex     */
        170,  /* sethostname  */
        171,  /* setdomainname*/
        172,  /* iopl         */
        173,  /* ioperm       */
        135,  /* personality  */
        153,  /* vhangup      */
        133,  /* mknod        */
        272,  /* unshare      */
        308,  /* setns        */
        /* Kernel interfaces a game must never touch */
        317,  /* seccomp      */
        321,  /* bpf          */
        298,  /* perf_event_open */
        323,  /* userfaultfd  */
        248,  /* add_key      */
        249,  /* request_key  */
        250,  /* keyctl       */
        303,  /* name_to_handle_at */
        304,  /* open_by_handle_at */
    };

#ifndef __x86_64__
    /* The syscall table above is x86_64. Other architectures get a
     * no-filter outcome (logged by the caller) rather than a filter
     * built from wrong numbers. */
    errno = ENOTSUP;
    return -1;
#else
    size_t i;

    memset(&b, 0, sizeof(b));

    /* Kill anything that is not x86_64 (no 32-bit compat bypass). */
    emit(&b, BPF_LD | BPF_W | BPF_ABS, 0, 0,
         (uint32_t)offsetof(struct seccomp_data, arch));
    emit(&b, BPF_JMP | BPF_JEQ | BPF_K, 1, 0, AUDIT_ARCH_X86_64);
    emit(&b, BPF_RET | BPF_K, 0, 0, SECCOMP_RET_KILL_PROCESS);

    for (i = 0; i < sizeof(denied) / sizeof(denied[0]); i++)
        deny_syscall(&b, denied[i]);

    /* prctl is allowed except prctl(PR_SET_SECCOMP), which would let the
     * game replace or stack another filter. */
    emit(&b, BPF_LD | BPF_W | BPF_ABS, 0, 0,
         (uint32_t)offsetof(struct seccomp_data, nr));
    emit(&b, BPF_JMP | BPF_JEQ | BPF_K, 0, 3, 157 /* __NR_prctl */);
    emit(&b, BPF_LD | BPF_W | BPF_ABS, 0, 0,
         (uint32_t)offsetof(struct seccomp_data, args[0]));
    emit(&b, BPF_JMP | BPF_JEQ | BPF_K, 0, 1, PR_SET_SECCOMP);
    emit(&b, BPF_RET | BPF_K, 0, 0,
         SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));

    /* Default: allow (paths are enforced by Landlock). */
    emit(&b, BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ALLOW);

    if (b.n >= BPF_BUDGET) {
        errno = E2BIG;
        return -1;
    }

    memset(&prog, 0, sizeof(prog));
    prog.len    = (unsigned short)b.n;
    prog.filter = b.insn;

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0) != 0)
        return -1;

    return 0;
#endif /* __x86_64__ */
}
