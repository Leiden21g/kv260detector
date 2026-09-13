[日本語](setup.md) | English

# KV260 Setup Guide — From Preparing the Boot SD to Starting the Web Stream

Target: Kria KV260 Starter Kit + AP1302 camera module (the IAS module bundled with the KV260).
End state: live video with the detection overlay (WebRTC, about 9.7 fps) visible in a browser at `http://<board IP>:8889/detect`.

This document has three parts.

1. §1 Preparing the boot SD (AMD's official generic Kria Starter Kit embedded Linux 2025.2)
2. §2 First boot and ssh setup from the PC (expanding the rootfs, ssh public key, passwordless sudo, additional packages, camera firmware)
3. §3 Deploying the distribution archive (install.sh) and starting, stopping, and checking the web stream

The PC side is assumed to be WSL2 (Ubuntu). `<board>` is the board's IP (`192.168.0.35` in this document's examples) and `<user>` is `amd-edf` (the login user of the official image).

**Verification markers**: things confirmed on real hardware or on the actual official image are marked "confirmed"; steps that are written
but have not been run are explicitly marked "**unverified**". Unmarked steps were confirmed under the conditions stated at the start of §1.

---

## 1. Preparing the boot SD

Use AMD's officially distributed **generic Kria Starter Kit embedded Linux 2025.2** (EDF 25.11, common to KV260/KR260/KD240).

Scope of verification: on **2026-09-12, §1 through §3 were run end-to-end on real hardware, starting from a pristine official SD**
(freshly written with the wic above). The end state reached was **a running web stream** — canary GOLD 6/6 match (**140.00 ms/img**),
live run1 at 2000 images / 206 s = **9.71 fps**, all 8 units `active`, HTTP 200 on `:8889` and `:8890`, and vcu_stream sent 1600 frames.

Gaps found during that run have been folded into this document (rootfs expansion = §2 step 3, `v4l-utils` = §2-1,
camera firmware = §2-2, removing the old host key = §2 step 5, how to apply NOPASSWD = §2 step 6).
The camera firmware `ap1302_ar1335_single_fw.bin` is in neither the official SD nor the dnf feed, so users must fetch it from upstream themselves
(its source and license are settled; procedure = §2-2). Apart from that, everything works from a pristine SD by following this document.

### 1-1. Requirements

| Item | Source | Notes |
|---|---|---|
| microSD, 16GB or larger | — | |
| `kria-image-full-cmdline-kria-zynqmp-generic.rootfs-20251116094523.wic.xz` | AMD Adaptive Computing Wiki "Kria SOMs & Starter Kits" (AMD account required) | The boot image itself. Login is `amd-edf` |
| `BOOT-k26-smk-sdt-1.06-20251115173103.bin` | AMD account required. See Wiki "Kria SOM Boot Firmware Update" https://xilinx-wiki.atlassian.net/wiki/spaces/A/pages/3020685316/Kria+SOM+Boot+Firmware+Update | K26 Boot FW v1.06. Officially v1.06 or later is recommended, but **updating is not required** (§1-2) |
| MediaMTX v1.19.2 linux_arm64 | GitHub Release (§3-0) | Third-party binary, not bundled in the distribution archive |

md5 of the files used to verify this document (to check that downloads are not corrupted):

```
2b62feb42d4f3a63d12ad3e4c4ba4750  kria-image-full-cmdline-kria-zynqmp-generic.rootfs-20251116094523.wic.xz
3f0796d99b61e11c802749b4d50cf71d  BOOT-k26-smk-sdt-1.06-20251115173103.bin
```

The kernel of this image is `6.12.40-xilinx-g31626ef92ff1`. XRT is **not included in the image** (confirmed: the wic rootfs has no
XRT / zocl), so install it from the EDF feed in §2-1 (`xrt` 2.20.0; `zocl` and
`kernel-module-zocl-6.12.40-xilinx-g31626ef92ff1` come in as dependencies). The `allegro_dvt.ko` in the distribution archive is built for this kernel
only (vermagic must match), and the inference host is linked against the sysroot of this XRT version.

### 1-2. Boot FW (QSPI) — update recommended but not required

The KV260 Boot FW lives in the SOM's QSPI. **Officially v1.06 or later is recommended** (Recommended FW for the 2025.2 wic = 01.06; confirmed: official doc).
v1.07 (for 2026.1) is unverified.

★**It boots even with firmware older than 1.06** (confirmed on hardware on 2026-09-12). The unit used for the end-to-end run reported in `bootfw_status`
`Last Booted Image: Image B` = **`K26-BootFW-01.01`** (`ImageA Revision Info: XilinxSOM_BootFW_20220915`,
QSPI image `XilinxSom_QspiImage_v1.1_20210422`), and with that firmware the official EDF 25.11 SD booted without issue, and
PL bring-up, canary, and web streaming **all worked**. The FW update is therefore **recommended but not required**
(within the scope of this document, everything works without updating). The steps below are for when the board does not boot, or you want to match the official recommendation.

The DTB and bootargs are supplied by the Boot FW in QSPI (confirmed: the boot partition p1 of the official wic has no `system.dtb`).

★The update procedure in this section itself has **not been performed (unverified)**. It is a summary of the official page's procedure for this document.

**(a) If the SD boots — update with `xmutil`** (from Linux on the board, after the ssh setup in §2):

```bash
scp BOOT-k26-smk-sdt-1.06-20251115173103.bin <user>@<board>:~/
ssh -t <user>@<board>
# ↓ run on the board
sudo xmutil bootfw_status                       # check current A/B bank and each Revision
sudo xmutil bootfw_update -i ~/BOOT-k26-smk-sdt-1.06-20251115173103.bin
sudo reboot
```
```bash
# ★must be run during the boot right after the reboot (otherwise the next boot falls back to the original bank)
ssh -t <user>@<board> 'sudo xmutil bootfw_update -v; sudo xmutil bootfw_status'
```
Pass criteria: `bootfw_status` shows **Last Booted Image** as the new bank, and that bank's Revision Info is `1-20251115173103`
(the actual output format has not been seen; field names are from the official page).

**(b) If the SD does not boot — Boot Image Recovery Tool** (rewrites QSPI on the board alone; unverified):

1. **Connect the board's LAN port (J10) directly to the PC with a LAN cable** (if left on a routed LAN,
   the Recovery Tool's fixed address 192.168.0.111 may collide with an existing device).
