/*
 *
 * honggfuzz - architecture dependent code (LINUX/PERF)
 * -----------------------------------------
 *
 * Author: Robert Swiecki <swiecki@google.com>
 *
 * Copyright 2010-2015 by Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 *
 */

#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <linux/sysctl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "files.h"
#include "linux/perf.h"
#include "log.h"
#include "util.h"

#define _HF_RT_SIG (SIGRTMIN + 10)

/* Buffer used with BTS (branch recording) */
static __thread uint8_t *perfMmapBuf = NULL;
/* By default it's 1MB which allows to run 1 fuzzing thread */
static __thread size_t perfMmapSz = 0UL;
/* Unique path counter */
static __thread uint64_t perfBranchesCnt = 0;
/* Have we seen PERF_RECRORD_LOST events */
static __thread uint64_t perfRecordsLost = 0;
/* Don't record branches using address above this parameter */
static __thread uint64_t perfCutOffAddr = ~(0ULL);
/* Perf method - to be used in signal handlers */
static __thread dynFileMethod_t perfDynamicMethod = _HF_DYNFILE_NONE;
/* Page Size for the current arch */
static __thread size_t perfPageSz = 0x0;

#if __BITS_PER_LONG == 64
#define _HF_PERF_BLOOM_SZ (size_t)(1024ULL * 1024ULL * 1024ULL)
#elif __BITS_PER_LONG == 32
#define _HF_PERF_BLOOM_SZ (1024ULL * 1024ULL * 128ULL)
#else
#error "__BITS_PER_LONG not defined"
#endif
static __thread uint8_t *perfBloom = NULL;

static size_t arch_perfCountBranches(void)
{
    return perfBranchesCnt;
}

static inline void arch_perfAddBranch(uint64_t from, uint64_t to)
{
    /*
     * Kernel sometimes reports branches from the kernel (iret), we are not interested in that as it
     * makes the whole concept of unique branch counting less predictable
     */
    if (__builtin_expect(from > 0xFFFFFFFF00000000, false)
        || __builtin_expect(to > 0xFFFFFFFF00000000, false)) {
        LOG_D("Adding branch %#018" PRIx64 " - %#018" PRIx64, from, to);
        return;
    }
    if (from >= perfCutOffAddr || to >= perfCutOffAddr) {
        return;
    }

    register size_t pos = 0ULL;
    if (perfDynamicMethod == _HF_DYNFILE_UNIQUE_BLOCK_COUNT) {
        pos = from % (_HF_PERF_BLOOM_SZ * 8);
    } else if (perfDynamicMethod == _HF_DYNFILE_UNIQUE_EDGE_COUNT) {
        pos = (from * to) % (_HF_PERF_BLOOM_SZ * 8);
    }

    size_t byteOff = pos / 8;
    uint8_t bitSet = (uint8_t) (1 << (pos % 8));

    register uint8_t prev = __sync_fetch_and_or(&perfBloom[byteOff], bitSet);
    if (!(prev & bitSet)) {
        perfBranchesCnt++;
    }
}

static inline uint64_t arch_perfGetMmap64(uint64_t off)
{
    return *(uint64_t *) (perfMmapBuf + perfPageSz + off);
}

static inline size_t arch_perfMmapBufferSize(size_t dataHeadOff, size_t dataTailOff)
{
    size_t perfSz = 0;
    if (dataHeadOff >= dataTailOff) {
        perfSz = dataHeadOff - dataTailOff;
    } else {
        perfSz = (perfMmapSz - dataTailOff) + dataHeadOff;
    }
    return perfSz;
}

