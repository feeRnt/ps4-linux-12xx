
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

This fork aimed to make the internal WiFi+Bluetooth modules on specific PlayStation models with the Marvell    
88w8897 combo card (internal codename Torus 2) functional, as they typically error out on the default kernels.

Over time, I also managed to fix the common blackscreen at GUI login issue on newer kernels, and added
support for various miscellaneous components such as the MT7668 WiFi and BT chip on certain consoles.    
The branch names are meant to be descriptive and provide an idea, but they're far from perfect!

Merging all the main fixes into a few distinct branches is a WIP.


-------

While the CUH-1216/1215 models are definitively known to have the Torus 2 models with probelmatic WiFi, along with some 11xx models with similar WiFi issues, here is a list of consoles reported working without problem from the kernels in this repo:

| Console Model | Variation | WiFi+BT Chip Present | Compatible Kernel (Patched) |
|---|---|---| --- |
| CUH-1216(A/B) | Phat - Belize B0 | 88w8897 (Torus 2) | *6.15.4, 5.15.15* |
| CUH-1215(A/B) | Phat - Belize | 88w8897 (Torus 2) | *6.15.4, 5.15.15*  |
CUH-1003  | Phat  - Aeolia | ? | *6.15.4* |
CUH-1116A | Phat - Aeolia | ? | *6.15.4* |
CUH-2215B | Pro - Baikal | ? | *5.4.247* |
CUH-2216A | Slim - Baikal B1 | MediaTek 7668 | *5.4.247* |
CUH-2216A | Slim - Belize | MediaTek 7668 | *5.15.15* |
CUH-7116B | Pro - Baikal B1 | ? | *5.4.247* |
CUH-7202B | Pro - Baikal | ? | *5.4.247* |
```
[A and B are just hard-drive specification: 500 GB vs 1000GB].

Aeolia, Belize and Baikal are the console Southbridges.
B0, B1 etc. are the Southbridge subrevisions.
```

<br>

- TODO: Add a list with all supported console models, their southbridges, and their compatible kernels.

---- 
<br>

The main patches which in combination fix the module are:     
[150 MHz rate limit quirk on the 88w8897 card's Function 0](https://github.com/feeRnt/ps4-linux-12xx/commit/df7f7dbb1b0fff6026e159540f029988c8067b70).

relying on the [patch for added sdio_id for the Function 0](https://github.com/feeRnt/ps4-linux-12xx/commit/f4835fb020010acff2b70e4c5fa9430e07f0073b),

Then a [few SDHCI Host quirks for the PlayStation SDHCI host](https://github.com/feeRnt/ps4-linux-12xx/commit/e6f342df7737722d5e27f0ae3974e493c5fe4ca4),

additionally with [extra retries for MMC CMD 52 or 53, which it would usually fail on](https://github.com/feeRnt/ps4-linux-12xx/commit/c57162e5ec7a4aa3af3310a36dc963b5c0298dfe).

The primary culprit behind the failed SDIO initialization, seems to be that the card doesn't properly support 208 or 200 MHz clock rate on this PS4 SDHCI host, causing the card to show tuning and other command failures.    
Through much trial and error, I was able to reach such an arcane fix,     
<br>
![Many of the kernels I had to compile and test before finally landing
on the fix kernel](./IMAGES/github_pic2.png)

and here is an image with working internal WiFi and Bluetooth as shown in the logs, on an Arch Linux system running on my CUH-1216 console:    
<br>
![Working WiFi image](./IMAGES/github_pic1.png)
<br>

Hard work paid off!

----
<br>
There are different branches that you can select on the repo,    
`ps4-linux-5.15.y` and `ps4-linux-5.15.y-conservative2` are branches with excessive debug logs, that helped me pinpoint the issue on the whole MMC stack. Due to the logging, it is not advisable to use those kernel branches.    

The `ps4-linux-5.15.y-fix` is a branch without the PS4 patches from codedwrench's Baikal branch. It still
runs as intended (no blackscreen fix yet), but you will get bad errors, even on Aeolia/Belize consoles. Should be used for testing only. 

However, it probably runs on Aeolia consoles unlike the \*-belize branches. (Not tested yet)


The main release branches are:    
- `ps4-linux-5.15.15-fix-belize` : The clean WiFi fix branch for Kernel version 5.15.15 on Belize southbridges.    

- ~~`ps4-linux-5.15.15-fix-baikal` : The clean WiFi fix branch for Kernel version 5.15.15 on Baikal southbridges.~~
~~Baikals don't have the WiFi/BT issue fixed here, but it's kept for testing only.~~


- `ps4-linux-5.15.189-fix-belize` : The clean WiFi fix branch for Kernel version 5.15.189 on Belize southbridges.

- `ps4-linux-5.4.247-baikal-dfaus`: A branch for version 5.4.247 with fixed blackscreen and MT7668 support for Baikal southbridges.    
Based on DFAUS' source.

- `ps4-linux-6.15.y-crashniels` : The clean WiFi fix branch for Kernel version 6.15.4, on Aeolia/Belize southbridges.    
Based on crashniels' source.

To compile them, you can simply fork the repo, go to the Actions tab and run the Workflow file for `build-kernel_latest.yaml` for a particular branch.

Or if you would like to build locally, just clone the repo for your desired branch and run the necessary commands:
```bash

git clone https://github.com/feeRnt/ps4-linux-12xx --branch <desired-branch-name> --depth=3
#keep a low depth to save on space

cd ps4-linux-12xx
echo "Copying necessary extra firmware to /lib/firmware"
sudo cp -ri extra_firmware/* /lib/firmware

sudo mkdir /lib/firmware/mrvl
wget -nc https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mrvl/sd8897_uapsta.bin \
&& sudo mv -i sd8897_uapsta.bin /lib/firmware/mrvl

wget -nc https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mrvl/sd8797_uapsta.bin \
&& sudo mv -i sd8797_uapsta.bin /lib/firmware/mrvl


sudo mkdir /lib/firmware/mediatek
wget -nc https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mediatek/mt7668pr2h.bin \
&& sudo mv -i mt7668pr2h.bin /lib/firmware/mediatek


## Rename .config file

mv config .config

export MAKE_OPTS="-j`nproc` \
              HOSTCC=gcc-11 \
              CC=gcc-11"
# gcc-11 is ideal for compiling the 5.15.y kernels
# Otherwise you will have many typecheck and compile errors
make ${MAKE_OPTS} olddefconfig
make ${MAKE_OPTS} prepare
echo "making kernel. . ."
make ${MAKE_OPTS}
echo "making modules . . ."
make ${MAKE_OPTS} modules
echo "installing kernel . . ."
make ${MAKE_OPTS} install
echo "installing modules . . ."
make ${MAKE_OPTS} modules_install
```

To get some pre-compiled kernels, go to the [releases section](https://github.com/feeRnt/ps4-linux-12xx/releases), and choose a kernel (bzImage) based on your needed version.

If something doesn't work, or your model still has unsupported WiFi, you can open a GitHub issue to share its details.

<br>
Enjoy your Linux-Station!

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
