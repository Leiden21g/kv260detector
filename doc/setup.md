# KV260 セットアップ手順 — 起動 SD の準備から Web 配信の起動まで

対象: Kria KV260 Starter Kit + AP1302 カメラモジュール(KV260 付属 IAS モジュール)。
到達点: ブラウザで `http://<board IP>:8889/detect` に検出オーバレイ付きライブ映像(WebRTC、約 9.7 fps)が見える状態。

本書は 3 段構成。

1. §1 起動 SD の準備(AMD 公式 Kria 汎用 Starter Kit 組込み Linux 2025.2)
2. §2 初回起動と PC からの ssh 準備(rootfs の拡張・ssh 公開鍵・パスワード不要な sudo・追加パッケージ・カメラ firmware)
3. §3 配布アーカイブの配置(install.sh)と Web 配信の起動・停止・確認

PC 側は WSL2(Ubuntu)を想定。`<board>` は board の IP(本書の例は `192.168.0.35`)、`<user>` は `amd-edf`(公式イメージのログインユーザ)。

**確認の程度の表記**: 本文中、実機や公式イメージの実物で確かめたものは「確認済」、手順として書いたが通していないものは
「**未検証**」と明記する。印の無い操作手順は §1 冒頭の条件で確認したもの。

---

## 1. 起動 SD の準備

AMD が公式に配布している **Kria 汎用 Starter Kit 組込み Linux 2025.2**(EDF 25.11、KV260/KR260/KD240 共通)を使う。

確認の範囲: **2026-09-12 に、素の公式 SD**(上記 wic を焼いた直後のもの)で §1 から §3 まで**頭から通して実機確認した**。
到達点は **Web 配信の稼働** — canary が GOLD 6/6 一致(**140.00 ms/img**)、live run1 が 2000 枚 / 206 s = **9.71 fps**、
8 unit すべて `active`、`:8889` と `:8890` が HTTP 200、vcu_stream が 1600 frames 送出。

その通しで判明した手順の不足は本書に反映済み(rootfs の拡張 = §2 手順 3、`v4l-utils` = §2-1、
カメラ firmware = §2-2、host 鍵の削除 = §2 手順 5、NOPASSWD の実行のしかた = §2 手順 6)。
カメラ firmware `ap1302_ar1335_single_fw.bin` は公式 SD にも dnf feed にも無いため、利用者が上流から各自取得する
(入手元とライセンスは確定済。手順 = §2-2)。それ以外は素の SD から本書どおりに通る。

### 1-1. 必要物

| 物 | 入手元 | 備考 |
|---|---|---|
| microSD 16GB 以上 | — | |
| `kria-image-full-cmdline-kria-zynqmp-generic.rootfs-20251116094523.wic.xz` | AMD Adaptive Computing Wiki「Kria SOMs & Starter Kits」(要 AMD アカウント) | 起動イメージ本体。ログインは `amd-edf` |
| `BOOT-k26-smk-sdt-1.06-20251115173103.bin` | AMD アカウント要。案内は Wiki「Kria SOM Boot Firmware Update」 https://xilinx-wiki.atlassian.net/wiki/spaces/A/pages/3020685316/Kria+SOM+Boot+Firmware+Update | K26 Boot FW v1.06。公式推奨は v1.06 以上だが、**更新は必須ではない**(§1-2) |
| MediaMTX v1.19.2 linux_arm64 | GitHub Release(§3-0) | 配布アーカイブに同梱していない第三者バイナリ |

本書の検証に使ったファイルの md5(ダウンロードが壊れていないかの確認用):

```
2b62feb42d4f3a63d12ad3e4c4ba4750  kria-image-full-cmdline-kria-zynqmp-generic.rootfs-20251116094523.wic.xz
3f0796d99b61e11c802749b4d50cf71d  BOOT-k26-smk-sdt-1.06-20251115173103.bin
```

このイメージの kernel は `6.12.40-xilinx-g31626ef92ff1`。XRT は **イメージに入っていない**(確認済: wic の rootfs に
XRT / zocl が無い)ので §2-1 で EDF の feed から入れる(`xrt` 2.20.0。依存で `zocl` と
`kernel-module-zocl-6.12.40-xilinx-g31626ef92ff1` が入る)。配布アーカイブの `allegro_dvt.ko` はこの kernel
専用(vermagic 一致が必須)で、推論 host はこの XRT 版の sysroot でリンクしてある。

### 1-2. Boot FW(QSPI)— 更新は推奨だが必須ではない

KV260 の Boot FW は SOM の QSPI にある。**公式推奨は v1.06 以上**(2025.2 wic の Recommended FW = 01.06。確認済: 公式 doc)。
v1.07(2026.1 向け)は未検証。

★**1.06 未満でも起動する**(2026-09-12 に実機で確認済)。通し検証に使った個体は `bootfw_status` が
`Last Booted Image: Image B` = **`K26-BootFW-01.01`**(`ImageA Revision Info: XilinxSOM_BootFW_20220915`、
QSPI image `XilinxSom_QspiImage_v1.1_20210422`)のままで、公式 EDF 25.11 SD が問題なく起動し、
PL bring-up・canary・Web 配信まで**全機能が動作した**。したがって FW 更新は **推奨だが必須ではない**
(本書の範囲では未更新のまま動作する)。以下は、起動しない場合や公式推奨に合わせたい場合の手順。

DTB と bootargs は QSPI の Boot FW が供給する(確認済: 公式 wic の p1 = boot 区画に `system.dtb` が無い)。

★この節の更新手順そのものは **未実施(未検証)**。公式ページの手順を本書向けにまとめたもの。

**(a) SD が起動する場合 — `xmutil` で更新する**(board の Linux 上から。§2 の ssh 準備後):

```bash
scp BOOT-k26-smk-sdt-1.06-20251115173103.bin <user>@<board>:~/
ssh -t <user>@<board>
# ↓ board 上で実行
sudo xmutil bootfw_status                       # 現在の A/B バンクと各 Revision を確認
sudo xmutil bootfw_update -i ~/BOOT-k26-smk-sdt-1.06-20251115173103.bin
sudo reboot
```
```bash
# ★再起動した直後の、その起動中に必ず実行する(打たないと次回の起動で元のバンクへ戻る)
ssh -t <user>@<board> 'sudo xmutil bootfw_update -v; sudo xmutil bootfw_status'
```
判定: `bootfw_status` の **Last Booted Image** が新しいバンクで、そのバンクの Revision Info が `1-20251115173103`
になっていれば完了(表示形式は実物では未確認。項目名は公式ページに基づく)。

**(b) SD が起動しない場合 — Boot Image Recovery Tool**(board 単体で QSPI を書き換える。未検証):

