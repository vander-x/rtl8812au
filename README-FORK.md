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