2. Set the PC's wired IP to a fixed `192.168.0.x` (anything but `.111`, e.g. `192.168.0.10/24`).
3. **Hold SW1 (FWUEN)** while applying 12V.
4. Open `http://192.168.0.111` in a browser on the PC and write `BOOT-k26-smk-sdt-1.06-20251115173103.bin`.

Source: https://xilinx.github.io/kria-apps-docs/bootfw/build/html/docs/bootfw_image_recovery.html

### 1-3. Writing the SD card

The `.wic.xz` is distributed xz-compressed.

```bash
xz -dc kria-image-full-cmdline-kria-zynqmp-generic.rootfs-20251116094523.wic.xz \
  | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress      # /dev/sdX is the SD device (★double-check; a mistake destroys a PC disk)
```

WSL2 often cannot access the SD card directly. In that case, use balenaEtcher on the Windows side (it accepts `.wic.xz` as is), or
write the `.wic` extracted with `xz -dk` using Rufus / Win32DiskImager.

---

## 2. First boot and ssh setup

1. **Wire up and power on** (source: UG1089 Interfaces https://docs.amd.com/r/en-US/ug1089-kv260-starter-kit/Interfaces):
   - The KV260 has **no boot mode switch**. It is fixed to QSPI32 boot from the factory, and the Boot FW in QSPI (§1-2) boots
     Linux from the microSD.
   - Insert the microSD into **J11**, connect the camera (AP1302 module) to **IAS0 = J7**, LAN to **J10**, and USB (UART/JTAG) to **J4**,
     and **apply 12V to J12 last**.
   - On a Rev2 carrier, make sure **J20 has no jumper** (with a jumper fitted it boots via JTAG).
   - **SW1 (FWUEN) is only for the Recovery Tool (§1-2 (b))**. Do not press it for a normal boot.
2. **Do the first login over serial (UART) or a monitor (DP)**.
   ⚠ `amd-edf` **has an empty initial password and is forced to change it at first login**. In this state **ssh login is not possible**
   (confirmed: shadow of the official wic). Set the password here before using ssh from step 5 onward.
   - **UART**: connecting J4's USB to the PC exposes 4 FTDI ports. Open the **second** one (usually `/dev/ttyUSB1` on native Linux,
     the second COM number on Windows) at **115200 bps**. The board-side console is `ttyPS1`.
     USB serial is not visible from WSL2, so use Tera Term / PuTTY on the Windows side.
   - Login: user `amd-edf`, press Enter with the empty password → enter a new password twice.
   ⚠ **`sudo` asks for a password by default** (confirmed on hardware). Step 6 makes it NOPASSWD.
3. ★**Expand the rootfs to the whole SD (manual step required)**. ★Do this **before** the dnf install in §2-1 (about 200MB).

   ⚠ **Right after writing, the rootfs does not span the whole SD** (confirmed on hardware on 2026-09-12).
   `systemd-growfs` does run at first boot, but it **only grows up to the partition size**, so
   the journal shows `systemd-growfs[498]: Successfully resized "/" to 999.2M bytes.` (= working as designed).
   Measured: on a 125GB SD, `/dev/mmcblk1p2` was **999.2MB** and `df -h /` showed **950M / 171MB free**.
   The additional packages in §2-1 will not fit as is.

   First check the current state:
   ```bash
   lsblk /dev/mmcblk1      # size of p2; if it is far smaller than the SD capacity, it needs expanding
   df -h /
   ```

   A pristine SD has neither `growpart` nor `resize2fs` (it does have `parted` / `sfdisk` / `partx` / `fdisk`).
   The procedure run on hardware:
   ```bash
   sudo sfdisk -d /dev/mmcblk1 > ~/ptable_before.bak      # ★back up the partition table first (for recovery)
   sudo dnf install -y e2fsprogs-resize2fs                 # a pristine SD has no resize2fs (this alone fits in 999MB)
   echo ",+" | sudo sfdisk --no-reread -N 2 /dev/mmcblk1   # grow p2 to the end of the SD (start sector is preserved)
   sudo partx -u /dev/mmcblk1                              # tell the kernel (no reboot needed)
   sudo resize2fs /dev/mmcblk1p2                           # online resize while mounted
   df -h /
   ```
   Expected (measured on a 125GB SD): p2 is **116G**, `/` is **113G / about 1% used = 108G free**.

   - ⚠ **Do not change the start sector**. The combination of `-N 2` and `,+` (size only, to the end) keeps the start position.
     Moving the start position loses the existing filesystem.
   - `sfdisk` printing `Re-reading the partition table failed: Device or resource busy` is **expected**
     (the device holds the root filesystem, so it cannot be re-read). The following `partx -u` informs the kernel, so
     proceed despite this warning (confirmed on hardware).
   - If something fails, `sudo sfdisk /dev/mmcblk1 < ~/ptable_before.bak` restores the original table.
4. Check the IP (`ip -4 addr`). DHCP by default. For a static IP, put a `.network` file in `/etc/systemd/network/`.
   ★Give the file a name that **sorts before the default `80-wired.network`** (e.g. `10-static.network`). systemd-networkd
   uses only the first file in name order that matches, so a name sorting later lets the default DHCP config win.
   Example (**unverified**; check the interface name with `ip -4 addr` and adjust. Doing this over ssh drops the connection, so use UART):
   ```ini
   # /etc/systemd/network/10-static.network
   [Match]
   Name=eth0

   [Network]
   Address=192.168.0.35/24
   Gateway=192.168.0.1
   DNS=192.168.0.1
   ```
   Apply with `sudo systemctl restart systemd-networkd` (or reboot).
