# ai_agent on bk7258-ap

What this board needs in order to run `packages/ai_agent`, and which parts of
that belong here rather than upstream.

Build config: `../configs/ai_agent/defconfig` (the `nsh` baseline plus the
network stack, mbedTLS, cJSON and the agent itself).

```sh
cd <openvela work tree root>
sh contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/boards/bk7258/bk7258-ap/ai_agent/apply.sh
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/ai_agent --cmake -j8
```

`-Werror` cannot be used with this config: `packages/ai_agent` itself does not
build warning-free (see "Upstream defects" below). The `nsh` config keeps
`-Werror`.

## Measured footprint

From the linker's own report (`-Wl,--print-memory-usage`):

| Region | Used | Size | % |
|---|---|---|---|
| FLASH | ~604 KB | 1088 KB (`primary_ap_app` 1156k, minus the 34/32 code overhead) | 54% |
| RAM | ~97 KB | 336 KB (`AP_RAM` 0x54000) | 29% |

The `nsh` baseline is ~243 KB of flash, so the agent plus mbedTLS plus the
network stack costs roughly 360 KB and leaves about 480 KB spare.

**The heap is the tight resource, not flash.** The agent holds 199 KB of the
AP's 247 KB kernel heap at idle, leaving 46 KB:

```
[agent] [boot +1153ms] AI Agent ready. Type 'help' in NSH for commands.
vela> heap_info
Heap: arena=247104 fordblks(free)=47544 uordblks(used)=199560
```

A TLS handshake needs tens of KB on top of that, so the network work will have
to account for this before `ask` can be expected to work.

Without a mounted `/mnt` the agent still starts; it only loses persistence
(`[skills] Cannot write skill: /mnt/ai_agent/skills/...`). It does not create
its data directory tree, so `mkfatfs /dev/ram0` + `mount -t vfat /dev/ram0
/mnt` + `mkdir /mnt/ai_agent` has to happen first. With no network it degrades
cleanly rather than hanging: `[agent] Network timeout — net services not
started.`

## Camera: the agent needs a JPEG, this board encodes in two steps

`camera_capture` (`src/tools/tool_camera.c`) is the only agent tool that
touches the camera, and it assumes a **capture** device that hands over
compressed frames directly: open one node, `VIDIOC_S_FMT` with
`V4L2_PIX_FMT_JPEG`, then `DQBUF` a JPEG.

That is not how this board encodes. JPEG comes from the **M2M** encoder on
`/dev/video1` (`boards/bk7258-ap/src/bk7258_jpeg_enc.c`): raw frames go into
its OUTPUT queue, JPEG comes out of its CAPTURE queue. `/dev/video0` is the
GC2145 capture path and offers `V4L2_PIX_FMT_UYVY` only. So getting a JPEG
here is two steps:

1. `/dev/video0` — capture a UYVY frame (480x480, 640x480 or 864x480)
2. `/dev/video1` — queue it to OUTPUT, dequeue JPEG from CAPTURE

`app/jpeg_test` already does exactly this end to end and is the reference for
the sequence, including the byte-order handling.

**So `camera_capture` cannot work on this board as written**, and the fix is
not a parameter: it needs the two-step flow. That is a larger upstream change
than the geometry patch below and has not been made yet. Until it is, the
agent's camera tool is the one piece of the port that is not functional; every
other part of the agent runs.

Also relevant when that patch is written: the tool asks for 320x180 or
1280x720, and this board's sensor programs 480x480 / 640x480 / 864x480 and
matches exactly, so the geometry has to be negotiated either way.

## Network

`ask` and the Vision LLM call cannot work yet: the AP core has no network
device. On this chip Wi-Fi belongs to CP (`CONFIG_WIFI_ENABLE=y` in
`bk_avdk_smp/projects/app_ab/cp/config/bk7258/config`, not set for the AP),
and the stock Armino AP reaches the network through a host driver that tunnels
to CP over the mailbox channels `MB_CHNL_WIFI_CMD` / `MB_CHNL_WIFI_DATA`
(`ap/components/bk_wifi_driver/wdrv_ipc.c`, `CONFIG_WIFI_VNET_CONTROLLER=y` on
both sides). A NuttX network device for this board therefore means porting
that host driver, which is out of scope for the camera work and tracked
separately.

The config still enables `CONFIG_NET` with loopback only, because the agent
needs `socket()`, `getifaddrs()` and mbedTLS to link at all.

## Upstream defects found while porting

To be sent as pull requests against `open-vela/packages_ai_agent`
(`dev-ai-contest-2026`). Until they are merged, `apply.sh` puts them in the
work tree; patches here are archived copies, not a fork.

| File | Defect | Status |
|---|---|---|
| `src/tools/tool_camera.c` | Hardcoded geometry with no negotiation; 6 `%u` conversions applied to `uint32_t` (wrong on this ABI, and fatal under `-Werror=format`) | patch 0001 |
| `src/tools/tool_camera.c` | Assumes a capture device that emits JPEG; needs the two-step M2M flow described above | not patched |
| `src/core/agent_loop.c:665` | `-Wformat-truncation`: `%s` may write up to 511 bytes into a 473..483 byte region | not patched |
| `src/tools/skill_loader.c:448` | `%x` applied to `uint32_t` | not patched |

The unpatched format ones only block `-Werror`; they are reported here so the
next person does not rediscover them.
