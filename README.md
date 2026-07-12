# TLBanalyser

An htop-style terminal analyser for **TLB shootdowns, IPIs, and L1/L2 cache
behaviour** on Intel Xeon and AMD EPYC class systems.

Built to answer one question precisely: *after you have already turned off
NUMA balancing and shootdowns dropped but did not disappear — who is sending
the remaining TLB shootdown IPIs, and from which kernel path?*

```
 TLBanalyser 1.0.0 Intel(R) Xeon ...  64C/128T 2skt 2node  L1d 48K L2 2M/2c L3 60M
   0[||||||               21.2%]   32[|||||||||||||||||||| 100.0%]  ...
 CACHE / TLB MISSES ────────────────────────────────────────────────────────────
 L1d miss% [  @ @. @ @@@@@@@                        ]     6.6% avg  232.2M/s
 L2  miss% [@@=@+@ @ @@@=+*@                        ]    37.9% avg  139.4M/s
 dTLB MPKI [  @ @. .  .. .                          ]     2.49 avg  120.1M/s
 TLB SHOOTDOWNS & IPIs ─────────────────────────────────────────────────────────
 recv/CPU  [  @ % % %                               ]           135k/s total
 send/CPU  [@                                       ]          34.0k/s total
 IPIs: resched 5.0/s  func-call 136k/s  flushed pages 34.3k/s   lost 0 (exact)
 reasons: switch 6.0k/s recv 133k/s local 0/s localmm 34.4k/s send 34.0k/s
 origins: wp_page_copy 100%  do_madvise 0%  ...
 WHO IS FLUSHING  (sort: SEND/s) ────────────────────────────────────────────────
     PID COMM               SEND/s   PAGES/s  LOCAL  LOCMM  SWTCH   RECV  ORIGIN
  293596 shootgen            33.7k     33.7k      0  33.7k    2.0   133k  wp_page_copy
```

## How it measures (accuracy model)

| Signal | Source | Accuracy |
|---|---|---|
| Shootdown IPIs **received** per CPU | `/proc/interrupts` `TLB` row | exact kernel counter |
| Rescheduling / function-call IPIs | `/proc/interrupts` `RES` / `CAL` rows | exact kernel counter |
| Shootdown **senders** (PID + kernel origin) | `tlb:tlb_flush` tracepoint, `sample_period=1` | every event captured; drops are counted and displayed (`lost`) |
| L1d / L1i / dTLB / iTLB misses | per-CPU generic PMU events | hardware counter (multiplex-scaled if over-subscribed, flagged) |
| L2 references / misses | vendor raw events — Intel `L2_RQSTS.REFERENCES`/`MISS` (0x24/0xff, 0x24/0x3f), AMD Zen `L2RequestG1.All` (0x60/0xff) / `L2CacheReqStat.IcDcMissInL2` (0x64/0x09) | hardware counter |
| Shootdown drivers (THP, compaction, migration, reclaim) | `/proc/vmstat` deltas | exact kernel counter |

The tracepoint capture is split into two channels with kernel-side filters:

* **attribution channel** — local flushes and remote-IPI sends
  (`reason ∈ {2,3,4,5}`) with kernel call chains. These identify the culprit
  process and the kernel path. Deep per-CPU rings + a dedicated 5 ms drain
  thread mean bursts of tens of thousands of events/s are captured losslessly;
  if a drop ever occurs it is reported in red (`LOST n attribution events`).
* **context channel** — task-switch and IPI-receive events (`reason ∈ {0,1}`),
  recorded without call chains. Losses here (only under extreme storms) are
  cosmetic because receive totals always come from the exact
  `/proc/interrupts` counters.

Cross-check built in: `send events × target-CPU fanout ≈ recv IPIs` from
`/proc/interrupts` — on test storms these agree within noise.

### Reading the table

* **SEND/s** — remote shootdown IPIs this task initiated. This is the column
  that answers "who is causing the shootdowns".
* **PAGES/s** — pages invalidated by those sends (a full-address-space flush
  reports 0 pages).
