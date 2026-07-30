# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Workspace layout

This directory is a workspace holding **two unrelated projects** and a backup.
They share a device and nothing else — different languages, different build
systems, different git roots. Work out which one you are in before running
anything.

### 1. The C applications — this repo's own code, git root `.`

Remote `git@github.com:naisema/beepy.git`, built by the **top-level `Makefile`**.
These run on the device's *existing* Raspberry Pi OS install, not on a Buildroot
image, and have nothing to do with `beepy-buildroot/`.

- `beepy-nav/` — a GPS bike navigator: turn-by-turn on a 400×240 1-bit Sharp
  panel, offline OSM basemap and routing. `beepy-nav/DESIGN.md` is the
  specification and carries the reasoning behind every constant;
  `beepy-nav/README.md` is how to ride with it. **Read DESIGN.md before
  changing anything in `beepy-nav/src/`** — the numbers in that code were each
  argued for, and several were paid for with a debugging cycle.
- `gps-monitor/` — the earlier, simpler app: satellite bars and a sky view.
- `libbeepyfb/` — panel drawing. `cover.c` is the analytic coverage renderer
  (signed-distance strokes, subsampled polygons) that makes 1-bit output look
  smooth; `font.c` holds the 5×7 glyph table, which `beepy-nav/mockup.py`
  parses directly.
- `libnmea/` — NMEA parsing and the serial port.
- `tools/` — Mac-side only, all of it: pack builders (`mktiles.py`,
  `mkpack.py`, `mergetiles.py`, `pbf2osm.py`), the one-command map refresh
  (`mkmaps.sh`), fixture generators and the gates.
- `goldens/` — frozen framebuffer dumps. `make check` byte-compares against
  these; they regenerate only under `GOLDEN_OK=1`.
- `host/` — Mac build products (gitignored).

### 2. `beepy-buildroot/` — a vendored third-party build, its own git root