1. board の LAN(J10)と PC を **LAN ケーブルで直結**する(ルータ経由の LAN につないだままだと、
   Recovery Tool の固定アドレス 192.168.0.111 が既存機器と衝突する恐れがある)。
2. PC の有線 IP を `192.168.0.x`(`.111` 以外、例 `192.168.0.10/24`)に固定する。
3. **SW1(FWUEN)を押しながら** 12V を投入する。
4. PC のブラウザで `http://192.168.0.111` を開き、`BOOT-k26-smk-sdt-1.06-20251115173103.bin` を書き込む。

出典: https://xilinx.github.io/kria-apps-docs/bootfw/build/html/docs/bootfw_image_recovery.html

### 1-3. SD への書込み

`.wic.xz` は xz 圧縮のまま配布されている。

```bash
xz -dc kria-image-full-cmdline-kria-zynqmp-generic.rootfs-20251116094523.wic.xz \
  | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress      # /dev/sdX は SD のデバイス(★要確認。間違えると PC のディスクを壊す)
```

WSL2 では SD を直接扱えないことが多い。その場合は Windows 側で balenaEtcher(`.wic.xz` をそのまま指定できる)か、
`xz -dk` で展開した `.wic` を Rufus / Win32DiskImager で書く。

---

## 2. 初回起動と ssh 準備