* **LOCAL / LOCMM** — local-only flushes; no IPIs to other CPUs.
* **SWTCH** — lazy-TLB flushes at context switch (benign).
* **RECV** — IPIs that interrupted this task. Victim, not cause.
* **ORIGIN** — dominant kernel function behind this task's flushes. Typical
  translations:
  * `unmap_region`, `vms_clear_ptes`, `zap_pte_range` → `munmap()` / process exit
  * `do_madvise`, `madvise_*` → allocator returning memory
    (`MADV_DONTNEED`/`MADV_FREE` — glibc trim, jemalloc/tcmalloc decay)
  * `wp_page_copy` → write-protect / CoW faults
  * `shrink_folio_list`, `try_to_unmap*` → memory reclaim (kswapd / cgroup limits)
  * `migrate_pages*`, `move_ptes` → page migration (compaction, NUMA, mbind)
  * `collapse_huge_page`, `khugepaged`, `split_huge_*` → THP
  * `change_protection*`, `do_mprotect_pkey` → `mprotect()` churn (JIT, GC)

## Build & run

```sh
make            # needs gcc, libncurses-dev
sudo ./tlbanalyser            # interactive TUI
sudo ./tlbanalyser -b -d 5    # batch mode for logging, 5 s intervals
sudo ./tlbanalyser -b -n 12 -d 5 > tlb.log   # one minute capture
```

Requires root (or `CAP_PERFMON` + `CAP_SYS_ADMIN`) for the PMU and
tracepoint. Without them, `/proc`-based panels still work.

Keys: `F1` help · `F5`/`s` sort (SEND, TOTAL, RECV, ΣSEND) · `c` cumulative ·
`p` pause · `m` compact CPU grid · `r` reset cumulative · arrows/PgUp/PgDn
scroll · `q` quit.

Options: `-d SEC` refresh interval (default 1.5) · `-b` batch · `-n N` batch
iterations · `-t N` batch top-N processes.

## Hunting residual shootdowns (NUMA balancing already off)

1. Run `sudo ./tlbanalyser`, sort by **SEND/s** (default). The top rows are
   your culprits; **ORIGIN** tells you the mechanism.
2. `do_madvise` from your app → allocator trimming. Tune
   `MALLOC_TRIM_THRESHOLD_`/`M_TRIM_THRESHOLD` (glibc),
   `dirty_decay_ms`/`muzzy_decay_ms` (jemalloc), or tcmalloc release rate.
3. `khugepaged`/`collapse_huge_page` or `split_huge_*` → THP. Consider
   `madvise`-only THP or defrag settings.
4. `shrink_folio_list` from `kswapd*` or your app → reclaim pressure; check
   cgroup memory limits and page cache churn.
5. `migrate_pages`/compaction origins with `compactstall` in the *drivers*
   row → proactive compaction (`vm.compaction_proactiveness`).
6. `do_mprotect_pkey`/`change_protection` → JIT/GC in-process; pin those
   processes to a CPU subset so IPIs don't reach latency-critical cores
   (shootdown IPIs only go to CPUs where the mm is active).
7. Watch the per-CPU `recv/CPU` strip to confirm your isolated cores stop
   receiving IPIs after each change.

## Notes

* Targeted at homogeneous server parts (Xeon, EPYC). On hybrid client CPUs it
  falls back to `cpu_core` for the raw L2 events (P-cores only) — fine for
  development, not the deployment target.
* Kernels ≥ 4.14 with tracepoint filter support get the two-channel capture;
  older kernels fall back to single-channel capture-everything.
* AMD Zen 3+ with INVLPGB (broadcast TLB invalidation, kernel ≥ 6.5 with
  `X86_FEATURE_INVLPGB`) performs some remote flushes without IPIs; the
  `TLB` interrupt row then undercounts while the tracepoint still records the
  flushes.
* Memory: per-CPU perf rings ≈ 0.6–2.3 MB per CPU depending on core count.
