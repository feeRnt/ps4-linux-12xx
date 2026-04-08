
Linux Kernel - On the Sony PlayStation 4
========================================

This is a Linux Kernel source tailored to run on exploitable PlayStation 4 systems with various subsystem patches from     
the [fail0verflow team](https://github.com/fail0verflow/ps4-linux),    
[eeply](https://github.com/eeply/ps4-linux),    
[Ps3itaTeam](https://github.com/Ps3itaTeam/ps4-linux),   
[rancido](https://github.com/rancido),    
valeryy (no Github - contributed to PS4 Baikal southbridges),    
[mircoho](https://github.com/ps4gentoo/ps4-linux-5.3.7),    
[codedwrench](https://github.com/codedwrench/ps4-linux/),    
[tihmstar](https://github.com/tihmstar/ps4-linux/tree/ps4-4.14.93-belize),    
[crashniels](https://github.com/crashniels/linux/),     
[saya](https://www.youtube.com/channel/UCc20KAcPCj9Ut8IQF3umSjg),    
[whitehax0r](https://github.com/whitehax0r/ps4-linux-baikal),    
[DFAUS](https://github.com/DFAUS-git/ps4-baikal-5.4.247-kernel) -- and others.

For a more detailed credits section, check out [this page](https://dionkill.github.io/ps4-linux-tutorial/ending.html#kernel-developers).

This fork originally aimed to make the internal WiFi+Bluetooth modules on specific PlayStation 4 models with the Marvell 88w8897 combo card (internal codename Torus 2) functional, as they typically error out on default kernels.

Over time it grew into a broader PS4-focused kernel tree covering graphics hotplug and bridge handling, HDMI audio, GPU clocking, thermal and LED control, firmware quirks, and newer desktop/container-oriented config paths.
The active branch in this repo is now `7.0-Clean`; older `5.4`, `5.15`, and `6.15` branches are still kept around as known-good reference points for some consoles.

<br>

-------
## Current Status (`7.0-Clean`)

- `7.0-Clean` is the rolling PS4-focused Linux 7.0 branch in this repository.
- Graphics and display work now includes custom PS4 bridge and encoder patches, HDMI hotplug detection fixes via AUX/DPCD polling, a DPCD-readiness gate to avoid premature link training, 1080p120 mode re-added for testing, PS4 PCIe ASPM and clock-state tuning, a Liverpool/Gladius SCLK force driver, and deeper GFX/SDMA low-power states disabled for steadier sustained performance on PS4.
- HDMI audio on Liverpool-based systems is fixed by enabling IEC958 on the relevant R6xx converters during init.
- PS4 platform support now includes an Aeolia/Belize front-panel LED driver with optional thermal mode, an Aeolia/Belize fan threshold and RPM `hwmon` driver, a lower minimum fan threshold, a PS4 DMI spoof fallback helper, `/dev/ps4-mesa-lock` for Mesa/kernel compatibility checks, and the missing extra firmware blobs in-tree.
- Networking and runtime work now includes the Aeolia `sky2` interrupt-storm fix, which also fixes the associated reported runaway memory-leak symptom seen on affected systems, plus BORE scheduler support with PS4/Jaguar tuning, dmem/TTM VRAM protection tiers, and cgroup plus namespace support for modern userspace.
- Build and forward-port maintenance now includes the Strawberry builder with `Server` and `General` profiles, ThinLTO or FullLTO selection, `mt76x8` build fixes for newer kernels, the `ps4_bridge` attach callback fix for newer DRM APIs, and General-profile `DMI` / `fw_cfg` sysfs support for desktop userspace compatibility.

<br>

-------
## Console Models and Southbridge

While the CUH-1216/1215 models are definitively known to have the Torus 2 models with problematic WiFi, along with some 11xx models with similar WiFi issues, here is a list of consoles reported working without any hiccups from the kernels in this repo:

| Console Model | Variation | WiFi+BT Chip Present | Compatible Kernel (Patched) |
|---|---|---| --- |
| CUH-1216(A/B) | Phat - Belize B0 | Marvell 88w8897 (Torus 2) | *6.15.4, 5.15.15* |
| CUH-1215(A/B) | Phat - Belize | Marvell 88w8897 (Torus 2) | *6.15.4, 5.15.15*  |
| CUH-1003  | Phat  - Aeolia | ? | *6.15.4; [probably non-built in firmware version](#builtin-fw-anchor)* |
| CUH-1004A | Phat - Aeolia | Marvell 88w8797 (Torus 1) | *6.15.4; [non-built in firmware version](#builtin-fw-anchor)* |
| CUH-1116A | Phat - Aeolia | ? | *6.15.4* |
| CUH-2215B | Slim - Baikal | ? | *5.4.247* |
| CUH-2216A | Slim - Baikal B1 | MediaTek 7668 | *5.4.247* |
| CUH-2216A | Slim - Belize | MediaTek 7668 | *5.15.15* |
| CUH-7116B | Pro - Baikal B1 | ? | *5.4.247* |
| CUH-7202B | Pro - Baikal | ? | *5.4.247* |

```
[A and B are just hard-drive specification: 500 GB vs 1000GB].

Aeolia, Belize and Baikal are the console Southbridges.
B0, B1 etc. are the Southbridge subrevisions.
```
<a name="builtin-fw-anchor"></a>
[ Certain older models (specifically the 1004A) needed a kernel that
was built without embedding the latest proprietary firmware blobs (for
WiFi+Bluetooth), and instead functioned properly with older firmware
sourced from the initramfs. For these, each affected kernel release has a "no-built-in-fw"
version for best functionality. See the releases page of the referring kernels for more
information.]
<br>

- Note: the table above is conservative and mostly reflects confirmed reports from the older release branches. `7.0-Clean` is newer rolling work and is still being validated model-by-model.

- TODO: Add a list with all supported console models, their southbridges, and their compatible kernels.

<br>

----
## Fixing the Wireless Card on CUH-1216

The main patches which in combination fix the CUH-1216/1215 wireless module are:     
[150 MHz rate limit quirk on the 88w8897 card's Function 0](https://github.com/feeRnt/ps4-linux-12xx/commit/df7f7dbb1b0fff6026e159540f029988c8067b70).

relying on the [patch for added sdio_id for the Function 0](https://github.com/feeRnt/ps4-linux-12xx/commit/f4835fb020010acff2b70e4c5fa9430e07f0073b),

Then a [few SDHCI Host quirks for the PlayStation SDHCI host](https://github.com/feeRnt/ps4-linux-12xx/commit/e6f342df7737722d5e27f0ae3974e493c5fe4ca4) {only the SDHCI_QUIRK2_PRESET_VALUE_BROKEN is needed now},

additionally with [extra retries for MMC CMD 52 or 53, which it would usually fail on](https://github.com/feeRnt/ps4-linux-12xx/commit/c57162e5ec7a4aa3af3310a36dc963b5c0298dfe) {this is optional}.

The primary culprit behind the failed SDIO initialization, seems to be that the card doesn't properly support 208 or 200 MHz clock rate on this PS4 SDHCI host, causing the card to show tuning and other command failures.
You can read more about the search for a solution [from here](https://ps4linux.com/forums/d/221-ps4-phat-wifi-fix-test-marvell-8897-torus-20/14).    

Through MUCH trial and error, I was able to reach such an arcane fix,     
<br>
![Many of the kernels I had to compile and test before finally landing
on the fix kernel](./IMAGES/github_pic2.png)

and here's a screenshot with working internal WiFi and Bluetooth as shown in the logs, on an Arch Linux system running on my CUH-1216 console:    
<br>
![Working WiFi image](./IMAGES/github_pic1.png)
<br>

Hard work paid off!

<br>

----
## Branches

`7.0-Clean` is the active branch and where current PS4 platform work lands first.

Older branches are still useful as historical references or fallback kernels:

- `5.15.15-belize`: clean WiFi, blackscreen, and misc. fixes for Belize southbridges.
- `5.15.189-belize`: later 5.15-based Belize branch, but not maintained as heavily as `5.15.15-belize`.
- `5.4.247-baikal-dfaus`: 5.4-based Baikal branch with blackscreen fixes and MT7668 support, based on DFAUS' source.
- `6.15.4-aeolia-belize-crashniels`: 6.15-based Aeolia/Belize branch based on crashniels' source.

Debug and experimental branches are still present, but should not be treated as release kernels:

- `x_old__ps4-linux-5.15.y` and `x_old__ps4-linux-5.15.y-conservative2` contain excessive debug logging from earlier WiFi/MMC investigation work.
- `x_experimental__*` and `x_exp__*` branches are kept for subsystem testing, regression hunting, or archival reference only.
- `x_experimental__5.15.15-fix-baikal` is an example of a non-release test branch and is not considered working.
- `5.15.15-aeolia-belize` intentionally omits parts of the Baikal patchset and is kept for testing only.

<br>

---
## Compile and Build

The current workflow is centered around `build.sh` ("Strawberry Builder") and the consolidated GitHub Actions workflow `.github/workflows/build-kernel_latest.yaml`.

GitHub Actions:

- Run `build-kernel_latest.yaml` from the Actions tab.
- Pick `profile=Server` or `profile=General`.
- Pick `lto=ThinLTO` or `lto=FullLTO`.

Profile summary:

- `Server`: headless/services oriented, `PREEMPT_VOLUNTARY`, performance governor, container/netfilter stack kept enabled.
- `General`: desktop/gaming oriented, full `PREEMPT`, BORE enabled, schedutil/reflex path, cgroup and namespace support enabled, `DMI`/`fw_cfg` sysfs enabled, and netfilter stack stripped.

Local build:
```bash
git clone https://github.com/feeRnt/ps4-linux-12xx --branch 7.0-Clean --depth=3
# Keep a low depth to save space.

cd ps4-linux-12xx

# Fetch required firmware into extra_firmware/ and build with the
# General profile using ThinLTO.
./build.sh --option 3 use=General lto=ThinLTO

# Example: build a Server-profile kernel with FullLTO.
./build.sh --option 3 use=Server lto=FullLTO
```

The builder will:

- move `config` to `.config` automatically if needed;
- fetch every blob listed in `CONFIG_EXTRA_FIRMWARE` into `extra_firmware/`;
- apply the selected profile and LTO settings;
- build `bzImage` with LLVM;
- write outputs to `out/` (`bzImage`, `.config`, `artifact_name.txt`).

If you need a more manual path, you can still do:

```bash
mv config .config
make -j"$(nproc)" LLVM=1 olddefconfig
make -j"$(nproc)" LLVM=1 prepare
make -j"$(nproc)" LLVM=1 bzImage
make -j"$(nproc)" LLVM=1 modules
```

<br>

---
## Releases and Downloads

To get some pre-compiled kernels, go to the [releases section](https://github.com/feeRnt/ps4-linux-12xx/releases), and choose a kernel (bzImage) based on your needed version.

Please read the boldened out and highlighted text, as they contain some information that might be useful for a particular release. It's very wordy, that needs to be fixed!!

<br>

---
## Contributing

If something doesn't work on these kernels, has missing features, or your model still has unsupported WiFi, you can open a GitHub issue to share its details.

Pull requests/code contributions are always welcome.

<br>

---
## Licensing

### Firmware and Drivers Notice

This repository includes non-GPL firmware/cfg blobs under extra_firmware/,

These files (e.g. Marvell and MediaTek firmware) are distributed under
their respective vendor licenses and are not covered by the GPL.

See extra_firmware/README.license for details.


There is an additional Dual BSD 3 & GPL 2 License for the MediaTek wireless driver in wireless/mediatek/mt76x8/**

See drivers/net/wireless/mediatek/mt76x8/README.license for details.

-- The rest of the repository and code is under the same terms as the Linux Kernel, GPLv2, unless noted otherwise --

<br>

---
## Documentation, Guides and the PS4 Linux Future (As of December 2025)

While many of the bugs and issues prevalent in PS4 Linux kernels, and PS4 Linux in general have been fixed over the years, many of them still exist, and are seldom worked on.  
A few honorable mentions aimed at improving this scene go to:

1. Blackscreen/No Display/Unsupported Monitor issue:
    - https://github.com/oberdfr/kernel-ps4linux/tree/ps4-linux-v6.17.1-custom-resolution:  
attempts at using the display EDID information from your monitor, to use in Linux. This aims to improve the various blackscreen issues for monitors that don't support 1080p, or when using a capture card.  
(Work in Progress)

    - https://github.com/ps4gentoo/initramfs &  
    https://github.com/ps4boot/ps4-linux-payloads/  
    Same goal as the last link, but attempts it (successfully) by acquiring the EDID from Orbis (PS4-OS) throught the PS4-Linux Loader, and copies it over to the initramfs.  
    (Work in Progress; latest fix might not've been committed)


2. Mainling the PS4-specific patches and packages (OS-specific):  
    See,
    - https://github.com/Jaguarlinux/
    - https://github.com/centi07/arch-ps4-aur
    - https://github.com/FalsePhilosopher/mesa-docker-ps4


3. General discussion/help:
    - https://ps4linux.com/
    - https://discord.gg/QtcPmzHVVm (PS4-Linux Server Discord)
    - https://discord.gg/jebUjgBu6T (ps4gentoo/ps4boot (mircoho's) Discord)   
4. Random and other links regarding PS4-Linux that were, are, or could be useful:
    - https://github.com/Hakkuraifu/PS4Linux-Documentation (Early documentation on PS4-Linux)
    - https://github.com/Ps3itaTeam/ (Fan control, kernel, graphics drivers etc.)
    - https://github.com/ErkkolaMaitohappo/arch-ps4-aur-smth-fork (Clean Arch Linux on PS4 (2025, Dec))
    - https://github.com/7coil/archlinux-on-ps4 (Arch Linux on PS4,  automated to fetch latest release)
    - https://github.com/Dr4kk3N/dkn-overlay (Gentoo overlay for PS4-Linux)
    - https://github.com/Hakkuraifu/PS4Linux-ArchDrivers (Graphics Drivers; Arch Based)
    - https://github.com/rinsuki/ps4linux-video-drivers (Graphics Drivers; Arch Based)
    - https://github.com/IT-Mania/PS4linux-deb/ (Graphics drivers ; Debian based)
    - https://github.com/DionKill/ps4-video-archlinux (Graphics drivers ; Arch based - 2025, Dec)
    - https://github.com/noob404yt/ (Mediatek drivers, Pop!_OS drivers)
    - https://github.com/TigerClips1/ (Developer of PS4 JaguarLinux)



### For an instructional manual on installation and other topics, refer to this [all-around guide.](https://dionkill.github.io/ps4-linux-tutorial/)

---
<p align="center">Enjoy your Linux-Station!</p>

---
<br>

Generic Linux Kernel Documentation
------

There are several guides for kernel developers and users. These guides can
be rendered in a number of formats, like HTML and PDF. Please read
Documentation/admin-guide/README.rst first.

In order to build the documentation, use ``make htmldocs`` or
``make pdfdocs``.  The formatted documentation can also be read online at:

    https://www.kernel.org/doc/html/latest/

There are various text files in the Documentation/ subdirectory,
several of them using the Restructured Text markup notation.

Please read the Documentation/process/changes.rst file, as it contains the
requirements for building and running the kernel, and information about
the problems which may result by upgrading your kernel.