/* Memory Barriers */
#define rmb()	__asm__ __volatile__("":::"memory")
#define wmb()	__sync_synchronize()
static inline void arch_perfMmapParse(void)
{
    struct perf_event_mmap_page *pem = (struct perf_event_mmap_page *)perfMmapBuf;

    /* Memory barrier - needed as per perf_event_open(2) */
    register size_t dataHeadOff = pem->data_head % perfMmapSz;
    register size_t dataTailOff = pem->data_tail % perfMmapSz;
    rmb();

    for (;;) {
        size_t perfSz = arch_perfMmapBufferSize(dataHeadOff, dataTailOff);
        if (perfSz < sizeof(struct perf_event_header)) {
            break;
        }

        uint64_t tmp = arch_perfGetMmap64(dataTailOff);
        struct perf_event_header *peh = (struct perf_event_header *)&tmp;

        if (perfSz < (peh->size + sizeof(struct perf_event_header))) {
            break;
        }
        dataTailOff = (dataTailOff + sizeof(uint64_t)) % perfMmapSz;

        if (__builtin_expect(peh->type != PERF_RECORD_SAMPLE, false)) {
            if (__builtin_expect(peh->type == PERF_RECORD_LOST, false)) {
                /* It's id an we can ignore it */
                arch_perfGetMmap64(dataTailOff);
                register uint64_t lost = arch_perfGetMmap64(dataTailOff);
                perfRecordsLost += lost;
                dataTailOff = (dataTailOff + sizeof(uint64_t)) % perfMmapSz;
                continue;
            } else {
                LOG_W("(struct perf_event_header)->type != PERF_RECORD_SAMPLE (%" PRIu16
                      "), size: %" PRIu16, peh->type, peh->size);
                dataTailOff = (dataTailOff + peh->size) % perfMmapSz;
            }
            continue;
        }

        register uint64_t from = arch_perfGetMmap64(dataTailOff);
        dataTailOff = (dataTailOff + sizeof(uint64_t)) % perfMmapSz;
        register uint64_t to = 0ULL;
        if (perfDynamicMethod == _HF_DYNFILE_UNIQUE_EDGE_COUNT) {
            to = arch_perfGetMmap64(dataTailOff);
            dataTailOff = (dataTailOff + sizeof(uint64_t)) % perfMmapSz;
        }
        arch_perfAddBranch(from, to);
    }

    pem->data_tail = dataTailOff;
    wmb();
}

static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd,
                            unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event, (uintptr_t) pid, (uintptr_t) cpu,
                   (uintptr_t) group_fd, (uintptr_t) flags);
}

static void arch_perfSigHandler(int signum)
{
    if (__builtin_expect(signum != _HF_RT_SIG, false)) {
        return;
    }
    arch_perfMmapParse();
    return;
}

static size_t arch_perfGetMmapBufSz(honggfuzz_t * hfuzz)
{
    char mlock_len[128];
    size_t sz =
        files_readFileToBufMax("/proc/sys/kernel/perf_event_mlock_kb", (uint8_t *) mlock_len,
                               sizeof(mlock_len) - 1);
    if (sz == 0U) {
        LOG_F("Couldn't read '/proc/sys/kernel/perf_event_mlock_kb'");
    }
    mlock_len[sz] = '\0';
    size_t ret = (strtoul(mlock_len, NULL, 10) * 1024) / hfuzz->threadsMax;

    for (size_t i = 1; i < 31; i++) {
        size_t mask = (1U << i);
        size_t maskret = (ret & ~(mask - 1));
        if (maskret == mask) {
            LOG_D("perf_mmap_buf_size = %zu", maskret);
            return maskret;
        }
    }
    LOG_F("Couldn't find the proper size of the perf mmap buffer");
    return false;
}

