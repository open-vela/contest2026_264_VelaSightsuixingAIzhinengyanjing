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

From the linker's own report (`-Wl,--print-memory-usage`), config
`ai_agent`, commit-time measurement:

| Region | Used | Size | % |
|---|---|---|---|
| FLASH | 601920 B | 1088 KB (`primary_ap_app` 1156k, minus the 34/32 code overhead) | 54.0% |
| RAM | 96960 B | 336 KB (`AP_RAM` 0x54000) | 28.2% |
| PSRAM_SECTION | 0 B | 6 MB | 0% |

The `nsh` baseline is 230720 B of FLASH, so the agent plus mbedTLS plus the
network stack costs about 371 KB of flash and leaves roughly 475 KB spare.
RAM here is the static footprint only; heap use at runtime has not been
measured yet.

## Camera path

`camera_capture` (`src/tools/tool_camera.c`) is the only agent tool that
touches this board's camera. It opens `/dev/video0`, sets a compressed
format, uses two `V4L2_MEMORY_MMAP` buffers, waits with `poll()` and hands
the frame to a Vision LLM.

What already matches:

- **MMAP buffers land in PSRAM.** `v4l2_cap.c` calls
  `imgdata_ops_s.alloc()` when one exists, and this board's implementation
  takes the buffer from `PSRAM_POOL_DISPLAY` (`bk7258_camera_imgdata.c`).
  That is what makes a 160 KB × 2 buffer heap possible on a board whose AP
  kernel heap is smaller than that, and it is also required: YUV_BUF can
  only write into `0x60000000..0x61000000` (invariant 5 in
  `docs/reference/camera.md`).
- **`sizeimage` decides the buffer size.** For a compressed format
  `get_bufsize()` returns `sizeimage` rather than `width*height`, so the
  tool's 160 KB hint is what gets allocated per buffer.
- **The same MMAP + `poll` + `DQBUF` shape is already proven on this board**
  by `camera_preview`, which uses `V4L2_MEMORY_MMAP` too. Only the pixel
  format differs (UYVY there, JPEG here).

What did not match, and how it was fixed. Both were found by running the
sequence on hardware, and both are now measured rather than argued:

- **The driver rejected the tool's buffer.** `set_buf()` required the buffer
  to be at least `width*height*2`, i.e. a whole raw frame, for every format.
  For a compressed format that is the wrong number: the size is the
  application's choice, because `get_bufsize()` returns `sizeimage` when one
  was set and only computes `width*height` when it was not. The agent asks
  for 160 KB, `480*480` is 225 KB, so every buffer was refused:

  ```
  set_buf: rejected addr=0x601901a0 size=163840 (need >=230400 bytes ...)
  start_capture: frame_buf is NULL, -EINVAL
  agent_camera: no frame within 5000 ms
  ```

  Fixed in `bk7258_camera_imgdata.c`: for JPEG the capacity comes from
  `set_buf()`'s own `size` argument, floored to a whole number of DMA
  transfer steps (the drain channel loops in `transfer_len` steps, so a
  window that is not a multiple of one would let the last step write past
  its end) and required to be at least two steps. For UYVY nothing changed
  -- whole-frame direct capture still needs exactly `width*height*2`.

  This also removed a latent overrun: `frame_bytes` was both the accept
  threshold *and* the DMA wrap bound, so a buffer smaller than
  `width*height` would previously have been drained as if it were that
  large.

- **Geometry.** The tool asks for 320x180 ("low", its default) or 1280x720
  ("high"). This driver programs 480x480 / 640x480 / 864x480 and
  `bk7258_gc2145_find_mode()` matches exactly, so `VIDIOC_S_FMT` refuses
  both and the tool gives up with no way to discover what it should have
  asked for. Fixed by patch 0001: when the requested geometry is refused,
  enumerate `VIDIOC_ENUM_FRAMESIZES` and retry at the closest size the
  driver offers. The requested geometry is still tried first, so devices
  that support it are unaffected.

### Measured on hardware

Firmware built from this config, packaged per `docs/reference/camera.md`
10.2 with the sha256 of `nuttx.bin`, `openvela-ap.bin` and `app1.bin`
verified equal, flashed with `autoflash.sh -b 1500000`.

The V4L2 path works end to end. Auto geometry, one frame:

