[한국어](README.md) | **English**

# ESP32-S31 1G Ethernet iperf Test Firmware

Firmware for checking the Gigabit Ethernet port of the ESP32-S31-Function-CoreBoard-1.
It is based on `examples/ethernet/iperf` from ESP-IDF v6.1.

## Web Flasher

**<https://firepooh.github.io/esp32s31-eth1g-iperf/>**

1. Open it in desktop Chrome or Edge.
2. Connect the board's USB-UART Type-C port to the PC.
3. Pick a version, then click **Connect** → **Flash**.

Binaries and esptool commands are on the [Releases](https://github.com/firepooh/esp32s31-eth1g-iperf/releases) page.

## Board

![ESP32-S31-Function-CoreBoard-1 component layout](docs/images/esp32-s31-function-coreboard-1.webp)

| Item | Details |
|---|---|
| Board | [ESP32-S31-Function-CoreBoard-1](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp32-s31-function-coreboard-1/index.html) |
| Module | ESP32-S31-WROOM-3 (16 MB flash, 16 MB PSRAM) |
| Chip | ESP32-S31 rev v0.0 |
| Ethernet | RJ45, 10/100/1000 Mbps auto-negotiation |
| Development environment | ESP-IDF v6.1, Windows 11 |

Ports and buttons used by this project. Numbers refer to the photo above.

| No. | Silkscreen | Use |
|---|---|---|
| (6) | USB-UART | Console (REPL and logs) and flashing. Shows up as a COM port on the PC (e.g. COM20). |
| (5) | USB-DBG | USB Serial/JTAG. Mirrors the logs, output only. |
| (2) | RJ45 | Ethernet port under test |
| (14), (16) | RST, BOOT | Manual reset, enter download mode |

## Goals

1. Confirm that the board actually establishes a 1000 Mbps link with its link partner.
2. Measure maximum TCP and UDP throughput in both directions with iperf.
3. If the link does not come up at 1G, find out why.
   PHY registers tell whether the link partner lacks 1G support or the cable is faulty.

## Status

- [x] Build, flash and boot
- [x] YT8531 PHY detected (MDIO address 0, ID 0x4F51_E91B)
- [x] 100M link (100 Mbps full duplex, 2026-11-10)
- [x] 100M TCP/UDP throughput (line rate reached in all four directions, [results](#100m-ethernet-results-2026-11-10))
- [ ] 1G link (needs a link partner that supports 1000BASE-T)
- [ ] 1G TCP/UDP throughput

## Hardware

| Item | Value |
|---|---|
| MAC | ESP32-S31 internal EMAC, RGMII |
| PHY | Motorcomm YT8531 (MDIO address 0, auto-detected at boot) |
| SMI | MDC GPIO5, MDIO GPIO6 |
| PHY reset | GPIO7 |
| TX | TXC GPIO13, TX_CTL GPIO12, TXD0~3 GPIO8~11 |
| RX | RXC GPIO14, RX_CTL GPIO15, RXD0~3 GPIO19, 18, 17, 16 |
| Console | UART0 (GPIO58, 59), USB-UART Type-C port, 115200 bps |
| Secondary log | USB Serial/JTAG Type-C port (output only) |

The pin map is the ESP32-S31 default in IDF v6.1 (`ETH_ESP32_EMAC_DEFAULT_CONFIG`).
The PHY runs on the generic IEEE 802.3 driver.
Two YT8531-specific settings are applied on top of it:

- Re-enable auto-negotiation, which the YT8531 turns off after a PHY reset.
- Set the RGMII TX/RX clock delays to about 2 ns.

### YT8531 PHY Circuit

![YT8531 PHY schematic](docs/images/yt8531-phy-schematic.png)

Key points from the schematic:

- The clock comes from a 25 MHz crystal (Y2). CLKOUT (pin 35) is not connected.
- RX signals (RX_CLK, RX_CTL, RXD0~3) have 33 Ω series resistors (R46~R51) near the MAC.
- TX signals (TX_CLK, TX_CTL, TXD0~3) have 33 Ω series resistors (R55~R62) near the PHY.
- MDIO is pulled up to ETH_PHY_3V3 through 4.7 kΩ (R66).
- INT_N is pulled up through 4.7 kΩ (R40) and routed to the ETH_INTN net.

The strap pins are shared with the RGMII RX pins and the LED pins.
The strap resistors on the RX pins (R53, R54, R57, R60, R63, R65) are not populated (NC).
These pins therefore take the chip's internal default values.

| Strap | Pin | Setting | Result |
|---|---|---|---|
| PHYAD0~2 | RXD3, RX_CLK, RX_CTL | Internal pull-down (R53, R54, R57 NC) | PHY address 0 |
| RXDLY | RXD0 | Internal pull-up (R60 NC) | RX_CLK delay added (2 ns at 125 MHz) |
| TXDLY | RXD1 | Internal pull-down (R63 NC) | No TX_CLK strap delay |
| PLLOFF | RXD2 | Internal pull-down (R65 NC) | PLL stays on during sleep |
| CFG_EXT | LED0 (pin 32) | 4.7 kΩ pull-up (R38) | I/O supply provided externally |
| CFG_LDO[1:0] | LED2, LED1 (pins 34, 33) | 4.7 kΩ pull-down (R35, R36) | 3.3 V I/O |

The TXDLY strap alone does not add a TX delay.
The firmware therefore sets the TX delay (about 1.95 ns) through the extended registers.
It also sets the RX delay again to the same value (about 2 ns).

## Build and Flash

Run in PowerShell.
Connect the USB-UART Type-C port to the PC and note its COM port number.
If the `ESPPORT` environment variable points to another port, it conflicts with the `-p` option.
Set `ESPPORT` directly instead.

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
$env:ESPPORT = "COM20"
idf.py --preview build flash monitor
```

- ESP32-S31 is still a preview target, so `--preview` is required.
- The DTR/RTS auto-reset circuit on the USB-UART port enters download mode. No need to press BOOT.
- To move the console back to the USB Serial/JTAG port, set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in `sdkconfig.defaults`.
- After editing `sdkconfig.defaults`, delete the existing `sdkconfig` and rebuild. Otherwise the new defaults are not applied.
- Release builds use the `sdkconfig` in the repository. Commit the regenerated `sdkconfig` as well.

### Release Build

Pushing a `v*` tag makes GitHub Actions build the firmware and update the release and the web flasher.

- Builds in the `espressif/idf:v6.1` Docker image.
- The release gets a merged image (`-merged.bin`), a bundle of individual files (`-all.zip`) and `SHA256SUMS.txt`.
- Tags containing `-` (e.g. `v1.1.0-rc1`) become pre-releases.
- `dependencies.lock` pins the component versions.
- Running "Release build" manually from the Actions tab only keeps the build output as an artifact, without a release.

## Console Commands

| Command | Description |
|---|---|
| `ethernet info` | Link state, MAC speed, actual PHY speed, IP |
| `ethernet phy` | PHY register dump and decode (1000BASE-T status, idle error count) |
| `ethernet speed auto` | Advertise 1000/100/10 and renegotiate |
| `ethernet speed 100` | Advertise 100M and below only, then renegotiate |
| `ethernet ip <addr> <mask> [gw]` | Set a static IP |
| `ethernet dhcp` | Switch to DHCP |
| `iperf ...` | Throughput test (iperf 2.x compatible) |
| `free`, `tasks`, `restart` | System commands |

## IP Configuration

The board requests an address over DHCP at boot.
If there is no reply within 15 s after the link comes up, it switches to a static IP.
The default static IP is `192.168.10.10/24` with gateway `192.168.10.1`.
When the board is connected directly to a PC, set the PC's wired NIC to `192.168.10.1/24`.
Change these values under "S31 Ethernet Test Configuration" in `idf.py menuconfig`.

## 1G Link Criteria

The result is logged as soon as the link comes up.
The link counts as a real 1G link when all of the following hold:

1. `==== LINK UP: 1000 Mbps, full duplex ====` is printed.
2. `PHY status: 1000 Mbps` shows that the PHY and the MAC agree on the speed.
3. `1000BASE-T OK: ... local_rx=OK, remote_rx=OK` is printed.
4. The idle error count in `ethernet phy` is 0 after an iperf run.
5. TCP throughput exceeds 100 Mbps. The TCP limit on a 100M link is about 94.9 Mbps.

The PHY may drop to 100M even though the link partner supports 1G.
This usually means the pair on pins 4-5 or 7-8 of the cable is broken.
Auto-negotiation only uses the pairs on pins 1-2 and 3-6, so it still succeeds.
When the MAC and PHY speeds disagree, the firmware logs an error.
It then advertises 100M only and brings the link up again.
After replacing the cable, run `ethernet speed auto` to retry 1G.

## iperf Measurement

The PC side must use iperf 2.x. It does not work with iperf3.
The PC must be wired to the same network. A 1G test needs a 1G wired connection.
On a PC connected over Wi-Fi, the Wi-Fi speed becomes the upper limit.

| Test | Board console | PC |
|---|---|---|
| TCP receive | `iperf -s -i 1` | `iperf -c <board IP> -t 10 -i 1` |
| TCP transmit | `iperf -c <PC IP> -t 10 -i 1` | `iperf -s -i 1` |
| UDP receive | `iperf -u -s -i 1` | `iperf -u -c <board IP> -b 1000M -t 10 -i 1` |
| UDP transmit | `iperf -u -c <PC IP> -t 10 -i 1` | `iperf -u -s -i 1` |

## 100M Ethernet Results (2026-11-10)

All four directions were measured on a 100 Mbps network.
Every direction reached the theoretical line rate of a 100M link. The board is not the bottleneck at 100M.

### Test Setup

| Item | Details |
|---|---|
| Firmware | This repository (ESP-IDF v6.1, console on UART0) |
| Network | 100 Mbps wired network, DHCP (192.168.0.0/24, gateway 192.168.0.1) |
| Board | 192.168.0.71 (DHCP) |
| PC | Windows 11, Realtek 2.5GbE NIC (linked at 100 Mbps), 192.168.0.12 |
| PC-side traffic | Python script that behaves like iperf 2.x ([described below](#pc-side-traffic-tool)) |
| Duration | 10 s per direction, 1 s reporting interval |
| Units | 1 MB = 10<sup>6</sup> bytes, 1 Mbit/s = 10<sup>6</sup> bits/s |

### Link State

Boot log:

```text
I (2850) eth_s31: ==== LINK UP: 100 Mbps, full duplex ====
I (2850) eth_s31: PHY status: 100 Mbps full duplex, resolved=1, MDI, downgrade=0 (SSR=0x7C00)
W (2860) eth_s31: NOT gigabit (100 Mbps)
W (2860) eth_s31: Link partner does not advertise 1000BASE-T (100M-only switch/NIC, or 1G disabled)
I (3900) eth_s31: ==== GOT IP (DHCP): 192.168.0.71  mask 255.255.255.0  gw 192.168.0.1 ====
```

Register values read with `ethernet phy` after the iperf runs:

| Register | Value | Meaning |
|---|---|---|
| BMSR (0x01) | `0x796D` | Link up, auto-negotiation complete, no remote fault |
| GBCR (0x09) | `0x0200` | The board advertises 1000BASE-T full duplex |
| ANLPAR (0x05) | `0xCDE1` | The link partner advertises only 100M/10M full/half duplex and pause |
| GBSR (0x0A) | `0x0000` | The link partner does not advertise 1000BASE-T |
| SSR (0x11) | `0x7C00` | 100 Mbps full duplex, MDI, no speed downgrade |
| idle error | 0 | A 1000BASE-T-only metric, for reference only at 100M |

- The link came up at 100M because the link partner does not advertise 1G. The board advertises 1G correctly.
- These results cannot tell whether the cable is fit for 1G (all four pairs connected).
- Four pings from the PC to the board were all answered within 1 ms.

### Throughput

| Test | Direction | Board command | Board average | 1 s interval range | Theoretical limit | Of limit |
|---|---|---|---:|---:|---:|---:|
| TCP receive | PC → board | `iperf -s -i 1 -t 14` | **94.84 Mbit/s** | 94.66 ~ 94.93 | 94.93 | 99.9 % |
| TCP transmit | board → PC | `iperf -c 192.168.0.12 -i 1 -t 10` | **94.91 Mbit/s** | 94.63 ~ 95.29 | 94.93 | 100 % |
| UDP receive | PC → board | `iperf -u -s -i 1 -t 14` | **95.62 Mbit/s** ¹ | 95.41 ~ 95.70 | 95.70 | 99.9 % |
| UDP transmit | board → PC | `iperf -u -c 192.168.0.12 -i 1 -t 10` | **95.73 Mbit/s** | 95.70 ~ 96.01 | 95.70 | 100 % |

¹ Average of the ten 1 s reports while data was flowing.
The board server was left open for 14 s.
The overall average printed by iperf (68.72 Mbit/s) therefore includes 4 s with no data.

For UDP, loss was checked by comparing the amount sent and received on both sides.

| Test | Sender | Receiver | Line loss |
|---|---|---|---|
| UDP receive (PC → board) | PC 120.26 MB (81,809 datagrams) | Board 120.26 MB | None |
| UDP transmit (board → PC) | Board 119.67 MB | PC 119.67 MB (81,407 datagrams) | None |

For TCP transmit, both sides also agree: board 118.64 MB, PC 118.65 MB.

### Theoretical Limit

Calculated from the bytes each frame occupies on a 100 Mbit/s line.
The Ethernet header and FCS take 18 B; the preamble and inter-frame gap take 20 B.

| Type | Payload | Protocol headers | On the wire | Theoretical limit |
|---|---:|---:|---:|---:|
| TCP | 1460 B (MSS) | TCP/IP 40 B | 1538 B | 1460 / 1538 × 100 = 94.93 Mbit/s |
| UDP | 1470 B | UDP/IP 28 B | 1536 B | 1470 / 1536 × 100 = 95.70 Mbit/s |

### Notes on the Results

- **First-second transmit figures:** The first 1 s values (TCP 95.29, UDP 96.01 Mbit/s) are slightly above the theoretical limit.
  This is likely because data still sitting in buffers, not yet on the wire, is already counted.
- **Gaps in UDP transmit sequence numbers:** Counting by sequence number on the PC, about 68 % appears lost.
  Only 81,407 of 255,996 sequence numbers were received.
  This is not real loss. The board tries to send faster than the line rate.
  When the send buffer is full, `sendto` fails with `ENOBUFS`, and iperf silently skips the failure.
  The sequence number increments on every send attempt, so gaps appear.
  The amount the board actually sent equals what the PC received, so there is no line loss.
- **TCP receive end-of-test warning:** `recvfrom error ... Socket is not connected` at the end is printed when the PC closes the connection.
  It does not affect the measurement.

### PC-side Traffic Tool

iperf 2.x was not installed on the PC at the time of the measurement.
PC-side traffic was generated with a Python script that behaves like iperf 2.x.

- Sends or receives data on TCP/UDP port 5001.
- Puts a sequence number in the first 4 bytes of each UDP datagram, as iperf 2.x does.
- Throughput figures are taken from the board's iperf output.
  PC-side counters were only used to cross-check transfer volume and loss.

For future measurements, install iperf 2.x on the PC and use the commands in [iperf Measurement](#iperf-measurement) as they are.

## RJ45 LEDs

Default values of the YT8531 LED configuration registers:
LED0=0x0610, LED1=0x0620, LED2=0x0640.
These appear to map to LED0 = 10M, LED1 = 100M, LED2 = 1000M link indication.
All three LEDs blink on TX/RX activity.

LED outputs in the schematic:

- Pin 33 (LED1) goes to the PHY_LED1 net through a 330 Ω resistor (R81).
- Pin 34 (LED2) goes to the PHY_LED2 net through a 330 Ω resistor (R77).
- Pin 32 (LED0) is used only as the CFG_EXT strap and is not connected to an LED net.

PHY_LED1 and PHY_LED2 appear to drive the RJ45 connector LEDs.
If so, PHY_LED1 lights on a 100M link and PHY_LED2 on a 1G link.
The 10M link indicator (LED0) is not visible from outside.

## Sources and Licenses

- `main/ethernet_iperf_main.c` and `main/cmd_ethernet.c` are modified from ESP-IDF `examples/ethernet/iperf`.
  The original license is Unlicense or CC0-1.0.
- The YT8531 init sequence in `main/eth_s31.c` follows ESP-IDF `examples/ethernet/basic`.
  The original license is Apache-2.0.
- `components/cmd_system` is an unmodified copy of the component in ESP-IDF `examples/system/console/advanced`.
  The original license is Unlicense or CC0-1.0.
- `web/index.html` loads [esptool-js](https://github.com/espressif/esptool-js) 0.7.1 from a CDN.
- The board photo and PHY schematic in `docs/images` come from Espressif's public board documentation.
