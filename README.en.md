[日本語](README.md) | English

# KV260 YOLO26n Live Detection Web Streaming

A complete setup for the AMD Kria **KV260** Starter Kit and the AP1302 camera module (the IAS module bundled with the KV260):
camera video is run through **YOLO26n (640×640) on the PL (FPGA)**, detection boxes are overlaid, the result is encoded to
**H.264 by the VCU**, sent out over RTSP, and delivered to browsers (WebRTC / HLS) via MediaMTX. The weights are Ultralytics
YOLO26n quantized for this PL (`n.q`). The end state is live video with the detection overlay (about 9.5 fps) visible in a browser
at `http://<board>:8889/detect` (`doc/setup.en.md`).

```
camera (AP1302) → capture_daemon → camera_preprocess → PL inference (YOLO26n) → detection overlay
  → vcu_stream(VCU H.264)→ rtsp_detect_server.py(RTSP :8554)→ MediaMTX(WebRTC :8889 / HLS :8888)
                                                     web_mode_server.py (viewer page :8890)
```

## Layout

| dir | Role | Details |
|---|---|---|
| [`live/`](live/) | **Source** for the programs that run on the board (capture / preprocess / inference host / vcu_stream / rtsp / web / launcher), plus the aarch64 cross build (`build_live.sh`) | [live/README.md](live/README.md) |
| [`doc/`](doc/) | Procedure from preparing the boot SD to starting, stopping, and checking the stream | [doc/setup.en.md](doc/setup.en.md) |

**This repo contains only source and procedures.** The streaming bundle transferred to the KV260 (binaries + launcher + fan control +
deployment script `install.sh`) is not in the repo; it is distributed separately as a **distribution archive** (see "Distribution" below).

## Prerequisites

- Kria KV260 Starter Kit + AP1302 camera module (IAS0 = J7).
- AMD's official generic Kria Starter Kit embedded Linux 2025.2 (kernel `6.12.40-xilinx-g31626ef92ff1`), XRT 2.20.0.
  The bundled `allegro_dvt.ko` is built for this kernel only (vermagic must match).
- The PL (bit / xclbin), `n.q`, and the inference host must **match as a set** (no swapping individual pieces; see "版の整合"
  (version consistency) in the `README.md` bundled with the distribution archive).

## Quick start

The full procedure and caveats are in **[doc/setup.en.md](doc/setup.en.md)**. Outline only:

1. Prepare the boot SD (AMD official 2025.2 wic, Boot FW v1.06 or later) — §1
2. First login, ssh public key, passwordless sudo, `dnf install xrt gstreamer1.0 …` — §2
3. Download and extract the distribution archive `y7-public-<tag>.tar.gz`, fetch MediaMTX v1.19.2 `linux_arm64` from upstream,
   and place it at `home/mediamtx` in the extracted tree — §3-0
4. In the extracted tree, `md5sum -c MD5SUMS.txt` → deploy with `bash install.sh <user>@<board>` → reboot — §3-1
5. `bash ~/vcu_enc_setup.sh` (PL bring-up after reboot) → optionally run the canary check — §3-2, §3-3
6. Start streaming with `run_live_rtsp_stream.sh` and `browser_stream_mediamtx.sh`, then open `http://<board>:8889/detect` — §3-4
7. Always stop via the stop flag (killing the inference host mid-run corrupts the PL CU; the only recovery is a reboot) — §3-6

For rebuilding, see [live/README.md](live/README.md) (requires the sysroot of the PetaLinux 2025.2 common SDK).
Regenerating the PL (bit / xclbin) and `n.q` is out of scope for this repo.

## Distribution (the bundle deployed to the board)

The bundle deployed to the board is **not included in the repo**. It is packed into a single `.tar.gz` and distributed as a
GitHub Release asset
(published in Release v1.0: https://github.com/Leiden21g/kv260detector/releases/tag/v1.0 ; download and sha256 check in [doc/setup.en.md](doc/setup.en.md) §3-0).

- The file name is `y7-public-<tag>.tar.gz`. `<tag>` is the **first 8 hex digits of the xclbin md5** of the adopted build
  (the build covered by this document is xclbin `14279337` = `y7-public-14279337.tar.gz`).
- Contents: the deployed binaries (`firmware/` = bit / xclbin / `al5e*.fw` / dtbo, `home/` = launcher, `allegro_dvt.ko`,
  `vcu_stream`, inference host ELF, `n.q` / `x.bin`, `fan/` = fan control unit) + deployment script **`install.sh`** +
  bundled **`README.md`** (contents, install locations, version consistency) + **`MD5SUMS.txt`** + **`LICENSE`** + **`THIRD_PARTY_NOTICES.md`** +
  **`src/`** = the **Corresponding Source** for the bundled binaries (`src/allegro-dvt/` = GPL-2.0 Corresponding Source for `allegro_dvt.ko`,
  `src/dtbo/` = the `.dtso` for `combined_allegro.dtbo`; both identical to [src/](src/) in this repo).
- Usage: extract → fetch `home/mediamtx` from upstream and place it → `md5sum -c MD5SUMS.txt` →
  `bash install.sh <user>@<board>`. Details in [doc/setup.en.md](doc/setup.en.md) §3-0 to §3-1.
- The bundled `MD5SUMS.txt` is authoritative for versions. The PL (bit / xclbin), `n.q`, and inference host ELF must match as a set.
- **MediaMTX is neither bundled nor redistributed.** Get it from the upstream GitHub Release (`doc/setup.en.md` §3-0).
- `capture_daemon` / `camera_preprocess` / `vcu_stream` rebuilt from `live/` in this repo are byte-identical to the deployed
  binaries in the asset (`live/MD5SUMS.expect.txt`). Regenerating the PL (bit / xclbin) and `n.q` is out of scope for this repo.
- The **Corresponding Source** for `allegro_dvt.ko` (GPL-2.0) and `combined_allegro.dtbo` is in [src/](src/).
  `combined_allegro.dtbo` can be regenerated byte-exact with `dtc` alone (`src/dtbo/`). The upstream source files for
  `allegro_dvt.ko` are distributed as a separate asset of the same Release as the ko, `allegro-dvt-gpl-src-31626ef92ff1.tar.gz`
  (in a form that builds offline; `src/allegro-dvt/README.md`).

## License

- The original parts of this repo are licensed under the **GNU Affero General Public License v3.0 (AGPL-3.0)** — [LICENSE](LICENSE).
- The weights (`n.q` and related files) are a quantized derivative of **Ultralytics YOLO26n** (AGPL-3.0). The YOLO26 model and
  trained weights are by Ultralytics (https://github.com/ultralytics/ultralytics). This repo is not an official Ultralytics product
  and is not affiliated with Ultralytics or AMD. Commercial use that cannot comply with AGPL-3.0 may require an Ultralytics Enterprise License
  (https://www.ultralytics.com/license).
- Third-party components (`allegro_dvt.ko` = GPL-2.0, VCU firmware `al5e*.fw` = Allegro DVT2 license, MediaMTX = MIT, XRT = Apache-2.0,
  AMD IP contained in the bit/xclbin, etc.) are subject to their respective licenses. For the list, sources, obligations, and open items, see
  **[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)**.

## Contact

- For inquiries about this repo: **leiden21g@gmail.com**
- Bug reports and questions are also welcome on GitHub [Issues](https://github.com/Leiden21g/kv260detector/issues).
