# ESP32-S31 1G Ethernet iperf 테스트 펌웨어

ESP32-S31-Function-CoreBoard-1의 기가비트 이더넷을 점검하는 펌웨어다.
ESP-IDF v6.1의 `examples/ethernet/iperf`를 바탕으로 만들었다.

## 웹 플래셔

**<https://firepooh.github.io/esp32s31-eth1g-iperf/>**

1. 데스크톱 Chrome이나 Edge로 연다.
2. 보드의 USB-UART Type-C 포트를 PC에 연결한다.
3. 버전을 고르고 **연결** → **플래시**를 누른다.

바이너리와 esptool 명령은 [Releases](https://github.com/firepooh/esp32s31-eth1g-iperf/releases)에 있다.

## 사용 보드

![ESP32-S31-Function-CoreBoard-1 부품 배치](docs/images/esp32-s31-function-coreboard-1.webp)

| 항목 | 내용 |
|---|---|
| 보드 | [ESP32-S31-Function-CoreBoard-1](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp32-s31-function-coreboard-1/index.html) |
| 모듈 | ESP32-S31-WROOM-3 (플래시 16 MB, PSRAM 16 MB) |
| 칩 | ESP32-S31 rev v0.0 |
| 이더넷 | RJ45, 10/100/1000 Mbps 자동 협상 |
| 개발 환경 | ESP-IDF v6.1, Windows 11 |

이 프로젝트에서 쓰는 포트와 버튼은 다음과 같다. 번호는 위 사진의 번호다.

| 번호 | 실크 표기 | 용도 |
|---|---|---|
| (6) | USB-UART | 콘솔(REPL과 로그)과 플래시. PC에서 COM 포트로 잡힌다(예: COM20). |
| (5) | USB-DBG | USB Serial/JTAG. 로그를 출력 전용으로 함께 내보낸다. |
| (2) | RJ45 | 시험 대상 이더넷 포트 |
| (14), (16) | RST, BOOT | 수동 리셋, 다운로드 모드 진입 |

## 목적

1. 보드가 상대 장비와 실제로 1000 Mbps 링크를 맺는지 확인한다.
2. TCP와 UDP의 송수신 최대 처리량을 iperf로 측정한다.
3. 링크가 1G로 맺어지지 않으면 원인을 구분한다.
   상대 장비가 1G를 지원하지 않는지, 케이블이 불량인지를 PHY 레지스터로 판별한다.

## 진행 상태

- [x] 빌드, 플래시, 부팅 확인
- [x] YT8531 PHY 인식 확인 (MDIO 주소 0, ID 0x4F51_E91B)
- [x] 100M 링크 확인 (100 Mbps 전이중, 2026-11-10)
- [x] 100M TCP/UDP 처리량 측정 (네 방향 모두 회선 한계 도달, [결과](#100m-이더넷-측정-결과-2026-11-10))
- [ ] 1G 링크 확인 (1000BASE-T를 지원하는 상대 장비 필요)
- [ ] 1G TCP/UDP 처리량 측정

## 하드웨어 구성

| 항목 | 값 |
|---|---|
| MAC | ESP32-S31 내장 EMAC, RGMII |
| PHY | Motorcomm YT8531 (MDIO 주소 0, 부팅 시 자동 검출) |
| SMI | MDC GPIO5, MDIO GPIO6 |
| PHY 리셋 | GPIO7 |
| TX | TXC GPIO13, TX_CTL GPIO12, TXD0~3 GPIO8~11 |
| RX | RXC GPIO14, RX_CTL GPIO15, RXD0~3 GPIO19, 18, 17, 16 |
| 콘솔 | UART0 (GPIO58, 59), USB-UART Type-C 포트, 115200 bps |
| 보조 로그 | USB Serial/JTAG Type-C 포트 (출력 전용) |

핀 배치는 IDF v6.1의 S31 기본값(`ETH_ESP32_EMAC_DEFAULT_CONFIG`)을 그대로 쓴다.
PHY는 IEEE 802.3 범용 드라이버로 구동한다.
YT8531 전용 설정 두 가지를 추가로 적용한다.

- PHY 리셋 뒤 꺼지는 자동 협상을 다시 켠다.
- RGMII TX/RX 클록 지연을 약 2 ns로 설정한다.

### YT8531 PHY 회로

![YT8531 PHY 회로도](docs/images/yt8531-phy-schematic.png)

회로도에서 확인한 주요 사항은 다음과 같다.

- 클록은 25 MHz 수정 진동자(Y2)를 쓴다. CLKOUT(35번 핀)은 연결하지 않는다.
- RX 신호(RX_CLK, RX_CTL, RXD0~3)에는 MAC 가까이에 33 Ω 직렬 저항(R46~R51)이 있다.
- TX 신호(TX_CLK, TX_CTL, TXD0~3)에는 PHY 가까이에 33 Ω 직렬 저항(R55~R62)이 있다.
- MDIO는 4.7 kΩ(R66)으로 ETH_PHY_3V3에 풀업한다.
- INT_N은 4.7 kΩ(R40)으로 풀업하고 ETH_INTN 넷으로 나간다.

스트랩 핀은 RGMII RX 핀과 LED 핀을 같이 쓴다.
RX 핀 쪽 스트랩 저항(R53, R54, R57, R60, R63, R65)은 미실장(NC)이다.
그래서 이 핀들은 칩 내부 기본값을 따른다.

| 스트랩 | 핀 | 설정 | 결과 |
|---|---|---|---|
| PHYAD0~2 | RXD3, RX_CLK, RX_CTL | 내부 풀다운 (R53, R54, R57 NC) | PHY 주소 0 |
| RXDLY | RXD0 | 내부 풀업 (R60 NC) | RX_CLK 지연 추가 (125 MHz에서 2 ns) |
| TXDLY | RXD1 | 내부 풀다운 (R63 NC) | TX_CLK 스트랩 지연 없음 |
| PLLOFF | RXD2 | 내부 풀다운 (R65 NC) | 슬립 중에도 PLL을 끄지 않음 |
| CFG_EXT | LED0 (32번) | 4.7 kΩ 풀업 (R38) | I/O 전원을 외부에서 공급 |
| CFG_LDO[1:0] | LED2, LED1 (34, 33번) | 4.7 kΩ 풀다운 (R35, R36) | I/O 전압 3.3 V |

TXDLY 스트랩만으로는 TX 지연이 들어가지 않는다.
그래서 펌웨어가 확장 레지스터로 TX 지연(약 1.95 ns)을 따로 넣는다.
RX 지연도 같은 값(약 2 ns)으로 다시 설정한다.

## 빌드와 플래시

PowerShell에서 실행한다.
USB-UART Type-C 포트를 PC에 연결하고 COM 포트 번호를 확인한다.
`ESPPORT` 환경 변수가 다른 포트로 잡혀 있으면 `-p` 옵션과 충돌한다.
그래서 `ESPPORT`를 직접 바꿔서 실행한다.

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
$env:ESPPORT = "COM20"
idf.py --preview build flash monitor
```

- ESP32-S31은 아직 프리뷰 타깃이라 `--preview` 옵션이 필요하다.
- USB-UART 포트의 DTR/RTS 자동 리셋 회로로 다운로드 모드에 들어간다. BOOT 버튼을 누를 필요가 없다.
- 콘솔을 USB Serial/JTAG 포트로 되돌리려면 `sdkconfig.defaults`에서 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`로 바꾼다.
- `sdkconfig.defaults`를 고친 뒤에는 기존 `sdkconfig`를 지우고 다시 빌드한다. 지우지 않으면 바뀐 기본값이 반영되지 않는다.
- 릴리즈 빌드는 저장소의 `sdkconfig`로 빌드한다. 다시 만든 `sdkconfig`도 함께 커밋한다.

### 릴리즈 빌드

`v*` 태그를 푸시하면 GitHub Actions가 빌드해 릴리즈와 웹 플래셔를 갱신한다.

- `espressif/idf:v6.1` Docker 이미지로 빌드한다.
- 릴리즈에는 병합 이미지(`-merged.bin`), 개별 파일 묶음(`-all.zip`), `SHA256SUMS.txt`를 올린다.
- `-`가 들어간 태그(`v1.1.0-rc1`)는 프리릴리즈가 된다.
- `dependencies.lock`으로 컴포넌트 버전을 고정한다.
- Actions 탭에서 "Release build"를 수동 실행하면 릴리즈 없이 빌드 결과만 아티팩트로 남는다.

## 콘솔 명령

| 명령 | 설명 |
|---|---|
| `ethernet info` | 링크 상태, MAC 속도, PHY 실제 속도, IP |
| `ethernet phy` | PHY 레지스터 덤프와 해석 (1000BASE-T 상태, idle error 수) |
| `ethernet speed auto` | 1000/100/10 광고 후 재협상 |
| `ethernet speed 100` | 100M 이하만 광고 후 재협상 |
| `ethernet ip <addr> <mask> [gw]` | 고정 IP 설정 |
| `ethernet dhcp` | DHCP로 전환 |
| `iperf ...` | 처리량 측정 (iperf 2.x 호환) |
| `free`, `tasks`, `restart` | 시스템 명령 |

## IP 설정 방식

부팅하면 DHCP로 주소를 받는다.
링크가 올라온 뒤 15초 안에 응답이 없으면 고정 IP로 바꾼다.
기본 고정 IP는 `192.168.10.10/24`, 게이트웨이는 `192.168.10.1`이다.
PC와 보드를 직접 연결하면 PC 유선 NIC를 `192.168.10.1/24`로 설정한다.
값은 `idf.py menuconfig`의 "S31 Ethernet Test Configuration"에서 바꾼다.

## 1G 연결 판정 기준

링크가 올라오면 로그에 결과가 바로 나온다.
아래 조건을 모두 만족하면 실제 1G 연결로 본다.

1. `==== LINK UP: 1000 Mbps, full duplex ====`가 출력된다.
2. `PHY status: 1000 Mbps`로 PHY 판정과 MAC 설정이 일치한다.
3. `1000BASE-T OK: ... local_rx=OK, remote_rx=OK`가 출력된다.
4. iperf 측정 뒤 `ethernet phy`의 idle error 수가 0이다.
5. TCP 처리량이 100 Mbps를 넘는다. 100M 링크의 TCP 한계는 약 94.9 Mbps다.

상대 장비는 1G를 지원하는데 PHY가 100M로 떨어질 수 있다.
이때는 케이블의 4-5번 핀이나 7-8번 핀 페어가 끊긴 경우가 많다.
자동 협상은 1-2번, 3-6번 페어만으로 이뤄지기 때문이다.
펌웨어는 MAC/PHY 속도가 어긋나면 오류를 출력한다.
그 다음 100M만 광고하도록 바꿔서 링크를 다시 올린다.
케이블을 바꾼 뒤 `ethernet speed auto`로 1G를 다시 시도한다.

## iperf 측정

PC 쪽은 반드시 iperf 2.x를 쓴다. iperf3와는 호환되지 않는다.
PC도 유선으로 같은 네트워크에 연결해야 한다. 1G 시험에는 1G 유선이 필요하다.
Wi-Fi로 연결된 PC에서는 Wi-Fi 속도가 상한이 된다.

| 시험 | 보드 콘솔 | PC |
|---|---|---|
| TCP 수신 | `iperf -s -i 1` | `iperf -c <보드 IP> -t 10 -i 1` |
| TCP 송신 | `iperf -c <PC IP> -t 10 -i 1` | `iperf -s -i 1` |
| UDP 수신 | `iperf -u -s -i 1` | `iperf -u -c <보드 IP> -b 1000M -t 10 -i 1` |
| UDP 송신 | `iperf -u -c <PC IP> -t 10 -i 1` | `iperf -u -s -i 1` |

## 100M 이더넷 측정 결과 (2026-11-10)

100 Mbps 네트워크에서 네 방향을 모두 측정했다.
네 방향 모두 100M 회선의 이론 한계에 도달했다. 100M에서는 보드 쪽 병목이 없다.

### 시험 환경

| 항목 | 내용 |
|---|---|
| 펌웨어 | 이 저장소 (ESP-IDF v6.1, 콘솔 UART0) |
| 네트워크 | 100 Mbps 유선 네트워크, DHCP (192.168.0.0/24, 게이트웨이 192.168.0.1) |
| 보드 | 192.168.0.71 (DHCP) |
| PC | Windows 11, Realtek 2.5GbE NIC (100 Mbps로 링크), 192.168.0.12 |
| PC 쪽 트래픽 | iperf 2.x와 같은 방식으로 동작하는 Python 스크립트 ([아래 설명](#pc-쪽-트래픽-도구)) |
| 측정 시간 | 방향마다 10초, 1초 간격 보고 |
| 단위 | 1 MB = 10<sup>6</sup> 바이트, 1 Mbit/s = 10<sup>6</sup> 비트/초 |

### 링크 상태

부팅 로그는 다음과 같다.

```text
I (2850) eth_s31: ==== LINK UP: 100 Mbps, full duplex ====
I (2850) eth_s31: PHY status: 100 Mbps full duplex, resolved=1, MDI, downgrade=0 (SSR=0x7C00)
W (2860) eth_s31: NOT gigabit (100 Mbps)
W (2860) eth_s31: Link partner does not advertise 1000BASE-T (100M-only switch/NIC, or 1G disabled)
I (3900) eth_s31: ==== GOT IP (DHCP): 192.168.0.71  mask 255.255.255.0  gw 192.168.0.1 ====
```

iperf 측정을 마친 뒤 `ethernet phy`로 읽은 레지스터 값이다.

| 레지스터 | 값 | 해석 |
|---|---|---|
| BMSR (0x01) | `0x796D` | 링크 업, 자동 협상 완료, 원격 오류 없음 |
| GBCR (0x09) | `0x0200` | 보드는 1000BASE-T 전이중을 광고한다 |
| ANLPAR (0x05) | `0xCDE1` | 상대 장비는 100M/10M 전이중·반이중과 pause만 광고한다 |
| GBSR (0x0A) | `0x0000` | 상대 장비는 1000BASE-T를 광고하지 않는다 |
| SSR (0x11) | `0x7C00` | 100 Mbps 전이중, MDI, 속도 강등 없음 |
| idle error | 0 | 1000BASE-T 전용 지표라 100M에서는 참고값이다 |

- 상대 장비가 1G를 광고하지 않아서 100M로 연결됐다. 보드는 정상적으로 1G를 광고한다.
- 이번 결과로는 케이블이 1G에 맞는지(4쌍 모두 연결됐는지) 판단할 수 없다.
- PC에서 보드로 ping을 4번 보냈고 모두 1 ms 안에 응답했다.

### 처리량

| 시험 | 방향 | 보드 명령 | 보드 측정 평균 | 1초 구간 범위 | 이론 한계 | 한계 대비 |
|---|---|---|---:|---:|---:|---:|
| TCP 수신 | PC → 보드 | `iperf -s -i 1 -t 14` | **94.84 Mbit/s** | 94.66 ~ 94.93 | 94.93 | 99.9 % |
| TCP 송신 | 보드 → PC | `iperf -c 192.168.0.12 -i 1 -t 10` | **94.91 Mbit/s** | 94.63 ~ 95.29 | 94.93 | 100 % |
| UDP 수신 | PC → 보드 | `iperf -u -s -i 1 -t 14` | **95.62 Mbit/s** ¹ | 95.41 ~ 95.70 | 95.70 | 99.9 % |
| UDP 송신 | 보드 → PC | `iperf -u -c 192.168.0.12 -i 1 -t 10` | **95.73 Mbit/s** | 95.70 ~ 96.01 | 95.70 | 100 % |

¹ 데이터가 흐른 10초 동안의 1초 보고 10개를 평균한 값이다.
보드 서버는 14초 동안 열어 뒀다.
그래서 iperf가 출력한 전체 평균(68.72 Mbit/s)에는 데이터가 없던 4초가 섞여 있다.

UDP는 보낸 양과 받은 양을 양쪽에서 비교해 손실을 확인했다.

| 시험 | 보낸 쪽 | 받은 쪽 | 회선 손실 |
|---|---|---|---|
| UDP 수신 (PC → 보드) | PC 120.26 MB (데이터그램 81,809개) | 보드 120.26 MB | 없음 |
| UDP 송신 (보드 → PC) | 보드 119.67 MB | PC 119.67 MB (데이터그램 81,407개) | 없음 |

TCP 송신도 보드 118.64 MB, PC 118.65 MB로 양쪽 집계가 같다.

### 이론 한계 계산

100 Mbit/s 회선에서 프레임 하나가 차지하는 바이트로 계산한다.
이더넷 헤더와 FCS는 18 B, 프리앰블과 프레임 간격은 20 B다.

| 종류 | 데이터 | 프로토콜 헤더 | 회선 점유 | 이론 한계 |
|---|---:|---:|---:|---:|
| TCP | 1460 B (MSS) | TCP/IP 40 B | 1538 B | 1460 / 1538 × 100 = 94.93 Mbit/s |
| UDP | 1470 B | UDP/IP 28 B | 1536 B | 1470 / 1536 × 100 = 95.70 Mbit/s |

### 결과를 볼 때 알아둘 점

- **송신 첫 1초 수치:** 첫 1초 값(TCP 95.29, UDP 96.01 Mbit/s)은 이론 한계보다 조금 높다.
  아직 회선에 나가지 않고 버퍼에 쌓인 데이터까지 집계되기 때문으로 보인다.
- **UDP 송신 패킷 번호 공백:** PC에서 패킷 번호로 계산하면 약 68 %가 빠진 것처럼 보인다.
  번호 255,996개 중 81,407개만 받았기 때문이다.
  실제 손실은 아니다. 보드는 회선 속도보다 빠르게 보내려 한다.
  송신 버퍼가 차면 `sendto`가 `ENOBUFS`로 실패하고, iperf는 이 실패를 출력 없이 건너뛴다.
  패킷 번호는 전송을 시도할 때마다 늘어나므로 번호에 빈 곳이 생긴다.
  보드가 실제로 보낸 양과 PC가 받은 양이 같으므로 회선 손실은 없다.
- **TCP 수신 종료 경고:** 측정 끝에 나오는 `recvfrom error ... Socket is not connected`는 PC가 연결을 닫을 때 나오는 메시지다.
  측정값에는 영향이 없다.

### PC 쪽 트래픽 도구

측정 당시 PC에 iperf 2.x가 설치되어 있지 않았다.
그래서 iperf 2.x처럼 동작하는 Python 스크립트로 PC 쪽 트래픽을 만들었다.

- TCP/UDP 5001번 포트로 데이터를 보내거나 받는다.
- UDP 데이터그램의 앞 4바이트에는 iperf 2.x처럼 패킷 번호를 넣는다.
- 처리량은 보드 iperf가 출력한 값을 기준으로 삼았다.
  PC 쪽 집계는 전송량과 손실을 맞춰 보는 데만 썼다.

다음 측정부터는 PC에 iperf 2.x를 설치해 [iperf 측정](#iperf-측정)의 명령을 그대로 쓰는 편이 좋다.

## RJ45 LED

YT8531 LED 설정 레지스터 기본값은 다음과 같다.
LED0=0x0610, LED1=0x0620, LED2=0x0640.
이 값은 LED0=10M, LED1=100M, LED2=1000M 링크 표시로 보인다.
세 LED 모두 송수신 중에 깜박인다.

회로도상 LED 출력은 다음과 같이 연결된다.

- 33번 핀(LED1)은 330 Ω 저항(R81)을 거쳐 PHY_LED1 넷으로 나간다.
- 34번 핀(LED2)은 330 Ω 저항(R77)을 거쳐 PHY_LED2 넷으로 나간다.
- 32번 핀(LED0)은 CFG_EXT 스트랩으로만 쓰고 LED 넷에는 연결되지 않는다.

PHY_LED1, PHY_LED2가 RJ45 커넥터 LED로 이어지는 것으로 보인다.
그렇다면 100M 링크에서는 PHY_LED1, 1G 링크에서는 PHY_LED2가 켜진다.
10M 링크 표시(LED0)는 바깥에서 볼 수 없다.

## 출처와 라이선스

- `main/ethernet_iperf_main.c`, `main/cmd_ethernet.c`는 ESP-IDF `examples/ethernet/iperf`를 고친 것이다.
  원본 라이선스는 Unlicense 또는 CC0-1.0이다.
- `main/eth_s31.c`의 YT8531 초기화 순서는 ESP-IDF `examples/ethernet/basic`을 따랐다.
  원본 라이선스는 Apache-2.0이다.
- `components/cmd_system`은 ESP-IDF `examples/system/console/advanced`의 컴포넌트를 그대로 복사한 것이다.
  원본 라이선스는 Unlicense 또는 CC0-1.0이다.
- `web/index.html`은 [esptool-js](https://github.com/espressif/esptool-js) 0.7.1을 CDN에서 불러 쓴다.
- `docs/images`의 보드 사진과 PHY 회로도는 Espressif의 보드 공개 자료에서 가져왔다.