```
agent_camera: auto-selected 480x480
agent_camera: format JPEG 480x480 sizeimage=163840
agent_camera: buffer[0] len=163840 at 0x601901a0
set_buf: addr=0x601901a0 frame_bytes=163840 (buffer size given=163840)
agent_camera: frame 0: index=0 bytesused=43263 flags=0x00000000
agent_camera: frame 0: index=0 bytesused=42735 flags=0x00000000
agent_camera: structure SOF=yes DQT=yes DHT=yes SOS=NO EOI=yes
agent_camera: NOT a decodable JPEG -- no SOS segment ...
agent_camera: OK
```

33-frame session: 29.78 fps, no dropped frames, no timeouts.

> **The bytes were not a decodable JPEG until the header was rebuilt, and
> this README claimed otherwise for a while.** The encoder emits SOI / APP0 /
> SOF0 / DQT×2 / DHT×3 and then entropy data, and **the AC Huffman table it
> writes into the stream is not the table it encoded with** — its two DC
> tables are byte-identical to the JPEG Annex K standard tables, its AC table
> is not, and it emits neither the chroma AC table nor an SOS segment.
> libjpeg's verdict on the raw stream: `Invalid JPEG file structure: missing
> SOS marker`.
>
> Fixed in the driver: `bk7258_jpeg_enc_write_header()` rewrites the header
> in place with the four standard tables plus an SOS, keeping the hardware's
> SOF0 and DQT (both correct). Zero-copy — the DMA writes 256 bytes into the
> buffer and a COM segment absorbs the length difference, so the entropy data
> is never moved and it runs in the ISR. Design and evidence:
> `docs/reference/camera.md` 14.7-14.9.
>
> Verified from the board: `identify` reports
> `JPEG 480x480 8-bit sRGB` with no warnings, PIL decodes, and the render is
> a clear photograph. 30-frame session: 31.56 fps, 30/30 frames structurally
> complete, `hdr_fail=0 short=0`.
>
> So `camera_capture` now hands a Vision LLM a file standard decoders accept.
>
> `SOI` + `EOI` present was the judgement used here, and it is not
> sufficient — `agent_camera`'s marker check and the driver were both
> applying the same inadequate test. `ffmpeg` makes this worse by
> "succeeding": it emits a 480x480 PNG that is uniformly **RGB(0,135,0)**,
> which is just YUV(0,0,0) through BT.601, i.e. no scan data decoded at all.
>
> So `camera_capture` would today hand a Vision LLM something most decoders
> reject. The V4L2 plumbing is done; the bitstream is not.

The patch, before and after, on the tool's own default request:

| Command | Result |
|---|---|
| `agent_camera low` (negotiates, = patched tool) | `S_FMT JPEG 320x180 failed: 22` → `320x180 refused, using enumerated 480x480` → `bytesused=13651` → `OK` |
| `agent_camera low strict` (= unpatched tool) | `S_FMT JPEG 320x180 failed: 22`, `S_FMT ENTR 320x180 failed: 22`, `FAILED (-22)` |

So the unpatched `camera_capture` cannot work on this board at all, and the
patched one captures a valid JPEG.

Three frames in one session, first one saved: `bytesused` 13531 / 12535 /
39915, `wrote 13531 bytes to /mnt/cap.jpg`, `ls -l /mnt` reports `13531
cap.jpg`. So streaming, buffer rotation and the file write all work — but see
the retraction above for what those bytes actually contain.

The start-up transient, diagnosed: every session begins with exactly
`err=3 resets=3 short=1` and then runs clean — the counts do not grow with
session length (measured at 3, 13 and 33 frames). In JPEG mode the stream is
driven by enabling the encoder, and this board's sensor streams continuously
from init, so the encoder always starts at an arbitrary phase and the first
frame is partial; that is precisely what it reports as `frame_err` (the
reference names the handler `dvp_camera_sensor_ppi_err_handler`, i.e. "the
picture is not the size I was told"). **It never reaches the application** —
a frame without an EOI is completed with an error and not handed up, so the
first frame dequeued is always whole: 33-frame session, 31 frames delivered with
no error flag, zero timeouts, 29.78 fps. That
is what matters here, because `camera_capture` takes exactly one frame.

Waiting for a VSYNC boundary before enabling the encoder was tried and
reverted: the premise is circular (in JPEG mode VSYNC events only start once
the encoder is enabled), the wait timed out in all three sessions, and its
only effect was cost — 29.72 → 25.99 fps. See `docs/reference/camera.md`
14.4.

### `ai_agent` itself

Starts and runs on the board. All init phases return 0 and it reaches its
own prompt in 1.15 s:

```
[agent] [boot +1153ms] AI Agent ready. Type 'help' in NSH for commands.
vela> heap_info
Heap: arena=247104 fordblks(free)=47544 uordblks(used)=199560
```

**The heap is the tight resource, not flash.** The agent holds 199 KB of the
AP's 247 KB kernel heap at idle, leaving 46 KB. A TLS handshake needs tens
of KB on top of that, so the network work will have to account for this
before `ask` can be expected to work -- flash, at 54%, is not the
constraint.

Without a mounted `/mnt` the agent still starts; it only loses persistence
(`[skills] Cannot write skill: /mnt/ai_agent/skills/...`). It does not
create its data directory tree, so `mkfatfs /dev/ram0` + `mount -t vfat
/dev/ram0 /mnt` + `mkdir /mnt/ai_agent` has to happen first. With no network
it degrades cleanly rather than hanging: `[agent] Network timeout — net
services not started.`

### Using the JPEG path

Three routes, all exercised on hardware. Resolutions are 480x480 / 640x480 /
864x480 only — the sensor driver matches exactly and rejects anything else.

```sh
agent_camera                      # one frame, prints the segment structure
agent_camera b64                  # one frame as base64, to pull it off-board
nxcamera                          # or use the stock tool, fourcc "JPEG":
  input /dev/video0
  output /mnt/n.jpg 1
  stream 480 480 30 JPEG
  q