static bool arch_perfOpen(pid_t pid, dynFileMethod_t method, int *perfFd)
{
    LOG_D("Enabling PERF for PID=%d (mmapBufSz=%zu), method=%x", pid, perfMmapSz, method);

    perfDynamicMethod = method;
    perfBranchesCnt = 0;
    perfRecordsLost = 0;

    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(struct perf_event_attr));
    pe.size = sizeof(struct perf_event_attr);
    pe.disabled = 0;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.exclude_callchain_kernel = 1;
    pe.enable_on_exec = 1;
    pe.type = PERF_TYPE_HARDWARE;

    switch (method) {
    case _HF_DYNFILE_INSTR_COUNT:
        LOG_D("Using: PERF_COUNT_HW_INSTRUCTIONS for PID: %d", pid);
        pe.config = PERF_COUNT_HW_INSTRUCTIONS;
        pe.inherit = 1;
        break;
    case _HF_DYNFILE_BRANCH_COUNT:
        LOG_D("Using: PERF_COUNT_HW_BRANCH_INSTRUCTIONS for PID: %d", pid);
        pe.config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS;
        pe.inherit = 1;
        break;
    case _HF_DYNFILE_UNIQUE_BLOCK_COUNT:
        LOG_D("Using: PERF_SAMPLE_BRANCH_STACK/PERF_SAMPLE_IP for PID: %d", pid);
        pe.config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS;
        pe.sample_type = PERF_SAMPLE_IP;
        pe.sample_period = 1;   /* It's BTS based, so must be equal to 1 */
        pe.watermark = 1;
        pe.wakeup_events = perfMmapSz / 2;
        break;
    case _HF_DYNFILE_UNIQUE_EDGE_COUNT:
        LOG_D("Using: PERF_SAMPLE_BRANCH_STACK/PERF_SAMPLE_IP|PERF_SAMPLE_ADDR for PID: %d", pid);
        pe.config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS;
        pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_ADDR;
        pe.sample_period = 1;   /* It's BTS based, so must be equal to 1 */
        pe.watermark = 1;
        pe.wakeup_events = perfMmapSz / 2;
        break;
    default:
        LOG_E("Unknown perf mode: '%d' for PID: %d", method, pid);
        return false;
        break;
    }

    *perfFd = perf_event_open(&pe, pid, -1, -1, 0);
    if (*perfFd == -1) {
        if (method == _HF_DYNFILE_UNIQUE_BLOCK_COUNT || method == _HF_DYNFILE_UNIQUE_EDGE_COUNT) {
            LOG_E
                ("'-LDp'/'-LDe' mode (sample IP/jump) requires LBR/BTS, which present in Intel Haswell "
                 "and newer CPUs (i.e. not in AMD CPUs)");
        }
        PLOG_F("perf_event_open() failed");
        return false;
    }

    if (method != _HF_DYNFILE_UNIQUE_BLOCK_COUNT && method != _HF_DYNFILE_UNIQUE_EDGE_COUNT) {
        return true;
    }

    perfBloom = mmap(NULL, _HF_PERF_BLOOM_SZ, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (perfBloom == MAP_FAILED) {
        perfBloom = NULL;
        PLOG_E("mmap(size=%zu) failed", (size_t) _HF_PERF_BLOOM_SZ);
    }

    perfMmapBuf =
        mmap(NULL, perfMmapSz + getpagesize(), PROT_READ | PROT_WRITE, MAP_SHARED, *perfFd, 0);
    if (perfMmapBuf == MAP_FAILED) {
        perfMmapBuf = NULL;
        PLOG_E("mmap() failed");
        close(*perfFd);
        return false;
    }

    struct sigaction sa = {
        .sa_handler = arch_perfSigHandler,
        .sa_flags = SA_RESTART,
    };
    sigemptyset(&sa.sa_mask);
    if (sigaction(_HF_RT_SIG, &sa, NULL) == -1) {
        PLOG_E("sigaction() failed");
        return false;
    }
    if (fcntl(*perfFd, F_SETFL, O_RDWR | O_NONBLOCK | O_ASYNC) == -1) {
        PLOG_E("fnctl(F_SETFL)");
        close(*perfFd);
        return false;
    }
    if (fcntl(*perfFd, F_SETSIG, _HF_RT_SIG) == -1) {
        PLOG_E("fnctl(F_SETSIG)");
        close(*perfFd);
        return false;
    }
    struct f_owner_ex foe = {
        .type = F_OWNER_TID,
        .pid = syscall(__NR_gettid)
    };
    if (fcntl(*perfFd, F_SETOWN_EX, &foe) == -1) {
        PLOG_E("fnctl(F_SETOWN_EX)");
        close(*perfFd);
        return false;
    }
    return true;
}

bool arch_perfEnable(pid_t pid, honggfuzz_t * hfuzz, perfFd_t * perfFds)
{
    if (hfuzz->dynFileMethod == _HF_DYNFILE_NONE) {
        return true;
    }
    if ((hfuzz->dynFileMethod & _HF_DYNFILE_UNIQUE_BLOCK_COUNT)
        && (hfuzz->dynFileMethod & _HF_DYNFILE_UNIQUE_EDGE_COUNT)) {
        LOG_F
            ("_HF_DYNFILE_UNIQUE_BLOCK_COUNT and _HF_DYNFILE_UNIQUE_EDGE_COUNT cannot be specified together");
    }

    perfBloom = NULL;
    perfPageSz = getpagesize();

    perfMmapSz = arch_perfGetMmapBufSz(hfuzz);
    perfCutOffAddr = hfuzz->dynamicCutOffAddr;

    perfFds->cpuInstrFd = -1;
    perfFds->cpuBranchFd = -1;
    perfFds->uniquePcFd = -1;
    perfFds->uniqueEdgeFd = -1;

    if (hfuzz->dynFileMethod & _HF_DYNFILE_INSTR_COUNT) {
        if (arch_perfOpen(pid, _HF_DYNFILE_INSTR_COUNT, &perfFds->cpuInstrFd) == false) {
            LOG_E("Cannot set up perf for PID=%d (_HF_DYNFILE_INSTR_COUNT)", pid);
            goto out;
        }
    }
    if (hfuzz->dynFileMethod & _HF_DYNFILE_BRANCH_COUNT) {
        if (arch_perfOpen(pid, _HF_DYNFILE_BRANCH_COUNT, &perfFds->cpuBranchFd) == false) {
            LOG_E("Cannot set up perf for PID=%d (_HF_DYNFILE_BRANCH_COUNT)", pid);
            goto out;
        }
    }
    if (hfuzz->dynFileMethod & _HF_DYNFILE_UNIQUE_BLOCK_COUNT) {
        if (arch_perfOpen(pid, _HF_DYNFILE_UNIQUE_BLOCK_COUNT, &perfFds->uniquePcFd) == false) {
            LOG_E("Cannot set up perf for PID=%d (_HF_DYNFILE_UNIQUE_BLOCK_COUNT)", pid);
            goto out;
        }
    }
    if (hfuzz->dynFileMethod & _HF_DYNFILE_UNIQUE_EDGE_COUNT) {
        if (arch_perfOpen(pid, _HF_DYNFILE_UNIQUE_EDGE_COUNT, &perfFds->uniqueEdgeFd) == false) {
            LOG_E("Cannot set up perf for PID=%d (_HF_DYNFILE_UNIQUE_EDGE_COUNT)", pid);
            goto out;
        }
    }

    return true;
 out:
    close(perfFds->cpuInstrFd);
    close(perfFds->cpuBranchFd);
    close(perfFds->uniquePcFd);
    close(perfFds->uniqueEdgeFd);
    return false;
}

