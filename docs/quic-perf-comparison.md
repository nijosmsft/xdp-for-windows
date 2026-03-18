# QUIC Performance Comparison Results

## Introduction

This report investigates the performance benefits of distributing QUIC (MsQuic) processing across more CPU cores than the default NIC RSS configuration provides. On our test hardware, the NIC's RSS is configured with 8 queues, leaving the remaining cores available for additional work.

We compare two approaches to spread QUIC traffic beyond the RSS CPUs:

- **XDP RSS**: Uses the XDP driver to intercept packets at the NIC level and redirect them to additional CPUs. For QUIC traffic, CPU selection is based on the partition index embedded in the QUIC Connection ID by MsQuic, ensuring packets are always delivered to the CPU that owns the connection. Requires installing the XDP driver on the server.
- **MsQuic RSS**: Uses a change in MsQuic to reassign new connections to workers on additional CPUs. No driver changes required — purely a MsQuic software change. However, packets still physically arrive on the original 8 RSS CPUs.

We tested each approach across five dimensions (40 tests total):

| Dimension | Options | What it isolates |
|-----------|---------|-----------------|
| **Approach** | Baseline, XDP RSS, MsQuic RSS | Which spreading mechanism works best |
| **Mode** | User, Kernel (WSK) | User vs Kernel MsQuic|
| **CPU range** | 24 CPUs (40-63), 32 CPUs (40-79) | Whether more CPUs improve throughput |
| **Affinity** | Core, Group | Whether pinning workers to CPUs helps or hurts |
| **Scenario** | RPS (request/response), Download (bulk) | How each approach handles different workloads |

**Primary baseline**: Core affinity represents the correct, intended behavior of MsQuic with secnetperf's `-affinitize:1` flag. Group affinity baseline is also reported because a secnetperf bug caused earlier testing to run in group affinity mode unintentionally, which produced some interesting results (see [FAQ](#why-test-both-core-affinity-and-group-affinity)). All percentage comparisons are shown against both baselines.

**Bottom line:**

- **Kernel mode**: Both XDP RSS and MsQuic RSS push MsQuic past **1 million RPS** (vs 466K baseline) and **93 Gbps download** near line rate on 100 GbE (vs 54 Gbps baseline). XDP RSS delivers tighter tail latency (p99=3ms at 1.14M RPS). MsQuic RSS achieves the highest absolute RPS (1.25M) and doesn't require a driver.
- **User mode**: XDP RSS reaches **1.08M RPS**. MsQuic RSS shows improvement only over the core affinity baseline (+14-45%), but is slower than the group affinity baseline (-25% to -41%).

## Test Environment

- **Server**: Dell PowerEdge R650, dual Xeon Silver 4316 (80 logical CPUs)
- **NIC**: Mellanox ConnectX-6 Dx 100 GbE, 8 RSS queues (CPUs 64,66,68,70,72,74,76,78)
- **Clients**: 6 Windows machines
- **Connections**: 64 per client x 4 streams = 1,536 total streams
- **Duration**: 30 seconds per test
- **Encryption**: TLS enabled (Schannel)
- **Tool**: MsQuic `secnetperf.exe` (user-mode and kernel-mode WSK via `secnetperfdrvpriv.sys`)

## Glossary