```

For your own application the sequence is the one in
`app/agent_camera/agent_camera_main.c`, which is also what
`packages/ai_agent`'s `tool_camera.c` does: `S_FMT`
(`V4L2_PIX_FMT_JPEG`, set `sizeimage`) → `REQBUFS` 2 MMAP → `QUERYBUF` +
`mmap` → `QBUF` → `STREAMON` → `poll` → `DQBUF`. `bytesused` is the length of
a complete JPEG file starting at byte 0 of the buffer: the driver has already
written a standard header in front of the entropy data, so nothing needs to be
added afterwards. Details, including why `sizeimage` matters, are in
`docs/reference/camera.md` 14.3.

### Reproducing any of this: `app/agent_camera`

`camera_capture` can only be reached through the LLM (the agent has no CLI
command that invokes a tool directly), so it cannot be triggered on a board
with no network. `app/agent_camera` exists to run the half that the board is
responsible for: the tool's ioctl sequence verbatim, then SOI/EOI and byte
count validation instead of an LLM post. A failure therefore separates into
"capture path" versus "geometry" rather than one opaque error.

```sh
agent_camera caps                 # what the driver enumerates
agent_camera                      # auto geometry, one frame, structure checked
agent_camera low                  # the tool's default request, negotiated
agent_camera low strict           # the same request without negotiation
agent_camera b64                  # print the frame as base64 to retrieve it
agent_camera n=5 out=/mnt/cap.jpg # five frames, first saved (needs a mount)
```

To get a frame onto the host and check it with a real decoder — which is the
only way to catch what the marker check missed — see
`docs/reference/camera.md` 14.5.

Its negotiation is the same algorithm as patch 0001, so `low` versus
`low strict` is a before/after of that patch against the real driver.

## Network

`ask` and the Vision LLM call cannot work yet: the AP core has no network
device. On this chip Wi-Fi belongs to CP (`CONFIG_WIFI_ENABLE=y` in
`bk_avdk_smp/projects/app_ab/cp/config/bk7258/config`, not set for the AP),
and the stock Armino AP reaches the network through a host driver that
tunnels to CP over the mailbox channels `MB_CHNL_WIFI_CMD` /
`MB_CHNL_WIFI_DATA` (`ap/components/bk_wifi_driver/wdrv_ipc.c`,
`CONFIG_WIFI_VNET_CONTROLLER=y` on both sides). A NuttX network device for
this board therefore means porting that host driver, which is out of scope
for the camera work and tracked separately.

The config still enables `CONFIG_NET` with loopback only, because the agent
needs `socket()`, `getifaddrs()` and mbedTLS to link at all.

## Upstream defects found while porting

To be sent as pull requests against `open-vela/packages_ai_agent`
(`dev-ai-contest-2026`). Until they are merged, `apply.sh` puts them in the
work tree; patches here are archived copies, not a fork.

| File | Defect | Status |
|---|---|---|
| `src/tools/tool_camera.c` | Hardcoded geometry with no negotiation; 6 `%u` conversions applied to `uint32_t` (wrong on this ABI, and fatal under `-Werror=format`) | patch 0001 |
| `src/core/agent_loop.c:665` | `-Wformat-truncation`: `%s` may write up to 511 bytes into a 473..483 byte region | not patched |
| `src/tools/skill_loader.c:448` | `%x` applied to `uint32_t` | not patched |

The two unpatched ones only block `-Werror`; they are reported here so the
next person does not rediscover them.