A checkout of [michaelstepner/beepy-buildroot](https://github.com/michaelstepner/beepy-buildroot),
which builds a Buildroot Linux SD-card image. **All commands in the Buildroot
sections below must be run from `beepy-buildroot/`, never from here.** Nothing
in section 1 is part of that image.

### 3. Backups, not code

- `beepy/` — an untracked backup of the `beepy` user's home directory copied off a device (dotfiles, `pip`-installed packages, a `chatgpt_client.py` scratch script). Not part of any build. Note: `beepy/chatgpt_client.py` contains a hardcoded OpenAI API key — it should be revoked and removed rather than edited around, and this directory is gitignored and must never be committed.
- `beepy.tar.gz` — archive of the above home directory.

## The C applications — build and gates

Everything here runs from the workspace root.

```sh
make host           # Mac: portable objects + host/beepy-nav, a replay binary
make design-gate    # Mac: the C pages vs beepy-nav/mockup.py, pixel for pixel
make test-tiles     # Mac: the basemap packs (needs Pillow)
make test-roads     # Mac: the road/place packs
make sync           # rsync to beepy.local, build there, run `make check`
```

**`make check` is the gate, and it runs on the DEVICE**, not on the Mac —
anything touching `/dev/fb1`, evdev or termios only compiles under Linux. It
byte-compares every `--demo` page dump against `goldens/` and runs the unit and
replay suites. It takes about 25 minutes on a Pi Zero 2 W; that is normal, not
a hang.

Standing rules in this project, each learned the hard way:

- **Never modify a committed golden to make a test pass.** A moved golden is a
  changed display; regenerating is deliberate (`GOLDEN_OK=1 make goldens`) and
  belongs in its own commit with the reason.
- **Commit with `git commit -F FILE`, never `-m`.** Messages here carry
  reasoning and embedded quotes, and `-m` has mangled them more than once.
- **A test whose "absent" case reads the device config is not a test.** Three
  assertions had quietly stopped being pairs because one half loaded whatever
  `~/.config/beepy-nav.conf` named. Name the fixture explicitly, or pass
  `--no-basemap` / `--no-roads`.
- Measure rather than estimate, and correct the record in the commit when a
  measurement contradicts something already written down.

## Buildroot image — build

The build is always run inside the Docker container defined by `docker/Dockerfile` (Ubuntu 24.04 + Buildroot's host dependencies). A full build takes ~3.5 hours.

```sh
git submodule init && git submodule update    # submodules are NOT initialized in this checkout
docker build --platform linux/amd64 --file docker/Dockerfile --tag buildroot-os-builder .
docker run --platform linux/amd64 --name my-beepy-buildroot buildroot-os-builder ./build-image.sh
docker cp my-beepy-buildroot:/home/builder/beepy-buildroot/buildroot/output/images/sdcard.img ./sdcard.img
```

Submodule remotes use `git@github.com:` SSH URLs, so SSH access to GitHub is required to fetch them.

For iterative work, run an interactive container so a crashed build can be resumed and artifacts inspected:

```sh
docker run -it --platform linux/amd64 --name my-beepy-buildroot buildroot-os-builder /bin/bash
./build-image.sh                              # first build
cd buildroot && make -j $(nproc)              # resume, reusing ccache
```

On Apple Silicon, `write jobserver: Bad file descriptor` requires serializing the build: add `--cpus=1` to `docker run` and `-j 1` to `./build-image.sh`.

There is no test suite and no linter. Verification means building the image and booting it on hardware.

### After changing a package

`build-image.sh` downloads Buildroot into `buildroot/` (version pinned by `buildroot_version` at the top of that script) and runs `make defconfig BR2_EXTERNAL=../beepy_drivers BR2_DEFCONFIG=../br_defconfig`. Buildroot caches aggressively, so an edit alone usually won't take effect:

```sh
cd buildroot
make <pkg>-dirclean && make -j $(nproc)                    # after changing a package or its br_defconfig options
make linux-savedefconfig                                   # writes output/build/linux-custom/defconfig
find output/ -name ".stamp_target_installed" -delete       # force reinstall into target/
```

## Buildroot image — architecture

### Buildroot external tree — `beepy_drivers/`

A standard `BR2_EXTERNAL` tree (`external.desc` declares name `BEEPY_DRIVERS`, so variables are `$(BR2_EXTERNAL_BEEPY_DRIVERS_PATH)`). `external.mk` globs every `package/*/*.mk`, but **a new package is only visible to menuconfig once it is `source`d from `Config.in`** (target packages) or `Config.in.host` (host tools like `mktable`).

Two kinds of packages live here:

- **Hardware drivers** built from git submodules under `package/*/module` with `SITE_METHOD = local`: `sharp-drm` (display), `beepy-kbd` (keyboard), `beepy-symbol-overlay`, `beepy-tmux-menus`. The driver `.mk` files follow a shared pattern — a `BUILD_CMDS` that compiles `*.dts` to `*.dtbo` with `dtc`, an `INSTALL_IMAGES_CMDS` that drops the overlays into `$(BINARIES_DIR)/rpi-firmware/overlays`, an `INSTALL_TARGET_CMDS` that installs the package's own `init/S01*` script, and both `$(eval $(kernel-module))` and `$(eval $(generic-package))`. Bumping a driver means updating the submodule pointer *and* the `_VERSION` in the `.mk` (the version is hand-maintained, not derived from git).
- **Upstream packages not in Buildroot** (or needing local patches): `aerc`, `notmuch`, `w3m`, `gmime`, `libgc`, `libxapian`, `dante-preload`, plus the `mktable` host tool. Patches are numbered `NNNN-*.patch` in the package directory and applied by Buildroot automatically.

### Configuration files at the repo root

`br_defconfig` is the Buildroot config: ARM Cortex-A53 target, custom Raspberry Pi kernel tarball with `zero2w_defconfig` as its kernel config, F2FS rootfs, and the package selection (`iwd`/NetworkManager, OpenSSH, tmux, Python 3 + pip, mosh, w3m, notmuch, aerc). It wires in `../users`, `../root_overlay`, `../post-build.sh`, and `../post-image.sh` — the `../` prefixes are relative to `buildroot/`, where make runs.

- `cmdline.txt` / `config.txt` — kernel cmdline and Pi firmware config; this is where device-tree overlays (`sharp-drm`, `beepy-kbd`) and the console font size are set.
- `genimage.cfg` — three-partition SD layout: 16M FAT16 boot (holds `cmdline.txt`, `config.txt`, `timezone.txt`, the `wlan/` directory, `zImage`, DTB, overlays), 512M F2FS rootfs, and a small `home` partition that is grown on first boot.
- `users` — creates the `beepy` user with default password `beepbeep`.
- `timezone.txt` — POSIX-format zone (e.g. `EST5EDT`), *not* IANA. Read at boot by `root_overlay/etc/profile.d/set-timezone.sh`.

### Target filesystem — `root_overlay/`

Copied verbatim onto the rootfs. Notable pieces:

- `etc/init.d/S08firstboot` — resizes and formats the home partition, populates `/home/beepy` from `/etc/skel` (including `authorized_keys` if present), and appends it to `/etc/fstab`. Runs only once.
- `etc/init.d/S09runbackground` + `run-background.sh` — the fast-boot mechanism. `post-build.sh` *moves* every `S[1-9]*` init script into `etc/init.d/background/`, and this pair runs them asynchronously after the console is up. A new service that must block boot needs an `S0*` name.
- `etc/wlan.d/{connecting,connected,disconnected}` — hooks sourced by `usr/sbin/wlan-notify.sh`, which watches NetworkManager `StateChanged` signals over D-Bus.
- `usr/bin/gomuks` — a **prebuilt 27MB ARM binary committed to the repo**, not a Buildroot package.
- `sbin/update_buildroot` — on-device in-place updater that loop-mounts a downloaded release image and rsyncs the rootfs over the running system.

### Cellular modem (`gsm-*` scripts)

USB GSM modem support added on top of upstream. Nothing new is compiled — the running image already has `ModemManager`, `pppd`, `dhcpcd` and every needed kernel module (`option`, `usb_wwan`, `huawei_cdc_ncm`, `qmi_wwan`, `ppp_async`), because `BR2_PACKAGE_NETWORK_MANAGER` pulls ModemManager in. The scripts are a thin, idempotent layer over `mmcli` + `nmcli`:

- `root_overlay/usr/local/bin/gsm-up` — waits for the modem, unlocks the SIM only if actually locked, waits for registration, then creates-or-updates a NetworkManager `gsm` profile and activates it.
- `gsm-down [--power-off]` — disconnects; `--power-off` also `--disable`s the modem *and* sets `--set-power-state-low`, which is what actually cuts current draw (plain `--disable` leaves it powered).
- `gsm-status [--watch]` — modem/SIM/signal/route/data-usage summary, deliberately kept under 50 columns to fit the Beepy screen.
- `gsm-at 'AT+CSQ'` — raw AT commands; the image ships no terminal program at all.
- `root_overlay/etc/init.d/background/S65gsm` — opt-in boot autoconnect, gated on `AUTOCONNECT` and placed directly in `background/` per the existing convention.
- `root_overlay/usr/local/lib/gsm/common.sh` — shared config loading and mmcli helpers.
- `gsm.conf` (repo root) → `/boot/gsm.conf`, wired into `genimage.cfg`'s file list so it sits on the FAT partition and is editable from any PC, matching how `timezone.txt` and `wlan/*.psk` are handled. Sourced through a `sed 's/\r$//'` copy so CRLF endings from a Windows editor don't break it.

Non-obvious things these scripts encode, each of which cost a debugging cycle:

- **NM's device for a `gsm` connection is the modem's control port** (`ttyUSB2`), not the interface holding the IP. The addresses live on `GENERAL.IP-IFACE` — `wwan0` for NCM/QMI, `pppN` when MM falls back to PPP. `gsm_nm_device` vs. `gsm_active_device` in `common.sh` keep these separate; `nmcli` queries need the former, `ip` needs the latter.
- **Escalate to root *before* parsing arguments.** `gsm_need_root` re-execs via `exec sudo -E "$0" "$@"`, so calling it after the arg loop has `shift`ed silently discards every flag. Also, `sudo`'s `secure_path` excludes `/usr/local/bin`, which is why the scripts self-escalate instead of being run as `sudo gsm-up`.
- **`mmcli --output-keyvalue` renders list properties as `key.value[1]`**, so a plain lookup of `key.value` silently returns nothing (hence `gsm_modem_list_field`).
- **Start the AT reader before writing the command.** The modem answers in milliseconds; opening `cat` after the `printf` loses the reply. ModemManager holds `ttyUSB2` but leaves `ttyUSB0` free, which is why that is the default `AT_PORT`. `mmcli --command` is not an alternative — it returns `Unauthorized: operation only allowed in debug mode`.
- Cellular gets `ipv4.route-metric` 700 vs. wifi's 600, so wifi stays preferred and cellular is a standby route; `PREFER="cellular"` drops it to 500. `ipv6.method` is `ignore` because this image blacklists the `ipv6` module.

### Post-build vs. post-image

`post-build.sh` runs against `$TARGET_DIR` before the filesystem images are made: autologin on tty1, `beepy` in sudoers, backgrounding the init scripts, `set convert-meta off` in inputrc (needed for direct symbol input from the keyboard), restoring pip's `.py` sources (Buildroot installs only `.pyc`), and quieting kernel printk. Several customization scripts work by *appending* to this file, so keep it append-safe. `post-image.sh` then calls `genimage` to assemble `sdcard.img`.

### Customization pipeline

`build-image.sh` looks for `customization.json` in the repo root; for each key it executes `customization_scripts/<key>.sh "<value>"` before building, erroring out if the script is missing or not executable. GitHub Actions (`.github/workflows/build.yml`) generates that JSON from `workflow_dispatch` inputs — and only when the repository is **private**, since the values may include a wifi password. Each script mutates repo files in place (`sed` on `cmdline.txt` / `timezone.txt`, renaming `wlan/ssid_goes_here.psk`, appending to `post-build.sh`).

Adding an option means both: a new input in `build.yml` *and* a matching executable `customization_scripts/<name>.sh` (make sure it is `chmod +x`). Interdependent inputs read each other from `customization.json` directly — see `wifi_ssid.sh` / `wifi_password.sh`, where the SSID script only validates and the password script does the work.

### Release and upgrade automation

- Pushing a **tag** triggers `build.yml` to build a vanilla image and publish it as a GitHub Release; `workflow_dispatch` instead uploads a customized `sdcard.img` as a run artifact. The workflow mounts a zstd-compressed BTRFS loopback file as Docker's data root and strips `BR2_CCACHE=y` from `br_defconfig`, because the build does not otherwise fit in a runner's disk.
- `upgrade_buildroot.yml` runs daily, scraping buildroot.org for the newest `YYYY.02` LTS release, rewriting `buildroot_version` in `build-image.sh`, and opening/updating a PR on the `buildroot-upgrade` branch. `upgrade_scripts/upgrade_buildroot.py` communicates with the shell helpers by writing `upgrade_scripts/upgrade_status.json` (commit message, PR body, modified files); the `git_*.sh` / `github_pr_*.sh` helpers read it with `-c`. It is a `uv` inline-script (PEP 723 header), run as `uv run ./upgrade_scripts/upgrade_buildroot.py`. The job is guarded to only run in a repo literally named `beepy-buildroot`, so forks don't spam PRs.

## The physical device

There is a live Beepy on the local network at **`beepy.local`**, reachable over SSH as `beepy` with the key at `~/.ssh/id_rsa`. `gsm-status`, `dmesg` and `mmcli` on that device are the fastest way to check hardware behaviour.

That device is where the C applications actually run and where `make check` executes. Relevant state on it:

- `~/beepy-src/` — the rsync target of `make sync`; `make check` runs there.
- `/usr/local/bin/beepy-nav`, `/usr/local/bin/gps-monitor` — the installed binaries.
- `~/packs/` — the map packs (a few hundred MB; **gitignored, never committed**), named by `~/.config/beepy-nav.conf`.
- `~/routes/`, `~/rides/` — GPX routes in, ride logs out.
- The panel is owned by `fbterm`. Taking it means `kill -STOP $(pgrep -x fbterm)` and **`kill -CONT` on the way out** — including on a crash, or the console is left frozen and the device looks dead.

⚠️ **The running device is not this Buildroot OS at all — it is Raspberry Pi OS.** `gcc --version` reports `gcc (Raspbian 10.2.1-6+rpi1)`, the rootfs is **ext4**, and the kernel is `6.1.21-v7+`, whereas `br_defconfig` specifies the `stable_20250127` tarball and an **f2fs** rootfs. Practical consequences:

- Anything added to `root_overlay/` is absent from the device until deployed by hand (`COPYFILE_DISABLE=1 tar czf - ... | ssh ... 'sudo tar xzf - -C / --no-same-owner'` — the env var prevents macOS AppleDouble `._` siblings).
- Testing on the device does **not** validate this repo's config. Package selection, kernel options and init layout can all differ.
- Conversely the device has a **native C toolchain** (gcc 10.2.1, g++, make, ld) and `cdc_acm` compiled into the kernel — neither of which the Buildroot image provides. Don't infer Buildroot capabilities from what works there, or vice versa: `CONFIG_USB_ACM` is *not* set in `zero2w_defconfig`, so a USB CDC-ACM device (e.g. a u-blox GPS) that works on the Raspbian install would need a kernel config change to work on a Buildroot image.

## Hardware constraints worth remembering

- Buildroot has no package manager on the device — everything is compiled into the image, and "updating" means reflashing or running `update_buildroot`. A change that would be an `apt install` on Raspbian is a `br_defconfig` edit plus a full rebuild here.
- Target is Beepy v1: Pi Zero 2 W with the original Sharp black & white screen, firmware 3.4+ (images tested against 3.8).
- The display is `/dev/fb1` (framebuffer console mapped there via `fbcon=map:10`); `cfg80211`, `rfkill`, `ipv6`, and `brcmfmac` are blacklisted in modprobe and loaded deliberately later by `S60brcmfmac`.