5. Register an ssh public key from the PC (all subsequent steps assume **ssh without a password prompt**).

   ⚠ **After rewriting / swapping the SD, the board's host key has changed**. If the PC still has the old host key,
   ssh stops with `REMOTE HOST IDENTIFICATION HAS CHANGED` (confirmed on hardware on 2026-09-12). Remove it first
   (`<board の IP>` = the board's IP):
   ```bash
   ssh-keygen -R <board の IP>      # run on the PC; removes the old host key from known_hosts
   ```

   **5-a. Create a key on the PC** (not needed if you already have `~/.ssh/id_ed25519.pub` or similar; `<PC 名など>` = e.g. the PC name):
   ```bash
   ls ~/.ssh/id_*.pub 2>/dev/null || ssh-keygen -t ed25519 -C "<PC 名など>"
   #   press Enter to keep the default location (~/.ssh/id_ed25519); passphrase is optional (use ssh-agent if you set one)
   ```

   **5-b. Register it on the board** (you are asked for the board `<user>` password **once** here = the one set in step 2):
   ```bash
   ssh-copy-id -i ~/.ssh/id_ed25519.pub <user>@<board>
   ```
   Without `ssh-copy-id`, do the equivalent by hand:
   ```bash
   cat ~/.ssh/id_ed25519.pub | ssh <user>@<board> \
     'mkdir -p ~/.ssh && chmod 700 ~/.ssh && cat >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys'
   ```

   **5-c. Confirm passwordless login** (the echoed string `鍵認証 OK` means "key auth OK"):
   ```bash
   ssh -o BatchMode=yes <user>@<board> true && echo "鍵認証 OK"
   #   BatchMode=yes fails instead of prompting for a password; if this fails, the key is not in effect
   ssh <user>@<board> 'uname -r; grep ^VERSION= /etc/os-release'
   # expected: 6.12.40-xilinx-g31626ef92ff1
   #       VERSION="25.11+development-S11151020 (scarthgap)"      (confirmed: /etc/os-release of the official wic)
   ```
   XRT is not present yet at this point (it is installed and checked in §2-1).
   If key authentication does not work, check permissions on the board (`~/.ssh` = 700, `~/.ssh/authorized_keys` = 600,
   and the home directory must not be writable by others; if any of these is too loose, sshd ignores the key).

   **5-d. (Optional) Add an alias on the PC** — in `~/.ssh/config`:
   ```
   Host kv260
       HostName <board の IP>
       User <user>
       IdentityFile ~/.ssh/id_ed25519
   ```
   After that, `ssh kv260` logs you in. The command examples in this document keep the `<user>@<board>` notation, so either substitute
   or write `kv260` in place of `<user>@<board>`.

   ⚠ The host key changes every time you rewrite the board's SD. Each time, run `ssh-keygen -R <board の IP>` from the start of this step
   and then redo 5-b.

6. **Configure passwordless sudo (NOPASSWD)**. The steps in this document call sudo **non-interactively** in the form
   `ssh <user>@<board> 'sudo …'` (`install.sh`, and bring-up, start, and stop from §3-2 on). If a password is requested,
   they stall there (`install.sh` checks this up front and, if not configured, points you to this step and stops).

   **6-a. Check the current state** (`NOPASSWD 済` = "already NOPASSWD", `要設定` = "needs setup"):
   ```bash
   ssh <user>@<board> 'sudo -n true && echo "NOPASSWD 済" || echo "要設定"'
   #   -n = fail instead of prompting for a password; if it prints "要設定" (needs setup), go to 6-b
   ```
   The initial state is **needs setup** (`/etc/sudoers.d/99-amd-edf` contains `amd-edf ALL=(ALL) ALL` = password required; confirmed).

   **6-b. Configure it** (**confirmed on hardware with a pristine SD** on 2026-09-12. Result =
   `/etc/sudoers.d/zz-amd-edf-nopasswd` is `-r--r----- root root`, `visudo -c` reports `parsed OK` for all 3 files,
   and `sudo -n true` succeeds)

   ★**Run the block below one line at a time** (do not paste it all at once). `sudo visudo -cf "$t"`
   asks for a password, so if pasted in one go, **the following lines are swallowed by the password prompt and lost**
   (confirmed on hardware — `NOPASSWD OK` never appears and the `rm` and check commands get eaten). One line at a time works as described.

   ★**Guard against lockout first**: on the official wic, root's password is `*` (locked), so `su` cannot become root, and there is no `pkexec`
   (confirmed). If sudoers breaks there is no other way to get root, so **keep a root shell open in a separate terminal**:
   ```bash
   # separate terminal (keep it open until you are done)
   ssh -t <user>@<board>
   sudo -i          # enter the password → prompt becomes # (root shell)
   ```
   In the working terminal, log in to the board interactively (sudo prompts for a password, so use `-t`):
   ```bash
   ssh -t <user>@<board>
   # ↓ run on the board ($USER must be expanded on the board; if written directly in ssh arguments on the PC, it becomes the PC's $USER)
   t=$(mktemp)
   echo "$USER ALL=(ALL) NOPASSWD: ALL" > "$t"
   sudo visudo -cf "$t"                          # ★syntax check; confirm "parsed OK" before continuing
   sudo install -m 0440 -o root -g root "$t" /etc/sudoers.d/zz-$USER-nopasswd
   rm -f "$t"
   sudo visudo -c                                # syntax check of everything (all files parsed OK)
   exit
   ```
   - ★**Use a file name that sorts after `99-…`** (the `zz-…` above). In sudoers **the rule read last wins**, so
     a name read before the existing `99-amd-edf` (password required) gets its NOPASSWD overridden and has no effect.
   - Files in `/etc/sudoers.d/` whose names contain `.` or end with `~` are **ignored** (sudo behavior).
   - Do not edit `/etc/sudoers` itself directly. If you break it, sudo becomes completely unusable.
     Note that rules appended to the main file **after** its `@includedir /etc/sudoers.d` line are read after sudoers.d, so they win
     (the board used to develop this document is in that state = NOPASSWD via an addition to the main file; not recommended here).
   - **Recovery if it fails**: run `rm /etc/sudoers.d/zz-*` in the root shell you kept open. Without a root shell,
     insert the SD into a Linux PC, mount p2 (ext4, `LABEL=root`), and delete `etc/sudoers.d/zz-*`.
     Either way `99-amd-edf` remains, so **you simply return to the original password-prompting state**.

   **6-c. Confirm it works** (from the PC):
   ```bash
   ssh <user>@<board> 'sudo -k; sudo -n true && echo "NOPASSWD OK"'
   #   sudo -k = discard cached credentials (otherwise the previous password entry makes it pass and the check is meaningless)
   ```
   Once confirmed, you may close the root shell you kept open.

   **6-d. (Recommended, optional, unverified) Disable ssh password authentication**

   ⚠ With NOPASSWD, **logging in as `<user>` = root privileges**. sshd has **password authentication enabled** by default, so
   disabling it is recommended once key authentication is confirmed.
   Prerequisites: **key login success confirmed in 5-c** and **a root shell (`sudo -i` from 6-b) kept open in a separate terminal**.

   On official EDF, sshd runs as `sshd.socket` + `sshd@.service` (**socket activation, one instance per connection**), and there is no `sshd.service`
   (confirmed). So `systemctl restart sshd` is unnecessary (it does not exist), and settings take effect **from the next new connection**.
   `sshd_config` starts with `Include /etc/ssh/sshd_config.d/*.conf`, but the directory itself does not exist (confirmed), so create it
   (`構文 OK` = "syntax OK"):
   ```bash
   # run on the board
   sudo mkdir -p /etc/ssh/sshd_config.d
   printf 'PasswordAuthentication no\nKbdInteractiveAuthentication no\n' | sudo tee /etc/ssh/sshd_config.d/10-no-password.conf >/dev/null
   sudo sshd -t && echo "構文 OK"
   sudo sshd -T | grep -iE '^(passwordauthentication|kbdinteractiveauthentication|pubkeyauthentication)'
   #   expected: passwordauthentication no / kbdinteractiveauthentication no / pubkeyauthentication yes
   ```
   ```bash
   # from the PC (takes effect from new connections; no restart needed)
   ssh -o BatchMode=yes <user>@<board> true && echo "鍵 OK"
   ssh -o PubkeyAuthentication=no -o PreferredAuthentications=password <user>@<board> true   # OK if Permission denied (publickey)
   ```
   If you can no longer log in with the key, run `rm /etc/ssh/sshd_config.d/10-no-password.conf` in the root shell you kept open to revert
   (login via UART / DP is not affected by this setting).

### 2-1. Additional packages and caveats

Do this after completing §2 step 5 (ssh key) and step 6 (NOPASSWD).

- The packages needed for streaming are not installed, so install them with dnf before §3-1 (the AMD EDF repo is preconfigured; about 200MB).
  ★**Always include `xrt`** (confirmed: the wic has no XRT / zocl. The EDF feed has `xrt-202520.2.20.0`, which pulls in `zocl` →
  `kernel-module-zocl-6.12.40-xilinx-g31626ef92ff1` as dependencies):
  ```bash
  ssh <user>@<board> 'sudo dnf install -y xrt gstreamer1.0 gstreamer1.0-plugins-base-meta gstreamer1.0-plugins-good-meta \
    gstreamer1.0-plugins-bad-meta gstreamer1.0-rtsp-server gstreamer1.0-python python3-pygobject \
    libopencv-core409 libopencv-imgproc409 devmem2 curl v4l-utils kernel-module-vcu vcu-firmware'
  ```
  (`kernel-module-vcu`/`vcu-firmware` are AMD's allegro.ko and fw. The patched `allegro_dvt.ko` and v2019.2 fw bundled in the distribution archive take precedence, so they are not required)
  (**installing this list in one go on a pristine SD was confirmed on 2026-09-12**)
- ★**Always include `v4l-utils`** (if it is missing, **the symptom is misleading**). Without `v4l2-ctl`, the launcher's
  (`run_live_rtsp_stream.sh`) `v4l2-ctl … 2>/dev/null` **fails silently**, the format of `/dev/video0` stays
  unset, you get `REQBUFS: Invalid argument` (a `vb2_core_reqbufs` WARNING in `sudo dmesg`), and
  it finally **surfaces as a completely different symptom**: `[live] ✗ capd が frame を出さない` ("capd produces no frames") (confirmed on hardware).
  `sudo dnf install -y v4l-utils` fixes it. After installing, check:
  ```bash
  ssh <user>@<board> 'command -v v4l2-ctl'      # expected: /usr/bin/v4l2-ctl
  ```
- **`zocl` comes in as a dependency of `xrt`. No need to name it explicitly, and no need for `dnf clean all`**
  (confirmed on a pristine SD on 2026-09-12: `zocl-202520.2.20.0` +
  `kernel-module-zocl-6.12.40-xilinx-g31626ef92ff1` were installed together with `xrt-202520.2.20.0`, and `/lib/modules/…/updates/zocl.ko` and
  `/usr/lib/libxrt_coreutil.so.2.20.0` were present).
- **Checking XRT** (after installing):
  ```bash
  ssh <user>@<board> 'ls /usr/lib/libxrt_coreutil.so.2.*'
  # expected: /usr/lib/libxrt_coreutil.so.2.20.0 (same version the inference host links against)
  ```
  `install.sh` checks this too and stops with a reason if it is missing.
- ⛔ **Do not run `dnf upgrade` / `dnf update`**. If the kernel is upgraded, the vermagic of `allegro_dvt.ko` in the distribution archive no longer matches and
  the VCU (= streaming) stops working. When installing individual packages, use only `dnf install <名前>` (`<名前>` = package name).
- The PL is loaded **via dfx-mgr (`xmutil loadapp yolov7vcu`)** (§3-2). dfx-mgr / `xmutil` are always present on the official wic.
  Writing the overlay directly to configfs has hit a kernel Oops with the 7.8MB bit (direct configfs apply is the path for old SDs).
- The `ap1302` media entity name is `ap1302.3-003c`. The launcher finds the entity by detection, so name differences do not matter.
- The fan: pwm-fan is already enabled in the base DTB. Enabling `y7-fan.service` replaces `fancontrol.service` (Conflicts).

★ The kernel module in the distribution archive (`allegro_dvt.ko`) is **for kernel 6.12.40-xilinx-g31626ef92ff1 only** (vermagic must match).
XRT is 2.20.0 (same version as the sysroot the host ELF links against). If `uname -r` differs from this, the binaries in the distribution archive cannot be used.

### 2-2. Camera firmware `ap1302_ar1335_single_fw.bin`

This file is **included in neither** the official SD image nor the AMD EDF 25.11 dnf feed.
Its source is settled: **[Xilinx/ap1302-firmware](https://github.com/Xilinx/ap1302-firmware), published by AMD (Xilinx) on GitHub**
(copyright holder = ON Semiconductor). It is **not bundled** in this release, so users fetch it from upstream themselves.

★ **Needed only when using the IAS camera on J7**. Configurations without the camera (still images / dump input only) do not need it.

**Why it is needed** (confirmed on hardware): without this file in `/lib/firmware/`, the AP1302 driver fails to probe.

```
ap1302 3-003c: Direct firmware load for ap1302_ar1335_single_fw.bin failed with error -2
ap1302 3-003c: probe with driver ap1302 failed with error -2
```

As a result **`/dev/media0` does not appear**, and `vcu_enc_setup.sh` in §3-2 prints `camera=無し` (camera = none) (= streaming does not come up).

**Specifications**:

| Item | Value |
|---|---|
| File name | `ap1302_ar1335_single_fw.bin` |
| Location | `/lib/firmware/ap1302_ar1335_single_fw.bin` |
| Permissions / owner | `0644` / `root:root` |
| Size | 79,324 bytes |
| md5 | `23adc4be340a6bbcc4a7ba562c7b2889` |
| sha256 | `2dd09e34c68eb2e9ff2b488c9b7fb6d77f4673bff9c1af167d9d466e795ec1c2` |
| Upstream | `main` branch of https://github.com/Xilinx/ap1302-firmware (last commit touching the file `3482046f28bfedbb59e3f7321bef4f0b148d0765`, 2024-01-25). **Byte-identical** to the md5 above (confirmed) |
| AMD EDF 25.11 dnf feed | **Not present** (`dnf provides '*/ap1302_ar1335_single_fw.bin'` gives No matches. The driver itself, `kernel-module-ap1302`, is installed) |

★ **Watch the version**: the `xlnx_rel_v2022.1` branch version in the same repo (79,276 bytes / md5 `17d6726a888e8683f6fa5c82af702c28`) is **a different file**;
fed to this configuration's driver, dmesg shows `CRC mismatch: expected 0xa152` and the image is **all black**. What this project has proven is
the **`main` version** in the table above.

**Fetching and installing** (if the board has internet access; run on the board):

```bash
# ① read and accept the license terms (required; ON Semiconductor AP1302 ISP Firmware License Agreement)
curl -sL -o /tmp/AP1302_LICENSE.txt \
  'https://raw.githubusercontent.com/Xilinx/ap1302-firmware/main/LICENSE.txt'
less /tmp/AP1302_LICENSE.txt
# ② fetch the firmware (use a commit permalink to pin the version)
curl -sL -o /tmp/ap1302_ar1335_single_fw.bin \
  'https://raw.githubusercontent.com/Xilinx/ap1302-firmware/3482046f28bfedbb59e3f7321bef4f0b148d0765/ap1302_ar1335_single_fw.bin'
# ③ verify (79324 bytes / md5 23adc4be340a6bbcc4a7ba562c7b2889)
ls -l /tmp/ap1302_ar1335_single_fw.bin; md5sum /tmp/ap1302_ar1335_single_fw.bin
# ④ install and reboot (do not bind the driver manually)
sudo install -m 644 -o root -g root /tmp/ap1302_ar1335_single_fw.bin /lib/firmware/
sudo systemctl reboot
```

If the board has no internet access, **do ① to ③ on the PC**, `scp` the file to `/tmp` on the board, then run ④:

```bash
# on the PC (after running ① to ③)
scp /tmp/ap1302_ar1335_single_fw.bin <user>@<board>:/tmp/
ssh <user>@<board> 'md5sum /tmp/ap1302_ar1335_single_fw.bin'
# expected: 23adc4be340a6bbcc4a7ba562c7b2889
ssh <user>@<board> 'sudo install -m 644 -o root -g root /tmp/ap1302_ar1335_single_fw.bin /lib/firmware/ && sudo systemctl reboot'
```

```bash
# check after reboot (then redo §3-2)
ssh <user>@<board> 'sudo dmesg | grep -i ap1302 | tail -5; ls /dev/media0'
# expected: AP1302 revision 0.2.6 detected / /dev/media0
```

- ★**Always reboot**. **Do not** manually rebind the driver (on hardware this gives `-17` and a refcount WARNING).
- After reboot, once `/dev/media0` appears, redo §3-2 (PL bring-up).

**License**: the copyright holder of this firmware is **ON Semiconductor** (Semiconductor Components Industries, LLC), and
AMD (Xilinx) is a licensed redistributor. The license is not an OSI-approved open source license but a dedicated EULA
(**ON Semiconductor AP1302 ISP Firmware License Agreement**) in `LICENSE.txt` of the upstream repo, with conditions such as permitting
distribution in binary form only. This release's policy is to **not bundle** this firmware; as in step ① above,
users read and accept the terms themselves before fetching it. For details and why it is not bundled, see [../THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) §11.

---

## 3. Deploying the distribution archive and starting the web stream

The bundle deployed to the board (binaries + launcher + fan control + `install.sh`) is **not in the repo**.
It is distributed as a single `.tar.gz` **distribution archive** (a GitHub Release asset). Contents, install locations, and version consistency are described in
**the `README.md` bundled in the archive**. Everything below is run from the PC (WSL).

★Release page: https://github.com/Leiden21g/kv260detector/releases/tag/v1.0

The distribution archive is named `y7-public-<tag>.tar.gz`. `<tag>` is the **first 8 hex digits of the xclbin md5** of the adopted build
(the build covered by this document = xclbin `14279337` ⇒ `y7-public-14279337.tar.gz`).
Below, the extraction directory is written as `<pkg>` (extracting creates a directory of the same name from the tar).

### 3-0. Extract the distribution archive and fetch MediaMTX (first time only)

```bash
# (a) get the asset and the sha256 listed with it from this repo's GitHub Release page, verify, and extract
#     the asset is on the Release page https://github.com/Leiden21g/kv260detector/releases/tag/v1.0
curl -LO https://github.com/Leiden21g/kv260detector/releases/download/v1.0/y7-public-14279337.tar.gz
sha256sum y7-public-14279337.tar.gz             # expected: ac50a6394f44f1fa9213170ede8badfbf1ee965dda4d136815069d1737229d85
tar -xzf y7-public-14279337.tar.gz              # → y7-public-14279337/ (= <pkg> below)
```

The same Release also carries the **Corresponding Source** for the bundled `allegro_dvt.ko` (GPL-2.0) as an asset
(not needed for deployment; fetch it only when you need the source. Details = `<pkg>/src/allegro-dvt/README.md`):

```bash
curl -LO https://github.com/Leiden21g/kv260detector/releases/download/v1.0/allegro-dvt-gpl-src-31626ef92ff1.tar.gz
sha256sum allegro-dvt-gpl-src-31626ef92ff1.tar.gz   # expected: eb1248ab86f9b940e6482d1af9000dcebcfb1a741b62d4739a5a2c457eee46cb
```

MediaMTX (WebRTC/HLS restreaming for browsers; a third-party MIT-licensed binary) is **not bundled** in the archive.
Get **v1.19.2 `linux_arm64`** from the upstream GitHub Release (https://github.com/bluenviron/mediamtx/releases) and
place the `mediamtx` inside it at `<pkg>/home/mediamtx`:

```bash
# (b) fetch MediaMTX from upstream and place it in <pkg>/home/
cd <pkg>
curl -LO https://github.com/bluenviron/mediamtx/releases/download/v1.19.2/mediamtx_v1.19.2_linux_arm64.tar.gz
curl -LO https://github.com/bluenviron/mediamtx/releases/download/v1.19.2/checksums.sha256
grep linux_arm64 checksums.sha256 | sha256sum -c -           # expected: mediamtx_v1.19.2_linux_arm64.tar.gz: OK
tar -xzf mediamtx_v1.19.2_linux_arm64.tar.gz -C home mediamtx # extract only the mediamtx binary into home/ (the bundled mediamtx.yml is not used)
rm -f mediamtx_v1.19.2_linux_arm64.tar.gz checksums.sha256
md5sum home/mediamtx                                           # expected: 01c0e5f7cff02ef566086367e48c9537 (62,164,043 bytes)
```

Confirmed (2026-09-11): the sha256 of the upstream tarball matched `checksums.sha256`, and the md5 of the extracted `mediamtx` matched
`home/mediamtx` in `MD5SUMS.txt` (= the very binary that has been used for streaming). For configuration, use
`home/mediamtx_detect.yml` bundled in the archive.

### 3-1. Deployment (install.sh)

`install.sh` is at the top of the archive (same level as `firmware/` `home/` `fan/` `MD5SUMS.txt`).

```bash
cd <pkg>
md5sum -c MD5SUMS.txt                     # extracted files are intact (all bundled files OK)
bash install.sh <user>@<board>          # prerequisite checks → scp → md5 check → atomic replace on the board → fan unit enable
```

★The bundled `MD5SUMS.txt` has **no line for `home/mediamtx`** (because it is not bundled).
`md5sum -c` checks only "files that were in the archive". Check the md5 of the `home/mediamtx` you placed in §3-0(b)
yourself against `01c0e5f7cff02ef566086367e48c9537` given in that section.

What install.sh does:

- **Prerequisite checks** (if anything is missing it stops with a reason; it never stops silently):
  the local extracted files match `MD5SUMS.txt` (if `home/mediamtx` is missing it points you to §3-0) / ssh works with a key /
  sudo on the board is NOPASSWD (§2 step 6) / XRT is present (§2-1) / `uname -r` is `6.12.40-xilinx-g31626ef92ff1`
  (vermagic of `allegro_dvt.ko`) / the live streaming units are not active (if active, tear them down first with §3-6).
- Transfers to `~/pkg_<日時>/` on the board (`<日時>` = timestamp; kept after deployment). Verifies md5 after transfer.
- `firmware/` → `/lib/firmware/`, `home/` → the home of `<user>` (`/home/amd-edf`), `fan/` → `/usr/local/bin/` and `/etc/systemd/system/`.
  Creates `/lib/firmware/xilinx/yolov7vcu/` (bit + dtbo + shell.json) for dfx-mgr (§3-2).
- How files are replaced:
  - Files whose **md5 matches the existing file are left untouched** (no backup either). At the end it prints "置換 N 件 / スキップ M 件" (replaced N / skipped M) and the names of replaced files.
  - A file being replaced is first backed up to `<名前>.bak_<日時>` (`<名前>` = name, `<日時>` = timestamp); then **a `.new_<日時>` is placed in the same dir with matching mode/owner and
    swapped in with `mv -f`** (atomic replace by rename; running scripts are not rewritten in place).
  - For the original VCU firmware `al5e*.fw`, the original file from the first replacement is also kept as `.bak_board`.
- Enables `y7-fan.service` (fan control starts at boot).

Whether to reboot after deployment follows install.sh's final message:

- **If the PL / firmware / ko (`/lib/firmware/*`, `~/allegro_dvt.ko`, `~/combined_allegro.dtbo`) were replaced → always reboot**
  (PL overlays cannot be stacked; rmdir crashes the kernel).
  ```bash
  ssh <user>@<board> 'sudo systemctl reboot'
  ```
- If only scripts in home were updated, no reboot is needed (they take effect from the next start).
- If the fan unit was updated, it takes effect from the next boot (for immediate effect: `sudo systemctl restart y7-fan`).

(This install.sh was reworked into the form above on 2026-09-11. On 2026-09-12 it was run against a real board with a pristine official SD as part of the end-to-end run from §1,
confirmed: the first run replaced 42 files; a second run replaced 0 / skipped 42 = re-running changes nothing.)

### 3-2. PL bring-up after reboot (every boot)

Right after a reboot the PL does not have the production bit (dfx-mgr has loaded `k26-starter-kits`).
`vcu_enc_setup.sh` does the insmod of `allegro_dvt.ko` and swaps in the **integrated VCU + camera + YOLO bit**.
**On the official wic it always takes the dfx-mgr / `xmutil` path** (`xmutil unloadapp` → `xmutil loadapp yolov7vcu`,
using `/lib/firmware/xilinx/yolov7vcu/` created by install.sh). The direct configfs apply branch still in the script is
for old SDs without `xmutil` (self-built wic) and is not taken on the official wic.

```bash
ssh <user>@<board> 'bash ~/vcu_enc_setup.sh'
# expected (script output, partly Japanese): [vcu-enc] dfx-mgr 経路(xmutil loadapp yolov7vcu) … ✓ VCU H.264 encoder ready(/dev/video1) camera=/dev/media0 dri=renderD128
ssh <user>@<board> 'ls /dev/video0 /dev/video1 /dev/media0 /dev/dri/renderD128; cat /sys/class/fpga_manager/fpga0/state; ls /sys/kernel/config/device-tree/overlays'
```

- ⛔ Do not rmdir the overlay (kernel crash). Always swap by rebooting.
- ⛔ **If a kernel Oops occurs, do not `systemctl reboot`**. Shutdown hangs and you end up power-cycling (measured 2026-09-06). Save `sudo dmesg` and power-cycle.
  (The Oops seen so far is `fpga_mgr_load` → `cma_heap_map_dma_buf` on the direct configfs apply = old-SD path. It has not occurred on the official wic's dfx-mgr path)
- To do it by hand: `sudo insmod ~/allegro_dvt.ko; sudo xmutil unloadapp; sudo xmutil loadapp yolov7vcu`.

The camera (AP1302) reports a firmware CRC mismatch on about half of reboots. If `sudo dmesg | grep -i ap1302 | tail -5` shows
`AP1302 revision 0.2.6 detected` (even after the mismatch), the driver has retried and capture is fine.
Only if `/dev/media0` is missing, reboot and redo §3-2.
If `/dev/media0` still does not appear and `sudo dmesg | grep -i ap1302` shows
`Direct firmware load for ap1302_ar1335_single_fw.bin failed`, the firmware is not installed (§2-2).

### 3-3. Sanity check (canary; optional but recommended the first time)

Checks the health of PL inference with a fixed input. A match with the 6 GOLD values means the PL, xclbin, n.q, and host combination is correct.

```bash
ssh <user>@<board> 'cd ~/yolov7 && mkdir -p /tmp/cg && rm -f /tmp/cg/o*_L* &&
  printf "%s\n%s\n" "$HOME/yolov7/data26_640/x.bin /tmp/cg/o" "$HOME/yolov7/data26_640/x.bin /tmp/cg/o2" > /tmp/cg/l.txt &&
  PERSIST_NIMG=2 STREAM_NIMG_RT=2 GOAXIS_EN=1 PINGPONG_EN=0 FILELIST=/tmp/cg/l.txt DUMP_HEADS_PF=131,136,140,145,149,154 \
  timeout 120 ./yolov7_host_overlap_camlive_geo640x640 ./data26_640 0 155 >/tmp/cg/run.log 2>&1; echo "rc=$?";
  for L in 131 136 140 145 149 154; do printf "%s " $(md5sum /tmp/cg/o_L$L.bin 2>/dev/null|cut -c1-8); done; echo;
  grep -a TIMING /tmp/cg/run.log | tail -1'
# expected: rc=0 / GOLD = d5053fa6 d4f8cc60 de3383a1 1c5155a5 13a60293 f30905c4 / TIMING N=2 ≈ 139-140 ms/img
#       (measured on a pristine SD on 2026-09-12: GOLD 6/6 match, 140.00 ms/img)
```

- If no head is dumped at all + exit 134, the PL is not programmed (§3-2 was skipped). This is not a CU fault.
- ⚠ `~/canary_golden.sh` has defaults for the 384×640 generation, so it does not produce the 640 GOLD. Use the command above.

### 3-4. Starting the web stream

```bash
ssh <user>@<board> 'rm -f /dev/shm/live.nv12'      # prevents a wrong stride calculation from a stale leftover frame
ssh <user>@<board> 'cd ~ && HOST=./yolov7_host_overlap_camlive_geo640x640_ovl bash ~/run_live_rtsp_stream.sh'
ssh <user>@<board> 'bash ~/browser_stream_mediamtx.sh'
```

`HOST=` is the same as the launcher's default and can be omitted (on 2026-09-12 the default was fixed to the actually deployed name;
earlier versions defaulted to a name not present on the board, so omitting it failed to start). Pass it only when trying a different ELF.

- The launcher creates 7 transient units (capdlive / ppdaemon / probedrain / vcustream / rtspdetect / webmode / yololoop) with
  `systemd-run` and returns in about 30 seconds. The log is `/tmp/live_rtsp_stream.log` on the board.
- `browser_stream_mediamtx.sh` creates an 8th unit, `mediamtx`. **Rerun it every time you tear down and restart live**
  (transient units disappear on stop).
- For about 1 minute after start, lines like `run 1..N t=0-2s frames=0 (+0)` are normal. It is just waiting for the first camera frame.
- ★ Start it from a foreground ssh. If launched with `nohup`/`setsid`, logind kills the whole session when ssh disconnects.

Viewing:

| URL | Content |
|---|---|
| `http://<board>:8889/detect` | ★Recommended. WebRTC (latency ~1 s, built-in player) |
| `http://<board>:8890/` | Viewer page + full view/crop + ←↑↓→ pan buttons. AGPL-3.0 and Corresponding Source notice at the right edge (see below) |
| `http://<board>:8888/detect` | HLS (latency several seconds, fallback) |
| `rtsp://<board>:8554/detect` | RTSP (VLC / ffplay) |

### 3-5. Checking operation (from the PC)

```bash
ssh <user>@<board> 'systemctl is-active capdlive ppdaemon probedrain vcustream rtspdetect webmode yololoop mediamtx; sudo journalctl -u yololoop -n 3 --no-pager'
curl -s -o /dev/null -w "webrtc=%{http_code}\n" -L http://<board>:8889/detect/
curl -s -o /dev/null -w "web=%{http_code}\n" http://<board>:8890/
```

- Pass = all 8 units `active`, and the frames count in yololoop's `run N t=… frames=…` is increasing
  (measured on a pristine SD on 2026-09-12: run1 at 2000 images / 206 s = **9.71 fps**, vcu_stream sent 1600 frames, HTTP 200 on both `:8889` and `:8890`).
- "HTTP 200 but the picture is frozen" is a half-stopped state where only yololoop died. Always judge by whether `yololoop` is alive and frames are increasing.

### 3-6. Stopping (always via the stop flag)

```bash
ssh <user>@<board> '
  touch /tmp/lv/stop
  for w in $(seq 1 200); do pgrep yolov7_host >/dev/null || break; sleep 2; done   # wait for the running host to exit on its own (up to ~7 min)
  for u in yololoop capdlive ppdaemon vcustream rtspdetect webmode mediamtx; do sudo systemctl stop $u; done
  rm -f /tmp/lv/stop'
```

- ★ Stopping a running host with `systemctl stop yololoop` / kill corrupts the PL CU; the only recovery is a reboot.
- Do not stop `probedrain` (if the probe FIFO fills up, the next start wedges).
- Do not add `-f` to `pgrep` (it matches ssh's own command line and the wait loop spins uselessly).

### 3-7. Key launch parameters

The launcher derives all of these from `~/yolov7/geo640.env` (do not hard-code them). Each can be overridden via the launch environment.

| Key | Value | Meaning |
|---|---|---|
| `Y26_NETH/Y26_NETW` | 640/640 | Inference input geometry (= streaming geometry; the camera captures 3840×2160 → capd converts to 640²) |
| `GEO640_FPS_CAP` / `GEO640_CAP_FPS` | 8 / 9 | Streaming fps cap / capd capture fps. Live is bound by inference, so raising the cap does not help |
| `GEO640_GO_FLUSH=cvac`, `GEO640_SNAP_THREADS=2`, `GEO640_GO_EARLY=1` | — | Host-side speed-up gates (GOLD match confirmed) |
| `GEO640_CAPD_PUB=1` | — | Capture 3840×2160, publish 640² (full view = center square decimated / crop = center 640² crop; toggled on :8890) |
| `Y7_SOURCE_URL` | (empty) | ★**AGPL-3.0 §13**: the **Corresponding Source URL** shown on the viewer page (`:8890`). If you let third parties use the stream over a network, you must offer them the source, so its location is shown on the page. If unset, no link is shown and the page displays "AGPL-3.0(対応ソース = 配布物同梱の LICENSE / README 参照)" ("AGPL-3.0 (Corresponding Source = see LICENSE / README bundled with the distribution)"). **If you modify it and stream publicly, set your own publication URL**. If you use this repo unmodified, use `Y7_SOURCE_URL=https://github.com/Leiden21g/kv260detector`; if you stream a modified version, set **your own publication location**. Example: `Y7_SOURCE_URL=https://github.com/Leiden21g/kv260detector bash ~/run_live_rtsp_stream.sh` |
| `NIMG` (launcher env) | 2000 | Images per start. The host restarts at run boundaries (~3 s freeze roughly every 3.4 minutes) |
| `CONF` | 0.25 | Detection threshold |
| `CAM_EXPOSURE/CAM_GAMMA/CAM_SATURATION` | 9 / 12288 / 6144 | AP1302 ISP (backlight compensation. exposure is the AE mode number; 4 to 8 are undefined and must not be used) |

### 3-8. Common symptoms

| Symptom | Cause / fix |
|---|---|
| canary exits 134, no head is produced | PL not programmed. `bash ~/vcu_enc_setup.sh` (rebooting does not help) |
| `[live] ✗ capd が frame を出さない` ("capd produces no frames") | First check `command -v v4l2-ctl`. If missing, `sudo dnf install -y v4l-utils` (§2-1). Without `v4l2-ctl`, the launcher fails silently, the format of `/dev/video0` stays unset → `REQBUFS: Invalid argument` |
| `/dev/media0` missing, launcher exits 5/6 | AP1302 CRC mismatch and the retry failed. reboot → §3-2. If dmesg has `Direct firmware load for ap1302_ar1335_single_fw.bin failed`, the firmware is not installed (§2-2) |
| Detection boxes run away at 300, picture is garbled | Stale leftover `/dev/shm/live.nv12`. Stop → `rm -f /dev/shm/live.nv12` → restart |
| :8889 gives 404/cannot connect, other units active | The `mediamtx` unit is missing. `bash ~/browser_stream_mediamtx.sh` |
| Picture frozen, HTTP is 200 | yololoop stopped (half-stopped). Tear everything down with §3-6 and redo from §3-4 |
| canary outputs values different from GOLD (values are produced) | CU degraded (aftereffect of a mid-run kill). reboot → §3-2 → canary |
| Wedge on the second or later start | The previous run was killed mid-run. reboot |
| `vcu_enc_setup.sh` prints 「clean base 確認」 (clean base check) or 「direct apply」 | It took the direct configfs apply branch (for old SDs) = `xmutil` is missing or `/lib/firmware/xilinx/yolov7vcu/` is missing. On the official wic, redo install.sh |
| kernel Oops in `vcu_enc_setup.sh` (fpga_mgr_load)| Known issue seen with direct configfs apply on old SDs. Do not reboot; power-cycle → load via the dfx-mgr path (§3-2) |
| insmod fails in `vcu_enc_setup.sh` | The kernel is not `6.12.40-xilinx-g31626ef92ff1` (e.g. upgraded by `dnf upgrade`). The bundled `allegro_dvt.ko` cannot be used (vermagic mismatch). Rewriting the SD is fastest. To rebuild for another kernel, use `src/allegro-dvt/` in the distribution archive (upstream identification + patch + `build_allegro_dvt.sh`; GPL-2.0 Corresponding Source), which requires that kernel's build tree and an aarch64 cross toolchain |

### 3-9. Rebuilding

The source and cross-build procedure for the board-side programs are in [../live/README.md](../live/README.md) (`build_live.sh`).

The SDK is created by extracting the `sdk.sh` contained in the ZynqMP common image of the AMD download page "Embedded Platforms" 2025.2
(`xilinx-zynqmp-common-v2025.2_*.tar.gz`, AMD account required)
(`sdk.sh -y -d <SDK 展開先> -p`; `<SDK 展開先>` = SDK install directory). It is not used to boot the board = needed **only when rebuilding**.

⚠ **`build_live.sh` requires the environment variable `SDK` (the PetaLinux 2025.2 common SDK install directory)**. There is no default
(to keep environment-specific paths out of the repo). If unset it prints `✗ SDK 未指定` (SDK not specified) and exits without building anything.
If passing it every time is tedious, add one line to your shell init file (`~/.bashrc` etc.):

```bash
export SDK=<SDK 展開先>      # directory extracted with sdk.sh -y -d <SDK 展開先> -p (where environment-setup-* lives)
```

- `SDK` is a generic name, so in environments where other tools read a variable of the same name, it is safer not to `export` it and
  instead **prefix it to the command only**, as in `SDK=<SDK 展開先> bash build_live.sh all`.
- At the end of the build, md5s are checked against `live/MD5SUMS.expect.txt`. With the same SDK, repeated builds give the same md5
  (the timestamp embedded in the inference host's build_id is pinned with `SOURCE_DATE_EPOCH`).
★2026-09-10: the inference host was simplified to be streaming-only (2 files, `live/inference_host/src/y26_live.cpp` +
`include/y26_live.h`; no Vitis includes needed). Confirmed canary GOLD 6/6 byte-exact match on the board.
The ELF bundled in the distribution archive (Release v1.0) is the simplified build `3f586c9d` built from this public source (identical to the expected value in `live/MD5SUMS.expect.txt`).
Distributions and deployments before 2026-09-12 used `c6b433a0`, built from the pre-simplification source (inference results are identical = canary GOLD 6/6 match).
Regenerating the PL (xclbin/bit) and n.q is out of scope for this release (requires Vitis 2025.2 and the cap platform).
