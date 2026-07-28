# BK7258 CP patches

This directory tracks the minimal changes required in the external Beken
`bk_avdk_smp` SDK. The OpenVela AP sources remain under `board/beken/`.

## AP log bridge

`0001-cp-bridge-ap-mailbox-logs-to-uart0.patch` applies to the Beken SDK
baseline commit `aa5df96`. It opens CP `MB_UART0`, drains AP log data from a
worker thread, and writes it to physical UART0 without blocking the mailbox
receive callback.

Apply from the `bk_avdk_smp` repository root:

```bash
git apply --check \
  ../contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/patches/bk_avdk_smp/0001-cp-bridge-ap-mailbox-logs-to-uart0.patch

git apply \
  ../contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/patches/bk_avdk_smp/0001-cp-bridge-ap-mailbox-logs-to-uart0.patch
```

If `git apply --check` reports that the patch does not apply, first check
whether `cp/middleware/driver/common/driver.c` already contains
`ap_uart0_log_init`. Do not apply the patch twice.

Verify the applied source without changing it:

```bash
git diff --check
git diff -- cp/middleware/driver/common/driver.c
```

The currently validated local SDK carries the equivalent changes in commits
`6bfc6e7`, `85e0476`, and `d203f9f`.