1. **配線して電源を入れる**(出典 UG1089 Interfaces https://docs.amd.com/r/en-US/ug1089-kv260-starter-kit/Interfaces):
   - KV260 には **boot mode スイッチは無い**。工場出荷時から QSPI32 ブート固定で、QSPI の Boot FW(§1-2)が
     microSD の Linux を起動する。
   - microSD を **J11** に挿し、カメラ(AP1302 モジュール)を **IAS0 = J7**、LAN を **J10**、USB(UART/JTAG)を **J4** に
     つないでから、**最後に 12V を J12** に入れる。
   - Rev2 キャリアでは **J20 にジャンパが無い**こと(挿さっていると JTAG ブートになる)。
   - **SW1(FWUEN)は Recovery Tool(§1-2 (b))専用**。通常の起動では押さない。
2. **初回ログインはシリアル(UART)かモニタ(DP)で行う**。
   ⚠ `amd-edf` は**初期パスワードが空で、初回ログイン時にパスワード変更を強制される**。この状態では **ssh では入れない**
   (確認済: 公式 wic の shadow)。手順 5 以降の ssh はここでパスワードを設定してから。
   - **UART**: J4 の USB を PC につなぐと FTDI が 4 ポート出す。その **2 番目**(Linux 直なら通常 `/dev/ttyUSB1`、
     Windows なら COM 番号の 2 番目)を **115200 bps** で開く。board 側のコンソールは `ttyPS1`。
     WSL2 からは USB シリアルが見えないので、Windows 側の Tera Term / PuTTY を使う。
   - ログイン: ユーザ `amd-edf`、パスワードは空のまま Enter → 新しいパスワードを 2 回入力。
   ⚠ **`sudo` は初期状態ではパスワードを求める**(実機で確認済)。手順 6 で NOPASSWD にする。
3. ★**rootfs を SD 全体へ広げる(操作が必要)**。★この作業は §2-1 の dnf(約 200MB)**より前**に行うこと。

   ⚠ **焼いた直後の rootfs は SD 全体には広がらない**(2026-09-12 に実機で確認済)。
   初回起動の `systemd-growfs` は動いているが、**パーティションの大きさまでしか広げない**ので、
   journal には `systemd-growfs[498]: Successfully resized "/" to 999.2M bytes.` と出る(= 正常動作)。
   実測では 125GB の SD に対し `/dev/mmcblk1p2` が **999.2MB**、`df -h /` は **950M / 空き 171MB** だった。
   このままでは §2-1 の追加パッケージが入らない。

   まず現状を見る:
   ```bash
   lsblk /dev/mmcblk1      # p2 のサイズ。SD 容量より極端に小さければ拡張が要る
   df -h /
   ```

   素の SD には `growpart` も `resize2fs` も無い(`parted` / `sfdisk` / `partx` / `fdisk` はある)。
   実機で通した手順は次のとおり:
   ```bash
   sudo sfdisk -d /dev/mmcblk1 > ~/ptable_before.bak      # ★先にパーティションテーブルを退避(復旧用)
   sudo dnf install -y e2fsprogs-resize2fs                 # 素の SD には resize2fs が無い(これだけなら 999MB でも入る)
   echo ",+" | sudo sfdisk --no-reread -N 2 /dev/mmcblk1   # p2 を SD 末尾まで広げる(開始セクタは維持される)
   sudo partx -u /dev/mmcblk1                              # カーネルに反映(再起動は不要)
   sudo resize2fs /dev/mmcblk1p2                           # マウントしたままオンライン拡張
   df -h /
   ```
   期待(125GB の SD の実測): p2 が **116G**、`/` が **113G / 使用 1% 前後 = 空き 108G**。

   - ⚠ **開始セクタを変えないこと**。`-N 2` と `,+`(サイズだけ末尾まで)の組はセクタ開始位置を維持する。
     開始位置を動かすと既存のファイルシステムを失う。
   - `sfdisk` が `Re-reading the partition table failed: Device or resource busy` を出すのは**想定内**
     (root が載っているデバイスなので再読込できない)。続く `partx -u` でカーネルに反映されるので、
     この警告のまま次へ進んでよい(実機で確認済)。
   - 失敗したときは `sudo sfdisk /dev/mmcblk1 < ~/ptable_before.bak` で元のテーブルに戻せる。
4. IP を確認(`ip -4 addr`)。DHCP 既定。固定にするなら `/etc/systemd/network/` に `.network` を置く。
   ★ファイル名は既定の `80-wired.network` **より前に並ぶ名前**にする(例 `10-static.network`)。systemd-networkd は
   名前順で最初に Match したファイルだけを使うので、後ろに並ぶ名前だと既定の DHCP 設定が勝つ。
   例(**未検証**。インタフェース名は `ip -4 addr` で確認して合わせる。ssh 越しに行うと切れるので UART から):
   ```ini
   # /etc/systemd/network/10-static.network
   [Match]
   Name=eth0

   [Network]
   Address=192.168.0.35/24
   Gateway=192.168.0.1
   DNS=192.168.0.1
   ```
   反映は `sudo systemctl restart systemd-networkd`(または reboot)。
5. PC から ssh 公開鍵を登録する(以後の手順はすべて **パスワード入力なしの ssh** を前提にしている)。

   ⚠ **SD を焼き直した / 入れ替えた直後は board の host 鍵が変わっている**。PC 側に古い host 鍵が残っていると
   `REMOTE HOST IDENTIFICATION HAS CHANGED` で ssh が止まる(2026-09-12 に実機で確認済)。先に消しておく:
   ```bash
   ssh-keygen -R <board の IP>      # PC 側で実行。古い host 鍵を known_hosts から消す
   ```

   **5-a. PC 側で鍵を作る**(既に `~/.ssh/id_ed25519.pub` 等があれば不要):
   ```bash
   ls ~/.ssh/id_*.pub 2>/dev/null || ssh-keygen -t ed25519 -C "<PC 名など>"
   #   保存先は既定(~/.ssh/id_ed25519)のまま Enter。パスフレーズは任意(付けるなら ssh-agent を使う)
   ```

   **5-b. board へ登録する**(ここで board の `<user>` のパスワードを **1 回だけ**聞かれる = 手順 2 で設定したもの):
   ```bash
   ssh-copy-id -i ~/.ssh/id_ed25519.pub <user>@<board>
   ```
   `ssh-copy-id` が無い環境では同等の手作業で:
   ```bash
   cat ~/.ssh/id_ed25519.pub | ssh <user>@<board> \
     'mkdir -p ~/.ssh && chmod 700 ~/.ssh && cat >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys'
   ```

   **5-c. パスワード無しで入れることを確認する**:
   ```bash
   ssh -o BatchMode=yes <user>@<board> true && echo "鍵認証 OK"
   #   BatchMode=yes はパスワードを聞かずに失敗させる。ここで失敗するなら鍵が効いていない
   ssh <user>@<board> 'uname -r; grep ^VERSION= /etc/os-release'
   # 期待: 6.12.40-xilinx-g31626ef92ff1
   #       VERSION="25.11+development-S11151020 (scarthgap)"      (確認済: 公式 wic の /etc/os-release)
   ```
   XRT はこの時点ではまだ無い(§2-1 で入れて確認する)。
   鍵認証が効かないときは、board 側の権限を確認する(`~/.ssh` = 700、`~/.ssh/authorized_keys` = 600、
   home ディレクトリが他人に書込可能でないこと。どれかが緩いと sshd は鍵を無視する)。

   **5-d.(任意)PC 側に別名を付ける** — `~/.ssh/config` に:
   ```
   Host kv260
       HostName <board の IP>
       User <user>
       IdentityFile ~/.ssh/id_ed25519
   ```
   以後 `ssh kv260` で入れる。本書のコマンド例は `<user>@<board>` 表記のままなので、読み替えるか
   `<user>@<board>` の所に `kv260` を書けばよい。

   ⚠ 以後も board の SD を焼き直すたびに host 鍵は変わる。そのつど本手順冒頭の `ssh-keygen -R <board の IP>` を
   打ってから 5-b をやり直す。

6. **パスワード不要な sudo(NOPASSWD)を設定する**。本書の手順は `ssh <user>@<board> 'sudo …'` の形で
   sudo を**非対話**で呼ぶ(`install.sh`、§3-2 以降の bring-up・起動・停止)。パスワードを求められると
   そこで止まる(`install.sh` は冒頭で確認し、未設定なら本手順を案内して止まる)。

   **6-a. 現状を確認する**:
   ```bash
   ssh <user>@<board> 'sudo -n true && echo "NOPASSWD 済" || echo "要設定"'
   #   -n = パスワードを聞かずに失敗させる。「要設定」なら 6-b へ
   ```
   初期状態は **要設定**(`/etc/sudoers.d/99-amd-edf` が `amd-edf ALL=(ALL) ALL` = パスワード必須。確認済)。

   **6-b. 設定する**(2026-09-12 に**素の SD で実機確認済**。結果 =
   `/etc/sudoers.d/zz-amd-edf-nopasswd` が `-r--r----- root root`、`visudo -c` で 3 ファイルとも `parsed OK`、
   `sudo -n true` 成功)

   ★**下のブロックは必ず 1 行ずつ実行する**(まとめて貼らない)。`sudo visudo -cf "$t"` が
   パスワードを聞くため、一括で貼ると**後続の行がパスワード入力に吸われて消える**
   (実機で確認済 — `NOPASSWD OK` が出ないまま `rm` や確認コマンドが食われる)。1 行ずつなら手順どおり通る。

   ★**先に締め出し対策をする**: 公式 wic は root のパスワードが `*`(ロック)で `su` では root になれず、`pkexec` も無い
   (確認済)。sudoers を壊すと root になる手段が無くなるので、**別の端末で root シェルを 1 本開いたままにしておく**:
   ```bash
   # 別端末(作業が終わるまで閉じない)
   ssh -t <user>@<board>
   sudo -i          # パスワードを入力 → プロンプトが # になる(root シェル)
   ```
   作業用の端末で board に対話ログインして(sudo がパスワードを聞くので `-t` を付ける):
   ```bash
   ssh -t <user>@<board>
   # ↓ board 上で実行($USER は board 上で展開させること。PC から ssh の引数に直接書くと PC 側の $USER に化ける)
   t=$(mktemp)
   echo "$USER ALL=(ALL) NOPASSWD: ALL" > "$t"
   sudo visudo -cf "$t"                          # ★文法チェック。"parsed OK" を確認してから次へ
   sudo install -m 0440 -o root -g root "$t" /etc/sudoers.d/zz-$USER-nopasswd
   rm -f "$t"
   sudo visudo -c                                # 全体の文法チェック(全ファイル parsed OK)
   exit
   ```
   - ★**ファイル名は `99-…` より後ろに並ぶ名前にする**(上の `zz-…`)。sudoers は**後に読まれたルールが勝つ**ので、
     既存の `99-amd-edf`(パスワード必須)より先に読まれる名前だと NOPASSWD が上書きされて効かない。
   - `/etc/sudoers.d/` のファイル名に `.` を含めたり末尾を `~` にしたりすると**読まれない**(sudo の仕様)。
   - `/etc/sudoers` 本体を直接編集しない。壊すと sudo が一切使えなくなる。
     なお、本体の `@includedir /etc/sudoers.d` 行**より後に**追記したルールは sudoers.d の後に読まれるので、そちらが勝つ
     (本書の開発に使った board はこの状態 = 本体側の追記で NOPASSWD になっている。本書では推奨しない)。
   - **失敗したときの復旧**: 保持しておいた root シェルで `rm /etc/sudoers.d/zz-*` を実行する。root シェルが無い場合は、
     SD を Linux PC に挿し、p2(ext4、`LABEL=root`)をマウントして `etc/sudoers.d/zz-*` を削除する。
     どちらでも `99-amd-edf` は残るので、**パスワードを聞かれる元の状態に戻るだけ**。

   **6-c. 効いたか確認する**(PC から):
   ```bash
   ssh <user>@<board> 'sudo -k; sudo -n true && echo "NOPASSWD OK"'
   #   sudo -k = キャッシュ済みの認証を捨てる(捨てないと直前のパスワード入力で通ってしまい判定にならない)
   ```
   確認できたら、保持していた root シェルを閉じてよい。

   **6-d.(推奨・任意・未検証)ssh のパスワード認証を切る**

   ⚠ NOPASSWD にすると **`<user>` のログイン = root 権限**になる。sshd は初期状態で**パスワード認証が有効**なので、
   鍵認証を確認できたら切ることを推奨する。
   前提: **5-c で鍵ログインの成功を確認済み** かつ **別端末で root シェル(6-b の `sudo -i`)を保持**していること。

   公式 EDF の sshd は `sshd.socket` + `sshd@.service`(**接続ごとに起動する socket 方式**)で、`sshd.service` は無い
   (確認済)。したがって `systemctl restart sshd` は不要(存在しない)で、設定は**次の新規接続から**効く。
   `sshd_config` の先頭に `Include /etc/ssh/sshd_config.d/*.conf` があるが、ディレクトリ自体は無い(確認済)ので作る:
   ```bash
   # board 上で実行
   sudo mkdir -p /etc/ssh/sshd_config.d
   printf 'PasswordAuthentication no\nKbdInteractiveAuthentication no\n' | sudo tee /etc/ssh/sshd_config.d/10-no-password.conf >/dev/null
   sudo sshd -t && echo "構文 OK"
   sudo sshd -T | grep -iE '^(passwordauthentication|kbdinteractiveauthentication|pubkeyauthentication)'
   #   期待: passwordauthentication no / kbdinteractiveauthentication no / pubkeyauthentication yes
   ```
   ```bash
   # PC から(新規接続から効く。restart 不要)
   ssh -o BatchMode=yes <user>@<board> true && echo "鍵 OK"
   ssh -o PubkeyAuthentication=no -o PreferredAuthentications=password <user>@<board> true   # Permission denied (publickey) なら OK
   ```
   鍵で入れなくなった場合は、保持していた root シェルで `rm /etc/ssh/sshd_config.d/10-no-password.conf` を実行すれば元に戻る
   (UART / DP からのログインはこの設定の影響を受けない)。

### 2-1. 追加パッケージの導入と注意点

§2 の手順 5(ssh 鍵)と手順 6(NOPASSWD)を済ませてから行う。

- 配信に必要なパッケージが入っていないので、§3-1 の前に dnf で導入する(AMD EDF の repo は設定済み、約 200MB)。
  ★**`xrt` を必ず含める**(確認済: wic に XRT / zocl が無い。EDF feed に `xrt-202520.2.20.0` があり、依存で `zocl` →
  `kernel-module-zocl-6.12.40-xilinx-g31626ef92ff1` が入る):
  ```bash
  ssh <user>@<board> 'sudo dnf install -y xrt gstreamer1.0 gstreamer1.0-plugins-base-meta gstreamer1.0-plugins-good-meta \
    gstreamer1.0-plugins-bad-meta gstreamer1.0-rtsp-server gstreamer1.0-python python3-pygobject \
    libopencv-core409 libopencv-imgproc409 devmem2 curl v4l-utils kernel-module-vcu vcu-firmware'
  ```
  (`kernel-module-vcu`/`vcu-firmware` は AMD 版 allegro.ko と fw。配布アーカイブ同梱の patch 版 `allegro_dvt.ko` と v2019.2 fw を優先して使うので、必須ではない)
  (**2026-09-12 に、このリストを素の SD で一括導入して確認済**)
- ★**`v4l-utils` を必ず含める**(欠けても**症状が分かりにくい**)。`v4l2-ctl` が無いと launcher
  (`run_live_rtsp_stream.sh`)の `v4l2-ctl … 2>/dev/null` が**無言で失敗**し、`/dev/video0` の format が
  未設定のまま `REQBUFS: Invalid argument`(`sudo dmesg` に `vb2_core_reqbufs` の WARNING)となり、
  最終的には `[live] ✗ capd が frame を出さない` という**まったく別の症状として現れる**(実機で確認済)。
  `sudo dnf install -y v4l-utils` で解決する。導入後の確認:
  ```bash
  ssh <user>@<board> 'command -v v4l2-ctl'      # 期待: /usr/bin/v4l2-ctl
  ```
- **`zocl` は `xrt` の依存で入る。明示指定は不要で、`dnf clean all` も不要**
  (2026-09-12 に素の SD で確認済: `xrt-202520.2.20.0` と同時に `zocl-202520.2.20.0` +
  `kernel-module-zocl-6.12.40-xilinx-g31626ef92ff1` が入り、`/lib/modules/…/updates/zocl.ko` と
  `/usr/lib/libxrt_coreutil.so.2.20.0` を確認した)。
- **XRT の確認**(導入後):
  ```bash
  ssh <user>@<board> 'ls /usr/lib/libxrt_coreutil.so.2.*'
  # 期待: /usr/lib/libxrt_coreutil.so.2.20.0(推論 host のリンク先と同版)
  ```
  `install.sh` もこれを確認し、無ければ理由を出して止まる。
- ⛔ **`dnf upgrade` / `dnf update` は打たない**。kernel が上がると配布アーカイブの `allegro_dvt.ko` の vermagic が合わなくなり、
  VCU(= 配信)が動かなくなる。個別に入れるときも `dnf install <名前>` だけにする。
- PL のロードは **dfx-mgr(`xmutil loadapp yolov7vcu`)経由**で行う(§3-2)。公式 wic には dfx-mgr / `xmutil` が常にある。
  configfs へ直接 overlay を書くと 7.8MB の bit で kernel Oops を踏んだ実績がある(configfs 直接 apply は旧 SD 用の経路)。
- `ap1302` の media entity 名は `ap1302.3-003c`。launcher は entity を検出式で探すので名前の違いは気にしなくてよい。
- fan は base DTB で pwm-fan が既に有効。`y7-fan.service` を enable すると `fancontrol.service` と入れ替わる(Conflicts)。

★ 配布アーカイブのカーネルモジュール(`allegro_dvt.ko`)は **kernel 6.12.40-xilinx-g31626ef92ff1 専用**(vermagic 一致が必須)。
XRT は 2.20.0(host ELF のリンク先 sysroot と同版)。`uname -r` がこれと違う場合は配布アーカイブのバイナリは使えない。

### 2-2. カメラ firmware `ap1302_ar1335_single_fw.bin`

このファイルは公式 SD イメージにも AMD EDF 25.11 の dnf feed にも**含まれていない**。
入手元は **AMD(Xilinx)が GitHub で公開している [Xilinx/ap1302-firmware](https://github.com/Xilinx/ap1302-firmware)**
(著作権者 = ON Semiconductor)で確定している。本公開物には**同梱していない**ので、利用者が上流から各自取得する。

★ **J7 の IAS カメラを使う場合にのみ必要**。カメラを使わない構成(静止画・ダンプ入力のみ)では不要。

**必要な理由**(実機で確認済): このファイルが `/lib/firmware/` に無いと AP1302 の driver が probe に失敗する。

```
ap1302 3-003c: Direct firmware load for ap1302_ar1335_single_fw.bin failed with error -2
ap1302 3-003c: probe with driver ap1302 failed with error -2
```

結果として **`/dev/media0` が生えず**、§3-2 の `vcu_enc_setup.sh` が `camera=無し` を出す(= 配信が立たない)。

**諸元**:

| 項目 | 値 |
|---|---|
| ファイル名 | `ap1302_ar1335_single_fw.bin` |
| 置き場所 | `/lib/firmware/ap1302_ar1335_single_fw.bin` |
| パーミッション / 所有 | `0644` / `root:root` |
| サイズ | 79,324 バイト |
| md5 | `23adc4be340a6bbcc4a7ba562c7b2889` |
| sha256 | `2dd09e34c68eb2e9ff2b488c9b7fb6d77f4673bff9c1af167d9d466e795ec1c2` |
| 上流 | https://github.com/Xilinx/ap1302-firmware の `main` ブランチ(実体の最終更新コミット `3482046f28bfedbb59e3f7321bef4f0b148d0765`、2024-01-25)。上記 md5 と**バイト一致**(確認済) |
| AMD EDF 25.11 の dnf feed | **無い**(`dnf provides '*/ap1302_ar1335_single_fw.bin'` は No matches。driver 本体の `kernel-module-ap1302` は入る) |

★ **版に注意**: 同 repo の `xlnx_rel_v2022.1` ブランチ版(79,276 バイト / md5 `17d6726a888e8683f6fa5c82af702c28`)は**別物**で、
本構成の driver に食わせると dmesg に `CRC mismatch: expected 0xa152` が出て**全黒**になる。本プロジェクトで実証済なのは
上表の **`main` 版**である。

**取得と配置**(board からインターネットに出られる場合。board 上で実行する):

```bash
# ① ライセンス条項を読んで同意する(必須。ON Semiconductor AP1302 ISP Firmware License Agreement)
curl -sL -o /tmp/AP1302_LICENSE.txt \
  'https://raw.githubusercontent.com/Xilinx/ap1302-firmware/main/LICENSE.txt'
less /tmp/AP1302_LICENSE.txt
# ② firmware を取得(版を固定するためコミット permalink を使う)
curl -sL -o /tmp/ap1302_ar1335_single_fw.bin \
  'https://raw.githubusercontent.com/Xilinx/ap1302-firmware/3482046f28bfedbb59e3f7321bef4f0b148d0765/ap1302_ar1335_single_fw.bin'
# ③ 検証(79324 バイト / md5 23adc4be340a6bbcc4a7ba562c7b2889)
ls -l /tmp/ap1302_ar1335_single_fw.bin; md5sum /tmp/ap1302_ar1335_single_fw.bin
# ④ 配置して reboot(driver の手動 bind はしない)
sudo install -m 644 -o root -g root /tmp/ap1302_ar1335_single_fw.bin /lib/firmware/
sudo systemctl reboot
```

board からインターネットに出られない場合は、**① 〜 ③ を PC 側で行い**、`scp` で board の `/tmp` に送ってから ④ を実行する:

```bash
# PC 側(① 〜 ③ を実行後)
scp /tmp/ap1302_ar1335_single_fw.bin <user>@<board>:/tmp/
ssh <user>@<board> 'md5sum /tmp/ap1302_ar1335_single_fw.bin'
# 期待: 23adc4be340a6bbcc4a7ba562c7b2889
ssh <user>@<board> 'sudo install -m 644 -o root -g root /tmp/ap1302_ar1335_single_fw.bin /lib/firmware/ && sudo systemctl reboot'
```

```bash
# reboot 後に確認(この後 §3-2 をやり直す)
ssh <user>@<board> 'sudo dmesg | grep -i ap1302 | tail -5; ls /dev/media0'
# 期待: AP1302 revision 0.2.6 detected / /dev/media0
```

- ★**必ず reboot する**。driver を手で bind し直すのは**やらないこと**(実機では `-17` と refcount の WARNING になる)。
- reboot 後に `/dev/media0` が出てから §3-2(PL bring-up)をやり直す。

**ライセンス**: この firmware の著作権者は **ON Semiconductor**(Semiconductor Components Industries, LLC)で、
AMD(Xilinx)は許諾を受けた再配布者である。ライセンスは OSI 承認のオープンソースライセンスではなく、上流 repo の
`LICENSE.txt` に置かれた専用 EULA(**ON Semiconductor AP1302 ISP Firmware License Agreement**)で、バイナリ形式での
配布のみを認めるなどの条件が付く。本公開物はこの firmware を**同梱しない**方針であり、上の手順 ① のとおり
利用者自身が条項を読んで同意したうえで取得する。詳細と同梱しない理由は [../THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) §11 を参照。

---

## 3. 配布アーカイブの配置と Web 配信の起動

board へ配置する一式(バイナリ + launcher + fan 制御 + `install.sh`)は **repo には入っていない**。
`.tar.gz` 1 本の**配布アーカイブ**(GitHub Release の asset)として配る。中身・配置先・版の整合は
**アーカイブ同梱の `README.md`** にある。以下は PC(WSL)から実行する。

★Release ページ: https://github.com/Leiden21g/kv260detector/releases/tag/v1.0

配布アーカイブの名前は `y7-public-<tag>.tar.gz`。`<tag>` には採用ビルドの **xclbin の md5 先頭 8 桁**が入る
(本書が対象とする採用ビルド = xclbin `14279337` ⇒ `y7-public-14279337.tar.gz`)。
以下では展開先を `<pkg>` と書く(展開すると tar の中に同名の dir ができる)。

### 3-0. 配布アーカイブを展開し、MediaMTX を取得して置く(初回のみ)

```bash
# (a) 本 repo の GitHub Release ページから asset と、併記の sha256 を取得して照合し、展開する
#     asset は Release ページ https://github.com/Leiden21g/kv260detector/releases/tag/v1.0 にある
curl -LO https://github.com/Leiden21g/kv260detector/releases/download/v1.0/y7-public-14279337.tar.gz
sha256sum y7-public-14279337.tar.gz             # 期待: ac50a6394f44f1fa9213170ede8badfbf1ee965dda4d136815069d1737229d85
tar -xzf y7-public-14279337.tar.gz              # → y7-public-14279337/ (= 以下の <pkg>)
```

同じ Release には、同梱する `allegro_dvt.ko`(GPL-2.0)の**対応ソース**も asset として置いてある
(配置には不要。ソースが要るときだけ取得する。詳細 = `<pkg>/src/allegro-dvt/README.md`):

```bash
curl -LO https://github.com/Leiden21g/kv260detector/releases/download/v1.0/allegro-dvt-gpl-src-31626ef92ff1.tar.gz
sha256sum allegro-dvt-gpl-src-31626ef92ff1.tar.gz   # 期待: eb1248ab86f9b940e6482d1af9000dcebcfb1a741b62d4739a5a2c457eee46cb
```

MediaMTX(ブラウザ向け WebRTC/HLS 再配信、第三者の MIT ライセンス・バイナリ)はアーカイブに**同梱していない**。
上流の GitHub Release(https://github.com/bluenviron/mediamtx/releases)から **v1.19.2 の `linux_arm64`** を取得し、
中の `mediamtx` を `<pkg>/home/mediamtx` に置く:

```bash
# (b) MediaMTX を上流から取得して <pkg>/home/ に置く
cd <pkg>
curl -LO https://github.com/bluenviron/mediamtx/releases/download/v1.19.2/mediamtx_v1.19.2_linux_arm64.tar.gz
curl -LO https://github.com/bluenviron/mediamtx/releases/download/v1.19.2/checksums.sha256
grep linux_arm64 checksums.sha256 | sha256sum -c -           # 期待: mediamtx_v1.19.2_linux_arm64.tar.gz: OK
tar -xzf mediamtx_v1.19.2_linux_arm64.tar.gz -C home mediamtx # mediamtx 本体だけを home/ へ(同梱の mediamtx.yml は使わない)
rm -f mediamtx_v1.19.2_linux_arm64.tar.gz checksums.sha256
md5sum home/mediamtx                                           # 期待: 01c0e5f7cff02ef566086367e48c9537(62,164,043 バイト)
```

確認済(2026-09-11): 上流 tarball の sha256 が `checksums.sha256` と一致し、展開した `mediamtx` の md5 は
`MD5SUMS.txt` の `home/mediamtx` と一致した(= 配信実績のあるバイナリそのもの)。設定はアーカイブ同梱の
`home/mediamtx_detect.yml` を使う。

### 3-1. 配置(install.sh)

`install.sh` はアーカイブの直下(`firmware/` `home/` `fan/` `MD5SUMS.txt` と同じ階層)にある。

```bash
cd <pkg>
md5sum -c MD5SUMS.txt                     # 展開物が壊れていないこと(同梱ファイルは全 OK)
bash install.sh <user>@<board>          # 前提確認 → scp → md5 照合 → board 上で原子置換 → fan unit enable
```

★同梱の `MD5SUMS.txt` には **`home/mediamtx` の行は無い**(同梱していないため)。
`md5sum -c` は「アーカイブに入っていたファイル」だけを検査する。§3-0(b) で置いた
`home/mediamtx` の md5 は、同節に書いた `01c0e5f7cff02ef566086367e48c9537` と自分で照合すること。

install.sh がやること:

- **前提確認**(欠けていれば理由を出して止まる。無言では止まらない):
  手元の展開物が `MD5SUMS.txt` と一致(`home/mediamtx` が無ければ §3-0 を案内)/ 鍵で ssh できる /
  board の sudo が NOPASSWD(§2 手順 6)/ XRT がある(§2-1)/ `uname -r` が `6.12.40-xilinx-g31626ef92ff1`
  (`allegro_dvt.ko` の vermagic)/ live 配信 unit が active でない(active なら先に §3-6 で畳む)。
- 転送は board の `~/pkg_<日時>/`(配置後も残る)。転送後に md5 を照合する。
- `firmware/` → `/lib/firmware/`、`home/` → `<user>` の home(`/home/amd-edf`)、`fan/` → `/usr/local/bin/` と `/etc/systemd/system/`。
  `/lib/firmware/xilinx/yolov7vcu/`(bit + dtbo + shell.json)を作る(dfx-mgr 用。§3-2)。
- 置換のしかた:
  - 既存と **md5 が同じファイルは触らない**(退避も作らない)。最後に「置換 N 件 / スキップ M 件」と置換したファイル名を出す。
  - 置換するファイルは `<名前>.bak_<日時>` に退避してから、**同じ dir に `.new_<日時>` を置いて mode/owner を合わせ、
    `mv -f` で差し替える**(rename による原子置換。走行中のスクリプトを書き換えない)。
  - VCU の元 firmware `al5e*.fw` は、初めて置換するときの元ファイルを `.bak_board` にも残す。
- `y7-fan.service` を enable(boot で fan 制御が立つ)。

配置後の reboot は install.sh の最後の表示に従う:

- **PL / firmware / ko(`/lib/firmware/*`、`~/allegro_dvt.ko`、`~/combined_allegro.dtbo`)を置換したとき → 必ず reboot**
  (PL overlay は stack できない。rmdir は kernel crash)。
  ```bash
  ssh <user>@<board> 'sudo systemctl reboot'
  ```
- home のスクリプト類だけの更新なら reboot は不要(次の起動から効く)。
- fan unit を更新したときは次回 boot から有効(今すぐなら `sudo systemctl restart y7-fan`)。

(この install.sh は 2026-09-11 に上の形へ改めた。PC 上で board を模した環境での動作確認はしたが、
実機 board に対して通した確認はまだ無い = 未検証。改修前の版では実機配置を確認済。)

### 3-2. reboot 後の PL bring-up(毎回)

reboot 直後は PL に本番 bit が無い(dfx-mgr が `k26-starter-kits` を載せている)。
`vcu_enc_setup.sh` が `allegro_dvt.ko` の insmod と、**VCU + カメラ + YOLO 統合 bit** の載せ替えを行う。
**公式 wic では常に dfx-mgr / `xmutil` 経路**(`xmutil unloadapp` → `xmutil loadapp yolov7vcu`。
install.sh が作る `/lib/firmware/xilinx/yolov7vcu/` を使う)になる。スクリプトに残っている configfs 直接 apply の分岐は、
`xmutil` の無い旧 SD(自作 wic)用で、公式 wic では通らない。

```bash
ssh <user>@<board> 'bash ~/vcu_enc_setup.sh'
# 期待: [vcu-enc] dfx-mgr 経路(xmutil loadapp yolov7vcu) … ✓ VCU H.264 encoder ready(/dev/video1) camera=/dev/media0 dri=renderD128
ssh <user>@<board> 'ls /dev/video0 /dev/video1 /dev/media0 /dev/dri/renderD128; cat /sys/class/fpga_manager/fpga0/state; ls /sys/kernel/config/device-tree/overlays'
```

- ⛔ overlay を rmdir しない(kernel crash)。載せ替えは必ず reboot から。
- ⛔ **kernel Oops が出たら `systemctl reboot` しない**。shutdown で hang して電源再投入になる(2026-09-06 実測)。`sudo dmesg` を保存して電源を入れ直す。
  (実績のある Oops は `fpga_mgr_load` → `cma_heap_map_dma_buf` で、configfs 直接 apply = 旧 SD 用経路のもの。公式 wic の dfx-mgr 経路では出ていない)
- 手動でやるなら: `sudo insmod ~/allegro_dvt.ko; sudo xmutil unloadapp; sudo xmutil loadapp yolov7vcu`。

カメラ(AP1302)は reboot の約半分で firmware CRC mismatch を出す。`sudo dmesg | grep -i ap1302 | tail -5` に
`AP1302 revision 0.2.6 detected` が(mismatch の後でも)出ていれば driver が引き直しており撮像は正常。
`/dev/media0` が無い場合だけ reboot して §3-2 をやり直す。
それでも `/dev/media0` が出ず、`sudo dmesg | grep -i ap1302` に
`Direct firmware load for ap1302_ar1335_single_fw.bin failed` があるときは firmware が入っていない(§2-2)。

### 3-3. 動作確認(canary、任意だが初回は推奨)

PL 推論の健全性を固定入力で確認する。GOLD 6 本と一致すれば PL・xclbin・n.q・host の組が正しい。

```bash
ssh <user>@<board> 'cd ~/yolov7 && mkdir -p /tmp/cg && rm -f /tmp/cg/o*_L* &&
  printf "%s\n%s\n" "$HOME/yolov7/data26_640/x.bin /tmp/cg/o" "$HOME/yolov7/data26_640/x.bin /tmp/cg/o2" > /tmp/cg/l.txt &&
  PERSIST_NIMG=2 STREAM_NIMG_RT=2 GOAXIS_EN=1 PINGPONG_EN=0 FILELIST=/tmp/cg/l.txt DUMP_HEADS_PF=131,136,140,145,149,154 \
  timeout 120 ./yolov7_host_overlap_camlive_geo640x640 ./data26_640 0 155 >/tmp/cg/run.log 2>&1; echo "rc=$?";
  for L in 131 136 140 145 149 154; do printf "%s " $(md5sum /tmp/cg/o_L$L.bin 2>/dev/null|cut -c1-8); done; echo;
  grep -a TIMING /tmp/cg/run.log | tail -1'
# 期待: rc=0 / GOLD = d5053fa6 d4f8cc60 de3383a1 1c5155a5 13a60293 f30905c4 / TIMING N=2 ≈ 139-140 ms/img
#       (2026-09-12 の素の SD での実測: GOLD 6/6 一致、140.00 ms/img)
```

- 全 head「dump 無し」+ exit 134 なら PL 未 program(§3-2 を飛ばしている)。CU 異常ではない。
- ⚠ `~/canary_golden.sh` は 384×640 世代の既定値を持つので 640 の GOLD は出ない。上のコマンドを使う。

### 3-4. Web 配信の起動

```bash
ssh <user>@<board> 'rm -f /dev/shm/live.nv12'      # 古い frame の残留による stride 誤算出を防ぐ
ssh <user>@<board> 'cd ~ && HOST=./yolov7_host_overlap_camlive_geo640x640_ovl bash ~/run_live_rtsp_stream.sh'
ssh <user>@<board> 'bash ~/browser_stream_mediamtx.sh'
```

`HOST=` は launcher の既定値と同じなので省略できる(2026-09-12 に既定を実在する配備名へ修正した。
それ以前の版は既定が board に無い名前で、省略すると起動できなかった)。別の ELF を試すときだけ渡す。

- launcher は 7 つの transient unit(capdlive / ppdaemon / probedrain / vcustream / rtspdetect / webmode / yololoop)を
  `systemd-run` で作り約 30 秒で戻る。ログは board の `/tmp/live_rtsp_stream.log`。
- `browser_stream_mediamtx.sh` が 8 つ目の `mediamtx` unit を作る。**live を畳んで再開したときは毎回打ち直す**
  (transient unit は stop で消える)。
- 起動直後 1 分ほど `run 1..N t=0-2s frames=0 (+0)` が並ぶのは正常。カメラの最初の frame を待っているだけ。
- ★ 起動は前景の ssh で行う。`nohup`/`setsid` で投げると ssh 切断時に logind がセッションごと殺す。

視聴:

| URL | 内容 |
|---|---|
| `http://<board>:8889/detect` | ★推奨。WebRTC(遅延 ~1 s、内蔵プレイヤー) |
| `http://<board>:8890/` | 視聴ページ + 全景/切り抜き + ←↑↓→ pan ボタン。右端に AGPL-3.0 と対応ソースの表示(下記) |
| `http://<board>:8888/detect` | HLS(遅延 数秒、フォールバック) |
| `rtsp://<board>:8554/detect` | RTSP(VLC / ffplay) |

### 3-5. 稼働確認(PC から)

```bash
ssh <user>@<board> 'systemctl is-active capdlive ppdaemon probedrain vcustream rtspdetect webmode yololoop mediamtx; sudo journalctl -u yololoop -n 3 --no-pager'
curl -s -o /dev/null -w "webrtc=%{http_code}\n" -L http://<board>:8889/detect/
curl -s -o /dev/null -w "web=%{http_code}\n" http://<board>:8890/
```

- 合格 = 8 unit すべて `active`、かつ yololoop の `run N t=… frames=…` の frames が増えている
  (2026-09-12 の素の SD での実測: run1 が 2000 枚 / 206 s = **9.71 fps**、vcu_stream 1600 frames 送出、`:8889`・`:8890` とも HTTP 200)。
- 「HTTP 200 だが画が止まっている」は yololoop だけ落ちた半停止。判定は必ず `yololoop` の生死と frames 増加で行う。

### 3-6. 停止(必ず stop フラグ経由)

```bash
ssh <user>@<board> '
  touch /tmp/lv/stop
  for w in $(seq 1 200); do pgrep yolov7_host >/dev/null || break; sleep 2; done   # 走行中 host の自然終了を待つ(最長 ~7 分)
  for u in yololoop capdlive ppdaemon vcustream rtspdetect webmode mediamtx; do sudo systemctl stop $u; done
  rm -f /tmp/lv/stop'
```

- ★ 走行中の host を `systemctl stop yololoop` / kill で止めると PL の CU が壊れ、復旧は reboot のみ。
- `probedrain` は止めない(probe FIFO が満杯になると次の起動が wedge する)。
- `pgrep` に `-f` を付けない(ssh 自身のコマンドラインに一致して待ちが空回りする)。

### 3-7. 起動時の主要パラメータ

すべて `~/yolov7/geo640.env` から launcher が導出する(直書きしない)。起動 env で個別上書き可。

| キー | 値 | 意味 |
|---|---|---|
| `Y26_NETH/Y26_NETW` | 640/640 | 推論入力幾何(= 配信幾何、カメラは 3840×2160 取込 → capd が 640² へ変換) |
| `GEO640_FPS_CAP` / `GEO640_CAP_FPS` | 8 / 9 | 配信 fps 蓋 / capd 取込 fps。live は推論律速なので蓋を上げても伸びない |
| `GEO640_GO_FLUSH=cvac`, `GEO640_SNAP_THREADS=2`, `GEO640_GO_EARLY=1` | — | host 側の高速化 gate(GOLD 一致確認済) |
| `GEO640_CAPD_PUB=1` | — | 取込 3840×2160、publish 640²(全景 = 中央正方 decimate / 切り抜き = 中央 640² crop、:8890 で切替) |
| `Y7_SOURCE_URL` | (空) | ★**AGPL-3.0 §13**: 視聴ページ(`:8890`)に出す**対応ソースの URL**。配信を第三者にネットワーク越しに使わせる場合、利用者にソースを提供する義務があるため、その在処をページに出す。未設定ならリンクは出ず「AGPL-3.0(対応ソース = 配布物同梱の LICENSE / README 参照)」と表示される。**改変して公開配信するなら、自分の公開先 URL を設定すること**。本 repo をそのまま使うなら `Y7_SOURCE_URL=https://github.com/Leiden21g/kv260detector`、改変して配信するなら**自分の公開先**を設定すること。例: `Y7_SOURCE_URL=https://github.com/Leiden21g/kv260detector bash ~/run_live_rtsp_stream.sh` |
| `NIMG`(launcher env) | 2000 | 1 起動あたりの枚数。run 境界で host が再起動(~3 s の凍結が 約 3.4 分毎) |
| `CONF` | 0.25 | 検出しきい値 |
| `CAM_EXPOSURE/CAM_GAMMA/CAM_SATURATION` | 9 / 12288 / 6144 | AP1302 ISP(逆光対策。exposure は AE モード番号、4〜8 は未定義で使用禁止) |

### 3-8. よくある症状

| 症状 | 原因 / 対処 |
|---|---|
| canary が exit 134、head が 1 本も出ない | PL 未 program。`bash ~/vcu_enc_setup.sh`(reboot は無駄) |
| `[live] ✗ capd が frame を出さない` | まず `command -v v4l2-ctl`。無ければ `sudo dnf install -y v4l-utils`(§2-1)。`v4l2-ctl` が無いと launcher が無言で失敗し、`/dev/video0` の format 未設定 → `REQBUFS: Invalid argument` になる |
| `/dev/media0` が無い、launcher が exit 5/6 | AP1302 CRC mismatch で引き直し失敗。reboot → §3-2。dmesg に `Direct firmware load for ap1302_ar1335_single_fw.bin failed` があれば firmware 未配置(§2-2) |
| 検出枠が 300 件で暴走、画が破綻 | 古い `/dev/shm/live.nv12` の残留。停止 → `rm -f /dev/shm/live.nv12` → 再起動 |
| :8889 が 404/接続不可、他 unit は active | `mediamtx` unit が無い。`bash ~/browser_stream_mediamtx.sh` |
| 画が凍結、HTTP は 200 | yololoop 停止(半停止)。§3-6 で全部畳んで §3-4 からやり直す |
| canary が GOLD と違う値を出す(値は出る) | CU 劣化(走行中 kill の後遺症)。reboot → §3-2 → canary |
| 2 回目以降の起動で wedge | 前回を mid-run kill した。reboot |
| `vcu_enc_setup.sh` が「clean base 確認」「direct apply」を出す | configfs 直接 apply(旧 SD 用)の分岐に入っている = `xmutil` が無いか `/lib/firmware/xilinx/yolov7vcu/` が無い。公式 wic なら install.sh をやり直す |
| `vcu_enc_setup.sh` で kernel Oops(fpga_mgr_load)| 旧 SD の configfs 直接 apply で出た既知事象。reboot せず電源再投入 → dfx-mgr 経路(§3-2)で載せる |
| `vcu_enc_setup.sh` で insmod 失敗 | kernel が `6.12.40-xilinx-g31626ef92ff1` でない(`dnf upgrade` で上がった等)。同梱 `allegro_dvt.ko` は使えない(vermagic 不一致)。SD を焼き直すのが早い。別 kernel 向けに作り直すなら配布アーカイブの `src/allegro-dvt/`(上流の特定 + patch + `build_allegro_dvt.sh`。GPL-2.0 の対応ソース)で再ビルドできるが、その kernel の build tree と aarch64 クロスツールチェインが要る |

### 3-9. 再ビルド

board 側プログラムのソースとクロスビルド手順は [../live/README.md](../live/README.md)(`build_live.sh`)。

SDK は AMD ダウンロードページ「Embedded Platforms」2025.2 の ZynqMP common image
(`xilinx-zynqmp-common-v2025.2_*.tar.gz`、要 AMD アカウント)に入っている `sdk.sh` を展開して作る
(`sdk.sh -y -d <SDK 展開先> -p`)。board の起動には使わない = **再ビルドするときだけ**必要。

⚠ **`build_live.sh` は環境変数 `SDK`(PetaLinux 2025.2 common SDK の展開先)が必須**。既定値は持たない
(環境固有のパスを repo に書かないため)。未指定だと `✗ SDK 未指定` を出して何も作らずに終了する。
毎回付けるのが手間なら、シェルの初期化ファイル(`~/.bashrc` 等)に 1 行足しておく:

```bash
export SDK=<SDK 展開先>      # sdk.sh -y -d <SDK 展開先> -p で展開したディレクトリ(environment-setup-* がある所)
```

- `SDK` は汎用的な名前なので、他のツールが同名の変数を読む環境では `export` せず、
  `SDK=<SDK 展開先> bash build_live.sh all` のように**コマンドの前にだけ付ける**方が安全。
- ビルド末尾で `live/MD5SUMS.expect.txt` と md5 を突合する。同じ SDK なら何度ビルドしても同じ md5 になる
  (推論 host の build_id に入る日時は `SOURCE_DATE_EPOCH` で固定している)。
★2026-09-10: 推論 host は Web 配信専用へ簡素化した(`live/inference_host/src/y26_live.cpp` +
`include/y26_live.h` の 2 ファイル、Vitis include 不要)。board で canary GOLD 6/6 byte-exact 一致を確認済。
配布アーカイブ同梱の ELF(`c6b433a0`)は簡素化前のものなので、**このまま使える**(差替は任意)。
PL(xclbin/bit)と n.q の再生成は本公開物の範囲外(Vitis 2025.2 と cap platform が要る)。
