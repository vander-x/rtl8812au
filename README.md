# aircrack-ng/rtl8812au — port to kernel 7.2.x (TP-Link Archer T2U Plus / RTL8821AU)

Fork of the [aircrack-ng/rtl8812au](https://github.com/aircrack-ng/rtl8812au) driver
with the fixes needed to build and run on recent kernels (tested on 7.2.6), plus a
handful of security, correctness and hardening improvements on top of that base.

**Hardware tested:** TP-Link Archer T2U Plus (USB ID `2357:0120`, RTL8821AU chipset).
The driver covers the same chip family as the upstream project (RTL8812AU/8821AU/8814AU),
but only this specific model was physically validated.

## Why this fork exists

The project's default branch (`v5.6.4.2`) doesn't build on kernel 7.x: the kernel
removed or renamed several APIs the driver relies on (`EXTRA_CFLAGS`, the timer API,
`strncpy`, `cfg80211` callback signatures). The community PRs addressing this
(#1253, #1264, #1263) cover part of the problem, but none of them fix everything this
particular kernel broke — in particular the full migration from `net_device*` to
`wireless_dev*` across the station-management callbacks (`add_key`, `get_station`,
`add_station`, etc.) and multi-radio wiphy support (`radio_idx`), a kernel feature
merged only from mid-2025 onward.

## Base used

Started from commit [`63cf0b4`](https://github.com/aircrack-ng/rtl8812au/commit/63cf0b4),
the last version the community confirmed as stable for TP-Link AC600-family devices
(issue [#1159](https://github.com/aircrack-ng/rtl8812au/issues/1159),
[#1153](https://github.com/aircrack-ng/rtl8812au/issues/1153)), from before a
regression introduced by commit `c7f8f6e` (later reverted by the project itself in
`41ea46f`, but documented in an outdated way in the official README).

## What was fixed/added

### Kernel 7.x compatibility
- `EXTRA_CFLAGS` → `ccflags-y` in the Makefile (the variable was dropped by recent
  kernel build systems).
- Compatibility shim for `from_timer`/`del_timer_sync`, renamed to
  `timer_container_of`/`timer_delete_sync`.
- `struct pppoe_tag`/`pppoe_hdr`: the flexible-array fields (`tag_data`, `tag`) were
  hidden from kernel code (`#ifndef __KERNEL__`); replaced with manual pointer
  arithmetic in `core/rtw_br_ext.c`.
- `strncpy()` was **removed from the kernel**; replaced with `strscpy()` (string
  copies) or `memcpy()` (fixed-size byte copies, where the behavior never relied on
  NUL-termination anyway).
- 12 `cfg80211` callbacks migrated from `struct net_device *` to
  `struct wireless_dev *` (`add_key`, `get_key`, `del_key`, `set_default_mgmt_key`,
  `get_station`, `add_station`, `del_station`, `change_station`, `dump_station`,
  `remain_on_channel`, plus the 2 `cfg80211_new_sta`/`cfg80211_del_sta` call sites).
- `radio_idx` (multi-radio wiphy) support added to `set_wiphy_params`, `set_tx_power`,
  `get_tx_power` — ignored in practice, since the supported adapters only have one
  radio.
- New `rx_addr` parameter in `remain_on_channel`.

### Monitor mode capture
Without this, monitor mode captures beacon/control frames fine but **zero data
frames** — including the EAPOL handshake needed for WPA2 auditing — because the chip
drops any frame with a CRC/ICV error in hardware before the driver ever sees it (and
that's exactly how every encrypted data frame arrives without the key). Fixed in
`hal/rtl8812a/rtl8812a_hal_init.c` and `hal/rtl8812a/usb/usb_ops_linux.c`:
- Enables `RCR_ACRC32 | RCR_AICV` (accept CRC/ICV-error frames) in the receive
  control register, only while in monitor mode.
- Writes `REG_RXFLTMAP0`/`REG_RXFLTMAP1` (previously only `REG_RXFLTMAP2` was
  written).
- `recvbuf2recvframe()` no longer drops frames with `crc_err`/`icv_err` while the
  interface is in monitor mode.

(Based on community PR [#1266](https://github.com/aircrack-ng/rtl8812au/pull/1266).)

### Non-DFS 5GHz TX unlock
The driver's regulatory table (`os_dep/linux/wifi_regd.c`) marked the **entire** 5GHz
band (36–165) as passive (`NL80211_RRF_PASSIVE_SCAN`/`NO_IR`), blocking any active
transmission — including legitimate injection/deauth testing on your own network.
Split into three rules:

| Sub-band | Channels | Transmit |
|---|---|---|
| UNII-1 | 36, 40, 44, 48 | Unlocked |
| UNII-2/2e (DFS) | 52–144 | **Still locked** (shares spectrum with radar) |
| UNII-3 | 149, 153, 157, 161, 165 | Unlocked |

DFS channels stay protected — including by a second, independent check already in the
driver (`rtw_is_dfs_ch()`), which was left untouched.

**Important for anyone reusing this fork:** this unlock assumes you know what you're
doing and are testing your own equipment or something you're explicitly authorized to
test. It's not a generic "power unlock" — DFS channels stay blocked on purpose, since
using them without radar detection is a real regulatory violation (it can interfere
with weather/aviation radar), not just a licensing technicality.

### TX power validation
`cfg80211_rtw_set_txpower()` only clamped the user's request to a generic 40 dBm
(10 W) ceiling, unrelated to the actual limits declared in the regulatory table
(20 dBm on 2.4GHz, 30 dBm on most of 5GHz). It now queries `freq_reg_info()` for the
current channel and clamps the requested value to the EIRP actually allowed there.

### Security
- **`access_ok(&priv_cmd.buf, ...)` → `access_ok(priv_cmd.buf, ...)`**
  (`os_dep/linux/rtw_android.c`): the code was validating the address of the *kernel
  variable* instead of the userspace pointer it held, defeating the check meant to
  stop a malicious userspace-supplied pointer. (Community PR
  [#1242](https://github.com/aircrack-ng/rtl8812au/pull/1242).)
- **Stack overflow via malformed WPS**: `rtw_get_wps_attr_content()` copied a WPS
  attribute's content into a destination buffer without checking the buffer's real
  size against the declared length (attacker-controlled, from a crafted
  beacon/probe/action frame) — a malicious AP could overflow 1-byte stack variables
  and corrupt the stack canary. Fixed across all 13 call sites in the driver, adding a
  `buf_content_len` parameter and clamping the `memcpy`. (Community PR
  [#1269](https://github.com/aircrack-ng/rtl8812au/pull/1269), reported at
  [lore.kernel.org](https://lore.kernel.org/all/20260901091226.444666-1-sha0@badchecksum.net/).)

### Minor fixes
- `cfg80211_rtw_indicate_connect()`: uses an exact BSSID+SSID match when looking up
  the BSS in `cfg80211`'s list instead of a wildcard, avoiding connecting to the wrong
  entry in scenarios with multiple APs sharing a similar BSSID. Plus a null-check
  before using a beacon's DS Parameter Set IE, which may not be present. (Community PR
  [#1244](https://github.com/aircrack-ng/rtl8812au/pull/1244).)
- Modernized the driver's 4 tasklets (TX, RX, mesh) from `tasklet_init()` (legacy API,
  breaks under CFI/Control Flow Integrity) to `tasklet_setup()` — same softirq
  dispatch semantics, no performance loss.
- Removed ~9,760 lines of dead code (`#if 0 ... #endif`) accumulated over the years,
  using a small purpose-built script (`scripts/strip_dead_if0.py`) that tracks
  `#if`/`#endif` nesting and **skips** any block with a live `#else`/`#elif` branch —
  avoiding the classic mistake of deleting a real definition sitting next to commented-
  out dead code.

### What was tried and reverted
A widening of the USB buffer pools (RX from 8→32, TX from 4→16, meant to improve
desktop throughput) was tried and **reverted** after hardware bisection confirmed it
consistently broke beacon counting/reception — likely by desyncing some queue-depth
expectation between the host and the chip's firmware. There's no known fix; the
original values (8/4) were kept on purpose.

## Building

```bash
make KVER=$(uname -r) KSRC=/lib/modules/$(uname -r)/build
```

## Installing via DKMS

```bash
sudo dkms remove realtek-rtl88xxau/5.6.4.2~20230501 --all 2>/dev/null
sudo rm -rf /usr/src/realtek-rtl88xxau-5.6.4.2~20230501
sudo cp -r . /usr/src/realtek-rtl88xxau-5.6.4.2~20230501
sudo dkms add realtek-rtl88xxau/5.6.4.2~20230501
sudo dkms build realtek-rtl88xxau/5.6.4.2~20230501
sudo dkms install realtek-rtl88xxau/5.6.4.2~20230501
sudo modprobe 88XXau
```

## Validated in practice

- Network, data and control frame capture in monitor mode (multiple networks, 2.4GHz
  and 5GHz).
- Deauth working on 2.4GHz and on non-DFS 5GHz (36-48, 149-165), with confirmed ACKs.
- EAPOL frame reception confirmed via raw capture (`tcpdump` filtering
  `ether proto 0x888e`).
- Stable across a multi-hour test session (one "scan stopped finding networks" case
  was resolved with a clean `rmmod`/`modprobe` — root cause not identified with
  certainty, no evidence it's related to this fork's changes).

## What wasn't tested

- The RTL8812AU and RTL8814AU chips (same codebase, different hardware) — only the
  RTL8821AU (T2U Plus) was physically tested.
- AP mode, P2P/Wi-Fi Direct, mesh.
- Very-long-term stability (days) or sustained heavy network load.
- A complete end-to-end 4-way handshake capture (we confirmed the driver correctly
  receives EAPOL frames, but never closed the loop with a fully captured handshake
  and a successful password crack).

## License

GPLv2, same as the original project.

---

# Original project README (aircrack-ng/rtl8812au)

## RTL8812AU/21AU and RTL8814AU Wireless drivers
Only for use with Linux & Android

[![Monitor mode](https://img.shields.io/badge/monitor%20mode-working-brightgreen.svg)](#)
[![Frame Injection](https://img.shields.io/badge/frame%20injection-working-brightgreen.svg)](#)
[![GitHub version](https://raster.shields.io/badge/version-v5.6.4.2-lightgrey.svg)](#)
[![GitHub issues](https://img.shields.io/github/issues/aircrack-ng/rtl8812au.svg)](https://github.com/aircrack-ng/rtl8812au/issues)
[![GitHub forks](https://img.shields.io/github/forks/aircrack-ng/rtl8812au.svg)](https://github.com/aircrack-ng/rtl8812au/network)
[![GitHub stars](https://img.shields.io/github/stars/aircrack-ng/rtl8812au.svg)](https://github.com/aircrack-ng/rtl8812au/stargazers)
[![Build Status](https://travis-ci.org/aircrack-ng/rtl8812au.svg?branch=v5.6.4.2)](https://travis-ci.org/aircrack-ng/rtl8812au)
[![GitHub license](https://img.shields.io/github/license/aircrack-ng/rtl8812au.svg)](https://github.com/aircrack-ng/rtl8812au/blob/master/LICENSE)
<br>
[![Kali](https://img.shields.io/badge/Kali-supported-blue.svg)](https://www.kali.org)
[![Arch](https://img.shields.io/badge/Arch-supported-blue.svg)](https://www.archlinux.org)
[![Armbian](https://img.shields.io/badge/Armbian-supported-blue.svg)](https://www.armbian.com)
[![ArchLinux](https://img.shields.io/badge/ArchLinux-supported-blue.svg)](https://img.shields.io/badge/ArchLinux-supported-blue.svg)
[![aircrack-ng](https://img.shields.io/badge/aircrack--ng-supported-blue.svg)](https://github.com/aircrack-ng/aircrack-ng)
[![wifite2](https://img.shields.io/badge/wifite2-supported-blue.svg)](https://github.com/kimocoder/wifite2)


### Important!

```
* Use "ip" and "iw" instead of "ifconfig" and "iwconfig"
     It's described further down, READ THE README!
```

### IPERF3 benchmark

**[Device]** Alfa Networks AWUS036ACH<br>
**[Chipset]** 88XXau (rtl8812au)<br>
**[Branch]** v5.6.4.1<br>
**[Distance]** 10m free sight

```
[ ID] Interval           Transfer     Bitrate         Retr  Cwnd
[  5]   0.00-1.00   sec  11.6 MBytes  97.4 Mbits/sec    0   96.2 KBytes
[  5]   1.00-2.00   sec  11.2 MBytes  93.8 Mbits/sec    0    100 KBytes
[  5]   2.00-3.00   sec  11.2 MBytes  93.8 Mbits/sec    0    100 KBytes
[  5]   3.00-4.00   sec  11.2 MBytes  93.8 Mbits/sec    0    100 KBytes
[  5]   4.00-5.00   sec  11.2 MBytes  93.8 Mbits/sec    0    100 KBytes
[  5]   5.00-6.00   sec  11.4 MBytes  95.9 Mbits/sec    0    105 KBytes
[  5]   6.00-7.00   sec  11.2 MBytes  93.8 Mbits/sec    0    105 KBytes
[  5]   7.00-8.00   sec  11.3 MBytes  94.9 Mbits/sec    0    157 KBytes
[  5]   8.00-9.00   sec  11.2 MBytes  93.8 Mbits/sec    0    157 KBytes
[  5]   9.00-10.00  sec  11.2 MBytes  94.3 Mbits/sec    0    157 KBytes
[  5]  10.00-11.00  sec  11.2 MBytes  93.8 Mbits/sec    0    157 KBytes
[  5]  11.00-12.00  sec  11.2 MBytes  93.8 Mbits/sec    0    157 KBytes
[  5]  12.00-13.00  sec  11.2 MBytes  94.4 Mbits/sec    0    157 KBytes
[  5]  13.00-14.00  sec  11.2 MBytes  93.8 Mbits/sec    0    157 KBytes
[  5]  14.00-15.00  sec  11.2 MBytes  94.4 Mbits/sec    0    157 KBytes
[  5]  15.00-16.00  sec  10.9 MBytes  91.7 Mbits/sec    0    157 KBytes
[  5]  16.00-17.00  sec  11.2 MBytes  94.4 Mbits/sec    0    157 KBytes
[  5]  17.00-18.00  sec  11.2 MBytes  94.4 Mbits/sec    0    157 KBytes
[  5]  18.00-19.00  sec  11.2 MBytes  94.4 Mbits/sec    0    157 KBytes
[  5]  19.00-20.00  sec  11.2 MBytes  93.8 Mbits/sec    0    157 KBytes
[  5]  20.00-21.00  sec  11.2 MBytes  93.8 Mbits/sec    0    157 KBytes
[  5]  21.00-22.00  sec  11.2 MBytes  93.8 Mbits/sec    0    157 KBytes
[  5]  22.00-23.00  sec  11.2 MBytes  93.8 Mbits/sec    0    157 KBytes
- - - - - - - - - - - - - - - - - - - - - - - - -
[ ID] Interval           Transfer     Bitrate         Retr
[  5]   0.00-23.15  sec   260 MBytes  94.2 Mbits/sec    0             sender
[  5]   0.00-23.15  sec  0.00 Bytes  0.00 bits/sec                  receiver
```

### DKMS
This driver can be installed using [DKMS]. This is a system which will automatically recompile and install a kernel module when a new kernel gets installed or updated. To make use of DKMS, install the `dkms` package, which on Debian (based) systems is done like this:
```
$ sudo apt-get install dkms
```

### Download
```
$ git clone -b v5.6.4.2 https://github.com/aircrack-ng/rtl8812au.git
cd rtl*
```

### Installation of Driver
In order to install the driver open a terminal in the directory with the source code and execute the following command:
```
$ sudo make dkms_install
```

### Removal of Driver
In order to remove the driver from your system open a terminal in the directory with the source code and execute the following command:
```
$ sudo make dkms_remove
```

### Make
For building & installing the driver with 'make' use
```
$ make && make install
```

Package / Build dependencies (Kali)
```
$ sudo apt-get update
$ sudo apt-get install bc mokutil build-essential libelf-dev linux-headers-`uname -r`
```
#### For Raspberry (RPI)

```
$ sudo apt-get install raspberrypi-kernel-headers
```

Then change the platform in Makefile to 32-bit `ARM` architecture (RPi 1/2/3/ & 0/Zero):
```
$ sed -i 's/CONFIG_PLATFORM_I386_PC = y/CONFIG_PLATFORM_I386_PC = n/g' Makefile
$ sed -i 's/CONFIG_PLATFORM_ARM_RPI = n/CONFIG_PLATFORM_ARM_RPI = y/g' Makefile
```

Or, for `ARM64` (RPI 3B+, 4B and Zero2) you will need to run:
```
$ sed -i 's/CONFIG_PLATFORM_I386_PC = y/CONFIG_PLATFORM_I386_PC = n/g' Makefile
$ sed -i 's/CONFIG_PLATFORM_ARM64_RPI = n/CONFIG_PLATFORM_ARM64_RPI = y/g' Makefile
```

In addition, if you receive an error message about `unrecognized command line option ‘-mgeneral-regs-only’` (i.e., Raspbian Buster), you will need to run the following commands for `ARM` architecture, then retry building and installing:
```
$ export ARCH=arm
$ sed -i 's/^MAKE="/MAKE="ARCH=arm\ /' dkms.conf
```

Or, for `ARM64` run the following before re-building:

```
$ export ARCH=arm64
$ sed -i 's/^MAKE="/MAKE="ARCH=arm64\ /' dkms.conf
```

Building the driver may exceed RAM on some RPi's resulting in a `gcc: fatal error: Killed signal terminated program cc1` error.  Swap space can be increased in `/etc/dphys-swapfile` e.g. to `2000` megabytes, followed by `/etc/init.d/dphys-swapfile restart`. Building on swap is very slow, however.

For setting monitor mode
  1. Fix problematic interference in monitor mode.
  ```
  $ airmon-ng check kill
  ```
  You may also uncheck the box "Automatically connect to this network when it is avaiable" in nm-connection-editor. This only works if you have a saved wifi connection.

  2. Set interface down
  ```
  $ sudo ip link set wlan0 down
  ```
  3. Set monitor mode
  ```
  $ sudo iw dev wlan0 set type monitor
  ```
  4. Set interface up
  ```
  $ sudo ip link set wlan0 up
  ```
For setting TX power
```
$ sudo iw wlan0 set txpower fixed 3000
```

### LED control

#### statically by module parameter in /etc/modprobe.d/8812au.conf or wherever, for example:

```sh
options 88XXau rtw_led_ctrl=0
```
value can be 0 or 1

#### or dynamically by writing to /proc/net/rtl8812au/$(your interface name)/led_ctrl, for example:

```sh
$ echo "0" > /proc/net/rtl8812au/$(your interface name)/led_ctrl
```
value can be 0 or 1

#### check current value:

```sh
$ cat /proc/net/rtl8812au/$(your interface name)/led_ctrl
```

### USB Mode Switch

0: doesn't switch, 1: switch from usb2.0 to usb 3.0 2: switch from usb3.0 to usb 2.0
```sh
$ rmmod 88XXau
$ modprobe 88XXau rtw_switch_usb_mode=int (0: no switch 1: switch from usb2 to usb3 2: switch from usb3 to usb2)
```

### NetworkManager

Newer versions of NetworkManager switches to random MAC address. Some users would prefer to use a fixed address.
Simply add these lines below
```
[device]
wifi.scan-rand-mac-address=no
```
at the end of file /etc/NetworkManager/NetworkManager.conf and restart NetworkManager with the command:
```
$ sudo service NetworkManager restart
```

### Credits / Contributors

```
Alfa Networks - https://www.alfa.com.tw/
Realtek.      - https://www.realtek.com
aircrack-ng   - https://www.aircrack-ng.org

astsam        - https://github.com/astsam
evilphish     - https://github.com/evilphish
fariouche     - https://github.com/fariouche
CGarces       - https://github.com/CGarces
ZerBea        - https://github.com/ZerBea
lwfinger      - https://github.com/lwfinger
Ulli-Kroll.   - https://github.com/Ulli-Kroll

```
