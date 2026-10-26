# aircrack-ng/rtl8812au — port para kernel 7.2.x (TP-Link Archer T2U Plus / RTL8821AU)

Fork do driver [aircrack-ng/rtl8812au](https://github.com/aircrack-ng/rtl8812au) com as
correções necessárias para compilar e funcionar em kernels recentes (testado no 7.2.6),
mais algumas melhorias de segurança, correção e hardening aplicadas sobre a base.

**Hardware testado:** TP-Link Archer T2U Plus (USB ID `2357:0120`, chipset RTL8821AU).
O driver cobre a mesma família de chips do projeto original (RTL8812AU/8821AU/8814AU),
mas só este modelo foi validado fisicamente.

## Por que esse fork existe

A branch padrão do projeto (`v5.6.4.2`) não compila em kernels 7.x: o kernel removeu ou
renomeou várias APIs que o driver usa (`EXTRA_CFLAGS`, API de timer, `strncpy`,
assinaturas do `cfg80211`). Os PRs da comunidade que tratam disso (#1253, #1264, #1263)
cobrem parte do problema, mas nenhum resolve tudo que esse kernel específico quebrou —
em especial a migração completa de `net_device*` para `wireless_dev*` nos callbacks de
estação (`add_key`, `get_station`, `add_station` etc.) e o suporte a múltiplos rádios
por wiphy (`radio_idx`), um recurso do kernel mesclado só a partir de meados de 2025.

## Base utilizada

Partiu do commit [`63cf0b4`](https://github.com/aircrack-ng/rtl8812au/commit/63cf0b4),
a última versão confirmada pela comunidade como estável para dispositivos da família
TP-Link AC600 (issue [#1159](https://github.com/aircrack-ng/rtl8812au/issues/1159),
[#1153](https://github.com/aircrack-ng/rtl8812au/issues/1153)) antes de uma regressão
introduzida pelo commit `c7f8f6e` (posteriormente revertida pelo próprio projeto em
`41ea46f`, mas documentada de forma desatualizada no README oficial).

## O que foi corrigido/adicionado

### Compatibilidade com kernel 7.x
- `EXTRA_CFLAGS` → `ccflags-y` no Makefile (variável removida pelos kernels recentes).
- Shim de compatibilidade para `from_timer`/`del_timer_sync`, renomeados para
  `timer_container_of`/`timer_delete_sync`.
- `struct pppoe_tag`/`pppoe_hdr`: os campos de array flexível (`tag_data`, `tag`) foram
  escondidos do código do kernel (`#ifndef __KERNEL__`); substituído por aritmética de
  ponteiro manual em `core/rtw_br_ext.c`.
- `strncpy()` foi **removido do kernel**; substituído por `strscpy()` (cópias de string)
  ou `memcpy()` (cópias de bytes de tamanho exato, onde o comportamento já não dependia
  de NUL-termination).
- 12 callbacks do `cfg80211` migrados de `struct net_device *` para
  `struct wireless_dev *` (`add_key`, `get_key`, `del_key`, `set_default_mgmt_key`,
  `get_station`, `add_station`, `del_station`, `change_station`, `dump_station`,
  `remain_on_channel`, mais os 2 call-sites de `cfg80211_new_sta`/`cfg80211_del_sta`).
- Suporte a `radio_idx` (multi-radio wiphy) em `set_wiphy_params`, `set_tx_power`,
  `get_tx_power` — ignorado na prática, já que os adaptadores suportados têm um rádio só.
- Novo parâmetro `rx_addr` em `remain_on_channel`.

### Captura em modo monitor
Sem isso, o modo monitor captura beacons/controle normalmente, mas **nenhum frame de
dados** — incluindo o handshake EAPOL necessário pra auditoria WPA2 — porque o chip
descarta no hardware qualquer frame com erro de CRC/ICV antes do driver vê-lo (e é
assim que todo frame de dados criptografado chega, sem a chave). Corrigido em
`hal/rtl8812a/rtl8812a_hal_init.c` e `hal/rtl8812a/usb/usb_ops_linux.c`:
- Habilita `RCR_ACRC32 | RCR_AICV` (aceitar frames com erro de CRC/ICV) no registrador
  de controle de recepção, só quando em modo monitor.
- Escreve `REG_RXFLTMAP0`/`REG_RXFLTMAP1` (antes só `REG_RXFLTMAP2` era escrito).
- `recvbuf2recvframe()` não descarta mais frames com `crc_err`/`icv_err` quando a
  interface está em modo monitor.

(Baseado no PR comunitário [#1266](https://github.com/aircrack-ng/rtl8812au/pull/1266).)

### Liberação de TX em 5GHz não-DFS
A tabela regulatória do driver (`os_dep/linux/wifi_regd.c`) marcava **toda** a faixa de
5GHz (36–165) como passiva (`NL80211_RRF_PASSIVE_SCAN`/`NO_IR`), bloqueando qualquer
transmissão ativa — incluindo testes de injeção/deauth legítimos na própria rede.
Dividida em três regras:

| Sub-banda | Canais | Transmissão |
|---|---|---|
| UNII-1 | 36, 40, 44, 48 | Liberada |
| UNII-2/2e (DFS) | 52–144 | **Mantida travada** (compartilha espectro com radar) |
| UNII-3 | 149, 153, 157, 161, 165 | Liberada |

Os canais DFS continuam protegidos — inclusive por uma segunda checagem independente
já existente no driver (`rtw_is_dfs_ch()`), que não foi tocada.

**Importante para quem for reusar este fork:** essa liberação pressupõe que você sabe
o que está fazendo e está testando equipamento próprio ou com autorização explícita.
Não é um "desbloqueio de potência" genérico — os canais DFS continuam bloqueados de
propósito, pois usá-los sem detecção de radar é uma infração regulatória real (pode
interferir com radar meteorológico/de aviação), não só uma questão de licenciamento.

### Validação de potência TX
`cfg80211_rtw_set_txpower()` só limitava o pedido do usuário a um teto genérico de
40 dBm (10 W), sem relação com os limites reais declarados na tabela regulatória
(20 dBm em 2.4GHz, 30 dBm na maior parte do 5GHz). Agora consulta `freq_reg_info()`
para o canal atual e satura o valor pedido ao EIRP realmente permitido ali.

### Segurança
- **`access_ok(&priv_cmd.buf, ...)` → `access_ok(priv_cmd.buf, ...)`**
  (`os_dep/linux/rtw_android.c`): o código validava o endereço da *variável do kernel*
  em vez do ponteiro de userspace que ela continha, anulando a checagem que deveria
  impedir um ponteiro malicioso vindo do userspace. (PR comunitário
  [#1242](https://github.com/aircrack-ng/rtl8812au/pull/1242).)
- **Estouro de pilha via WPS malformado**: `rtw_get_wps_attr_content()` copiava o
  conteúdo de um atributo WPS para um buffer de destino sem checar o tamanho real do
  buffer contra o tamanho declarado (controlado pelo remetente de um beacon/probe/ação
  craftado) — um AP malicioso podia estourar variáveis de 1 byte na pilha e corromper
  o stack canary. Corrigido em 13 pontos de chamada no driver inteiro, adicionando um
  parâmetro `buf_content_len` e saturando o `memcpy`. (PR comunitário
  [#1269](https://github.com/aircrack-ng/rtl8812au/pull/1269), reportado em
  [lore.kernel.org](https://lore.kernel.org/all/20260901091226.444666-1-sha0@badchecksum.net/).)

### Correções menores
- `cfg80211_rtw_indicate_connect()`: usa match exato de BSSID+SSID ao procurar o BSS na
  lista do `cfg80211` em vez de wildcard, evitando conectar a uma entrada errada em
  cenários com múltiplos APs de BSSID parecido. Mais um null-check antes de usar o DS
  Parameter Set IE de um beacon que pode não estar presente. (PR comunitário
  [#1244](https://github.com/aircrack-ng/rtl8812au/pull/1244).)
- Modernização dos 4 tasklets do driver (TX, RX, mesh) de `tasklet_init()` (API legada,
  quebra sob CFI/Control Flow Integrity) para `tasklet_setup()` — mesma semântica de
  despacho em softirq, sem perda de desempenho.
- Remoção de ~9.760 linhas de código morto (`#if 0 ... #endif`) acumulado ao longo dos
  anos, usando um script próprio (`scripts/strip_dead_if0.py`) que rastreia aninhamento
  de `#if`/`#endif` e **pula** qualquer bloco com `#else`/`#elif` ativo — evitando o erro
  clássico de apagar uma definição real junto com código morto comentado perto dela.

### O que foi tentado e revertido
Um aumento nos pools de buffer USB (RX de 8→32, TX de 4→16, pensado pra throughput em
desktop) foi testado e **revertido** depois de confirmado, via bisecção em hardware
real, que quebrava a contagem/recepção de beacon de forma consistente — provavelmente
por desalinhar alguma expectativa de profundidade de fila entre o host e o firmware do
chip. Não há correção conhecida; os valores originais (8/4) foram mantidos de propósito.

## Como compilar

```bash
make KVER=$(uname -r) KSRC=/lib/modules/$(uname -r)/build
```

## Como instalar via DKMS

```bash
sudo dkms remove realtek-rtl88xxau/5.6.4.2~20230501 --all 2>/dev/null
sudo rm -rf /usr/src/realtek-rtl88xxau-5.6.4.2~20230501
sudo cp -r . /usr/src/realtek-rtl88xxau-5.6.4.2~20230501
sudo dkms add realtek-rtl88xxau/5.6.4.2~20230501
sudo dkms build realtek-rtl88xxau/5.6.4.2~20230501
sudo dkms install realtek-rtl88xxau/5.6.4.2~20230501
sudo modprobe 88XXau
```

## Validado na prática

- Captura de rede, dados e controle em modo monitor (múltiplas redes, 2.4GHz e 5GHz).
- Deauth funcional em 2.4GHz e em 5GHz não-DFS (36-48, 149-165), com ACK confirmado.
- Recepção de frames EAPOL confirmada via captura crua (`tcpdump` filtrando
  `ether proto 0x888e`).
- Estável ao longo de uma sessão de testes de várias horas (um caso de "scan parou de
  achar rede" foi resolvido com `rmmod`/`modprobe` limpo — causa raiz não identificada
  com certeza, sem evidência de estar ligada às mudanças deste fork).

## O que não foi testado

- Os chips RTL8812AU e RTL8814AU (mesma base de código, hardware diferente) — só o
  RTL8821AU (T2U Plus) foi testado fisicamente.
- Modo AP, P2P/Wi-Fi Direct, mesh.
- Estabilidade de longuíssimo prazo (dias) ou sob carga de rede pesada sustentada.
- Captura completa de um handshake de 4 vias do início ao fim (confirmamos que o driver
  recebe frames EAPOL corretamente, mas não fechamos o ciclo com uma captura 100%
  completa e uma quebra de senha bem-sucedida).

## Licença

GPLv2, igual ao projeto original.

---

# README original do projeto (aircrack-ng/rtl8812au)

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