| Term | Description |
|------|-------------|
| **XDP RSS** | Spreads traffic by redirecting **packets** to additional CPUs using the XDP driver. Every packet in a connection lands on the same target CPU. Requires XDP driver installation. |
| **MsQuic RSS** | Spreads traffic by reassigning **connections** to workers on additional CPUs. Packets still arrive on RSS CPUs, but the QUIC worker processing them runs elsewhere. No driver required. |
| **Core affinity** | Each MsQuic worker thread is locked to exactly one CPU. Provides deterministic placement but idle CPUs can't help busy ones. See [FAQ](#why-test-both-core-affinity-and-group-affinity). |
| **Group affinity** | Each MsQuic worker thread prefers one CPU but the OS can move it to another if needed. More flexible but less predictable. See [FAQ](#why-test-both-core-affinity-and-group-affinity). |
| **RSS CPUs** | The 8 CPUs (64,66,68,70,72,74,76,78) that the NIC delivers packets to. These are the bottleneck in the baseline configuration. |
| **HT siblings** | CPUs (65,67,69,71,73,75,77,79) that share a physical core with RSS CPUs. |
| **40-63** | 24 "clean" CPUs with no NIC processing overhead. |
| **40-79** | 32 effective CPUs (40-63 plus the 8 RSS CPUs, skipping HT siblings). |
| **User mode** | QUIC runs in the application process (secnetperf.exe). |
| **Kernel mode** | QUIC runs as a kernel driver (secnetperfdrvpriv.sys via WSK) |

## Highlights

| Category | Approach | CPUs | Mode | Affinity | Throughput | Tail latency (p99 / p99.9) |
|----------|----------|------|------|----------|------------|---------------------------|
| Highest RPS | MsQuic RSS | 32 | kernel | group | **1,247,010 RPS** | 5,696 / 31,902us |
| Best RPS + latency | XDP RSS | 32 | kernel | core | **1,135,917 RPS** | 3,088 / 3,415us |
| Tightest tail latency | MsQuic RSS | 24 | kernel | core | **1,026,936 RPS** | 1,775 / 1,975us |
| Best user-mode RPS | XDP RSS | 32 | user | group | **1,078,502 RPS** | 4,467 / 6,863us |
| Best download | MsQuic RSS | 32 | kernel | core | **93.7 Gbps** | — |

Note: The highest RPS configuration (1.25M) has 16x worse p99.9 latency than the best-balanced configuration (1.14M). Throughput and tail latency trade off — the right choice depends on the application's sensitivity to outlier latency.

## Interesting Findings

1. **MsQuic RSS matches or outperforms XDP RSS in kernel mode**. RPS: 1.15M vs 1.14M. Download: 93.7 vs 88.5 Gbps (both at 40-79 core). Connection-level redistribution (MsQuic RSS) performs on par with packet-level redistribution (XDP RSS) in kernel mode — without requiring a driver. However, in user mode MsQuic RSS shows limited gains (+14-45% over core baseline) and is slower than the group affinity baseline, while XDP RSS reaches 1.08M RPS.

2. **Group affinity baseline is 1.94x core affinity baseline** (710K vs 365K RPS). With core affinity, 80 workers are pinned to 80 CPUs but only 8 have RSS traffic. Note: this was measured on an idle test system. In production with other workloads competing for CPU, group affinity behavior depends on OS scheduler decisions and may be less predictable.

3. **Steering to HT siblings of RSS CPUs worsens performance**. The 40-79 range includes HT siblings (65,67,...,79) which share physical cores with RSS CPUs. These are excluded from targets in all tests. Early testing without excluding them showed performance degrading.

## RPS Scenario (512B request, 8KB response)

### Baseline RPS

| Mode   | Affinity | CPUs | RPS     | p50us | p99us | p99.9us | p99.99us |
|--------|----------|------|---------|-------|-------|---------|----------|
| user   | core     | 8    | 365,468 | 4,171 | 4,789 | 13,204  | 26,292   |
| user   | group    | 8    | 709,799 | 1,738 | 6,019 | 6,929   | 23,728   |
| kernel | core     | 8    | 466,362 | 3,323 | 3,697 | 3,874   | 19,265   |
| kernel | group    | 8    | 502,237 | 2,647 | 4,006 | 4,970   | 18,527   |

### XDP RSS RPS

| CPUs  | Mode   | Affinity | Count | RPS       | vs base(core) | vs base(group) | vs MsQuic RSS | p50us | p99us | p99.9us | p99.99us |
|-------|--------|----------|-------|-----------|---------------|----------------|---------------|-------|-------|---------|----------|
| 40-63 | user   | core     | 24    | 784,950   | +115%         | +11%           | +67%          | 1,610 | 4,370 | 5,589   | 17,251   |
| 40-63 | user   | group    | 24    | 1,055,490 | +189%         | +49%           | +103%         | 1,095 | 4,848 | 11,164  | 26,579   |
| 40-79 | user   | core     | 32    | 1,013,467 | +177%         | +43%           | +143%         | 1,133 | 3,664 | 4,237   | 21,247   |
| 40-79 | user   | group    | 32    | 1,078,502 | +195%         | +52%           | +104%         | 1,099 | 4,467 | 6,863   | 22,429   |
| 40-63 | kernel | core     | 24    | 900,314   | +93%          | +79%           | -12%          | 1,818 | 2,621 | 2,869   | 6,552    |
| 40-63 | kernel | group    | 24    | 960,375   | +106%         | +91%           | -12%          | 1,430 | 8,251 | 16,122  | 16,858   |
| 40-79 | kernel | core     | 32    | 1,135,917 | +144%         | +126%          | -1%           | 1,070 | 3,088 | 3,415   | 6,556    |
| 40-79 | kernel | group    | 32    | 1,138,599 | +144%         | +127%          | -9%           | 916   | 4,481 | 14,359  | 16,383   |

### MsQuic RSS RPS

| CPUs  | Mode   | Affinity | Count | RPS       | vs base(core) | vs base(group) | vs XDP RSS | p50us | p99us  | p99.9us | p99.99us |
|-------|--------|----------|-------|-----------|---------------|----------------|------------|-------|--------|---------|----------|
| 40-63 | user   | core     | 24    | 469,974   | +29%          | -34%           | -40%       | 2,712 | 3,229  | 6,882   | 23,060   |
| 40-63 | user   | group    | 24    | 521,170   | +43%          | -27%           | -51%       | 2,463 | 7,087  | 8,494   | 23,435   |
| 40-79 | user   | core     | 32    | 416,386   | +14%          | -41%           | -59%       | 3,722 | 4,324  | 43,981  | 49,674   |
| 40-79 | user   | group    | 32    | 529,259   | +45%          | -25%           | -51%       | 2,463 | 6,950  | 8,203   | 24,035   |
| 40-63 | kernel | core     | 24    | 1,026,936 | +120%         | +104%          | +14%       | 1,480 | 1,775  | 1,975   | 7,053    |
| 40-63 | kernel | group    | 24    | 1,093,995 | +135%         | +118%          | +14%       | 1,273 | 4,460  | 7,854   | 17,561   |
| 40-79 | kernel | core     | 32    | 1,151,535 | +147%         | +129%          | +1%        | 1,132 | 3,439  | 4,243   | 8,212    |
| 40-79 | kernel | group    | 32    | 1,247,010 | +167%         | +148%          | +10%       | 1,034 | 5,696  | 31,902  | 72,791   |

## Download Scenario

### Baseline Download

| Mode   | Affinity | CPUs | Gbps |
|--------|----------|------|------|
| user   | core     | 8    | 50.3 |
| user   | group    | 8    | 73.3 |
| kernel | core     | 8    | 54.5 |
| kernel | group    | 8    | 52.5 |

### XDP RSS Download

| CPUs  | Mode   | Affinity | Count | Gbps | vs base(core) | vs base(group) | vs MsQuic RSS |
|-------|--------|----------|-------|------|---------------|----------------|---------------|
| 40-63 | user   | core     | 24    | 85.1 | +69%          | +16%           | +2%           |
| 40-63 | user   | group    | 24    | 48.4 | -4%           | -34%           | -48%          |
| 40-79 | user   | core     | 32    | 93.3 | +86%          | +27%           | +5%           |
| 40-79 | user   | group    | 32    | 60.1 | +19%          | -18%           | -34%          |
| 40-63 | kernel | core     | 24    | 89.5 | +64%          | +70%           | +1%           |
| 40-63 | kernel | group    | 24    | 87.8 | +61%          | +67%           | +4%           |
| 40-79 | kernel | core     | 32    | 88.5 | +62%          | +69%           | -6%           |
| 40-79 | kernel | group    | 32    | 75.6 | +39%          | +44%           | -10%          |

### MsQuic RSS Download

| CPUs  | Mode   | Affinity | Count | Gbps | vs base(core) | vs base(group) | vs XDP RSS |
|-------|--------|----------|-------|------|---------------|----------------|------------|
| 40-63 | user   | core     | 24    | 83.2 | +65%          | +14%           | -2%        |
| 40-63 | user   | group    | 24    | 92.4 | +84%          | +26%           | +91%       |
| 40-79 | user   | core     | 32    | 89.2 | +77%          | +22%           | -4%        |
| 40-79 | user   | group    | 32    | 91.7 | +82%          | +25%           | +53%       |
| 40-63 | kernel | core     | 24    | 88.9 | +63%          | +69%           | -1%        |
| 40-63 | kernel | group    | 24    | 84.4 | +55%          | +61%           | -4%        |
| 40-79 | kernel | core     | 32    | 93.7 | +72%          | +78%           | +6%        |
| 40-79 | kernel | group    | 32    | 83.8 | +54%          | +60%           | +11%       |

## FAQ

### Why test both core affinity and group affinity?

We discovered a bug in secnetperf where `-affinitize:1` was not correctly setting the `QUIC_GLOBAL_EXECUTION_CONFIG_FLAG_AFFINITIZE` flag on the MsQuic execution config. The code checked the wrong variable:

```c
// Bug: checked PerfDefaultHighPriority instead of PerfDefaultAffinitizeThreads
if (PerfDefaultHighPriority) {
    Config->Flags |= QUIC_GLOBAL_EXECUTION_CONFIG_FLAG_AFFINITIZE;
}
```

This meant all our initial tests with `-affinitize:1` were actually running with **group affinity** (ideal processor hint only) — MsQuic workers were not pinned to cores. We were getting ~600K RPS baseline and thought that was the true baseline.

After fixing the bug to correctly check `PerfDefaultAffinitizeThreads`, the baseline dropped to ~365K RPS. Workers were now hard-pinned to 80 CPUs, but only 8 had RSS traffic — 72 workers sat idle on empty CPUs.

Since the results differ significantly between the two modes (e.g., baseline 710K vs 365K RPS), we test both. We added the `-pin:0/1` flag to secnetperf to allow testing both modes. `-affinitize:1` always affinitizes the secnetperf perf client threads. `-pin:1` (default) additionally pins MsQuic worker threads. `-pin:0` leaves MsQuic workers with group affinity only.

