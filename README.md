# redOS-oelinux

This builds the OS, the /anki programs (`redOS`), and puts it all into a final OTA.

## Submodules

- /poky/poky -> [yoctoproject/poky](https://github.com/yoctoproject/poky) (walnascar)
- /poky/meta-openembedded -> [openembedded/meta-openembedded](https://github.com/openembedded/meta-openembedded) (walnascar)
- /anki/victor -> [redOS](https://github.com/purpl-org/redOS) (main)
  - Where all the personality code lives - the README there has more info
- /anki/wired -> [wired](https://github.com/os-vector/wired) (main)
  - Little webserver with configuration options

## Prebuilt OTA:

redOS is in the dropdown box in [https://devsetup.froggitti.net/](https://devsetup.froggitti.net/). Put your unlocked bot into recovery mode (hold the button for 15 seconds on the charger), head to the site, choose redOS-oelinux, then go through the process.

## Build

- Note: you will need a somewhat beefy **x86_64 Linux** machine with at least 16GB of RAM and 100GB of free space.

1. [Install Docker](https://docs.docker.com/engine/install/), git, and wget.

2. Configure Docker so a regular user can use it:

```
sudo groupadd docker
sudo gpasswd -a $USER docker
newgrp docker
sudo chown root:docker /var/run/docker.sock
sudo chmod 660 /var/run/docker.sock
```

3. Clone and build:

```
git clone https://github.com/purpl-org/wire-os --recurse-submodules
cd wire-os
./build/build.sh -bt <dev/oskr> -bp <boot-passwd> -v <build-increment>
# boot password not required for dev
# example: ./build/build.sh -bt dev -v 1
# <build-increment> is what the last number of the version string will be - if it's 1, it will be 0.9.0.1.ota
```

### Where is my OTA?

`./_build/0.9.0.1.ota`

## Development path

- **Most work should be done in `redOS`. Generally, that's all you need to have cloned. That can be worked on on a less beefy Linux laptop or M-series MacBook. If you have a modern base redOS-oelinux OTA installed; you can clone `redOS`, make changes, build that standalone, and deploy that to your robot. This repo is more meant to be cloned to a build server, and built less often.**

##  Donate

If you want to :P

[![Buy Me A Coffee](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://buymeacoffee.com/kercre123)

## Differences compared to normal Vector FW

-   New OS base
    -   Yocto Walnascar rather than Jethro
        -   glibc 2.41 (latest as of 07-2025)
-   `victor` software compiled with Clang 18.1.8 rather than 5.0.1
    -	The code is properly fixed so there are no compile warnings
-   Rainbow eye color
    -   Can be activated in :8888/demo.html
-   Some Anki-era PRs have been merged
    -   Performances
        -   He will somewhat randomly do loosepixel and binaryeyes
    -   Better camera gamma correction
        -   He handles too-bright situations much better now
-   Picovoice Porcupine (1.5) wakeword engine
    -   Custom wake words in :8080 webserver!
-   `htop` and `rsync` are embedded
-   Python 3.13 rather than Python 2
-   General bug fixes - for instance, now he won't read the EMR partition upon every single screen draw (DDL bug)
-   :8080 webserver for configuring things I don't want to integrate into a normal app
-   Cat and dog detection (basic, similar to Cozmo)
-   Smaller OTA size - a dev OTA is 171M somehow
-   New Anki boot animation, new pre-boot-anim splash screen, rainbow backpack light animations
-   TensorFlow Lite has been updated to v2.19.0 (latest as of 07-2025)
	-  This means we can maybe leverage the GPU delegate at some point
	-  XNNPACK - the CPU delegate - is faster than what was there before
-   OpenCV has been updated to 4.12.0 (latest as of 07-2025)
  	-  Much better SDK streaming performance
-   [Face overlays](https://www.reddit.com/r/AnkiVector/comments/1lteb3m/_/?utm_source=share&utm_medium=web3x&utm_name=web3xcss&utm_term=1&utm_content=share_button)
        -  How to activate: [WireOS PR #17](https://github.com/os-vector/wire-os/pull/17)
-   Global SSH key: ([ssh_root_key](https://raw.githubusercontent.com/kercre123/unlocking-vector/refs/heads/main/ssh_root_key))

## Helpful scripts

-	`anki-debug`
	-	If you are debugging `victor` and want to see backtraces in /var/log/messages, run this to enable those.
-	`ddn [on/off]`
	-	Turns on/off DevDoNothing, which makes the bot stand still until shaken.
-	`reonboard`
	-	Puts him back into onboarding mode without fully clearing user data
-	`vmesg [-c|-t] <grep args>`
	-	A wrapper for cat/tail /var/log/messages:
-	`temper`
	-	Simple script which tells you CPU temps

```
usage: vmesg [-t|-c] <grep args>
this is a helper tool for viewing Vector's /var/log/messages
if no grep args are provided, the tailed/whole log will be given
-t = tail (-f), -c = cat
example for searching: vmesg -t -i "tflite\|gpu"
example for whole log: vmesg -c
```

## Proprietary software notes

-	This repo contains lots of proprietary Qualcomm code and prebuilt software.
-	After a stupid amount of work, I have most HAL programs compiling with Yocto's GCC 14. It wasn't terribly difficult since it's generally all autotools, but some jank is still involved, and it was still time-consuming.
-	The camera programs and *some* of the BLE programs are being copied in rather than compiled.
	-	Why not compile camera programs? Because I would have to add 2GB to the repo and figure out how to use the weird Qualcomm-specific toolchain.
	-	Why not compile those BLE programs? `ankibluetoothd` and `hci_qcomm_init` are able to compile under GCC 14, but there is some weird low-level issue which makes them unable to properly communicate with a BLE library. So, for now, I am just copying pre-compiled ones in. I will probably try to fix this at some point.

## How this upgrade was done

-	Much work upgrading Yocto recipes.
-	All of the software is compiling with Yocto's GCC 14 or the Clang 18.1.8 vicos-sdk toolchain, with a couple of tiny exceptions.
-	Some recipes are still somewhat old - these include wpa_supplicant and connman (I had issues with SAE - he's able to recognize SAE networks, but his WLAN driver and kernel don't know how to actually connect to it, and I was unable to disable it in modern wpa_supplicant and connman)