void arch_perfAnalyze(honggfuzz_t * hfuzz, fuzzer_t * fuzzer, perfFd_t * perfFds)
{
    if (hfuzz->dynFileMethod == _HF_DYNFILE_NONE) {
        return;
    }

    uint64_t instrCount = 0;
    if (hfuzz->dynFileMethod & _HF_DYNFILE_INSTR_COUNT) {
        ioctl(perfFds->cpuInstrFd, PERF_EVENT_IOC_DISABLE, 0);
        if (read(perfFds->cpuInstrFd, &instrCount, sizeof(instrCount)) != sizeof(instrCount)) {
            PLOG_E("read(perfFd='%d') failed", perfFds->cpuInstrFd);
        }
        close(perfFds->cpuInstrFd);
    }

    uint64_t branchCount = 0;
    if (hfuzz->dynFileMethod & _HF_DYNFILE_BRANCH_COUNT) {
        ioctl(perfFds->cpuBranchFd, PERF_EVENT_IOC_DISABLE, 0);
        if (read(perfFds->cpuBranchFd, &branchCount, sizeof(branchCount)) != sizeof(branchCount)) {
            PLOG_E("read(perfFd='%d') failed", perfFds->cpuBranchFd);
        }
        close(perfFds->cpuBranchFd);
    }

    uint64_t pathCount = 0;
    if (hfuzz->dynFileMethod & _HF_DYNFILE_UNIQUE_BLOCK_COUNT) {
        ioctl(perfFds->uniquePcFd, PERF_EVENT_IOC_DISABLE, 0);
        close(perfFds->uniquePcFd);
        arch_perfMmapParse();
        pathCount = arch_perfCountBranches();

        if (perfRecordsLost > 0UL) {
            LOG_W("%" PRIu64
                  " PERF_RECORD_LOST events received, possibly too many concurrent fuzzing threads in progress",
                  perfRecordsLost);
        }
    }

    uint64_t edgeCount = 0;
    if (hfuzz->dynFileMethod & _HF_DYNFILE_UNIQUE_EDGE_COUNT) {
        ioctl(perfFds->uniqueEdgeFd, PERF_EVENT_IOC_DISABLE, 0);
        close(perfFds->uniqueEdgeFd);
        arch_perfMmapParse();
        edgeCount = arch_perfCountBranches();

        if (perfRecordsLost > 0UL) {
            LOG_W("%" PRIu64
                  " PERF_RECORD_LOST events received, possibly too many concurrent fuzzing threads in progress",
                  perfRecordsLost);
        }
    }

    if (perfMmapBuf != NULL) {
        munmap(perfMmapBuf, perfMmapSz + getpagesize());
    }
    if (perfBloom != NULL) {
        munmap(perfBloom, _HF_PERF_BLOOM_SZ);
    }

    fuzzer->hwCnts.cpuInstrCnt = instrCount;
    fuzzer->hwCnts.cpuBranchCnt = branchCount;
    fuzzer->hwCnts.pcCnt = pathCount;
    fuzzer->hwCnts.pathCnt = edgeCount;
}
