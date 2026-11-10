# ESP32-S31 1G Ethernet iperf 테스트 펌웨어

ESP32-S31-Function-CoreBoard-1의 기가비트 이더넷을 점검하는 펌웨어다.
ESP-IDF v6.1의 `examples/ethernet/iperf`를 바탕으로 만들었다.

## 사용 보드

| 항목 | 내용 |
|---|---|
| 보드 | [ESP32-S31-Function-CoreBoard-1](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp32-s31-function-coreboard-1/index.html) |
| 모듈 | ESP32-S31-WROOM-3 (플래시 16 MB, PSRAM 16 MB) |
| 칩 | ESP32-S31 rev v0.0 |
| 이더넷 | RJ45, 10/100/1000 Mbps 자동 협상 |
| 개발 환경 | ESP-IDF v6.1, Windows 11 |

## 목적

1. 보드가 상대 장비와 실제로 1000 Mbps 링크를 맺는지 확인한다.
2. TCP와 UDP의 송수신 최대 처리량을 iperf로 측정한다.
3. 링크가 1G로 맺어지지 않으면 원인을 구분한다.
   상대 장비가 1G를 지원하지 않는지, 케이블이 불량인지를 PHY 레지스터로 판별한다.

## 진행 상태

- [x] 빌드, 플래시, 부팅 확인
- [x] YT8531 PHY 인식 확인 (MDIO 주소 0, ID 0x4F51_E91B)
- [ ] 링크 확인 (첫 시험에서는 상대 장비 신호가 잡히지 않음)
- [ ] TCP/UDP 처리량 측정

## 하드웨어 구성

| 항목 | 값 |
|---|---|
| MAC | ESP32-S31 내장 EMAC, RGMII |
| PHY | Motorcomm YT8531 (MDIO 주소 0, 부팅 시 자동 검출) |
| SMI | MDC GPIO5, MDIO GPIO6 |
| PHY 리셋 | GPIO7 |
| TX | TXC GPIO13, TX_CTL GPIO12, TXD0~3 GPIO8~11 |
| RX | RXC GPIO14, RX_CTL GPIO15, RXD0~3 GPIO19, 18, 17, 16 |
| 콘솔 | USB Serial/JTAG Type-C 포트 (예: COM23) |

핀 배치는 IDF v6.1의 S31 기본값(`ETH_ESP32_EMAC_DEFAULT_CONFIG`)을 그대로 쓴다.
PHY는 IEEE 802.3 범용 드라이버로 구동한다.
YT8531 전용 설정 두 가지를 추가로 적용한다.

- PHY 리셋 뒤 꺼지는 자동 협상을 다시 켠다.
- RGMII TX/RX 클록 지연을 약 2 ns로 설정한다.

## 빌드와 플래시

PowerShell에서 실행한다.
`ESPPORT` 환경 변수가 다른 포트로 잡혀 있으면 `-p` 옵션과 충돌한다.
그래서 `ESPPORT`를 직접 바꿔서 실행한다.

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
$env:ESPPORT = "COM23"
idf.py --preview build flash monitor
```

ESP32-S31은 아직 프리뷰 타깃이라 `--preview` 옵션이 필요하다.

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
5. TCP 처리량이 100 Mbps를 넘는다. 100M 링크로는 약 94 Mbps가 한계다.

상대 장비는 1G를 지원하는데 PHY가 100M로 떨어질 수 있다.
이때는 케이블의 4-5번 핀이나 7-8번 핀 페어가 끊긴 경우가 많다.
자동 협상은 1-2번, 3-6번 페어만으로 이뤄지기 때문이다.
펌웨어는 MAC/PHY 속도가 어긋나면 오류를 출력한다.
그 다음 100M만 광고하도록 바꿔서 링크를 다시 올린다.
케이블을 바꾼 뒤 `ethernet speed auto`로 1G를 다시 시도한다.

## iperf 측정

PC 쪽은 반드시 iperf 2.x를 쓴다. iperf3와는 호환되지 않는다.
PC도 1G 유선으로 같은 네트워크에 연결해야 한다.
Wi-Fi로 연결된 PC에서는 Wi-Fi 속도가 상한이 된다.

| 시험 | 보드 콘솔 | PC |
|---|---|---|
| TCP 수신 | `iperf -s -i 1` | `iperf -c <보드 IP> -t 10 -i 1` |
| TCP 송신 | `iperf -c <PC IP> -t 10 -i 1` | `iperf -s -i 1` |
| UDP 수신 | `iperf -u -s -i 1` | `iperf -u -c <보드 IP> -b 1000M -t 10 -i 1` |
| UDP 송신 | `iperf -u -c <PC IP> -t 10 -i 1` | `iperf -u -s -i 1` |

## RJ45 LED

YT8531 LED 설정 레지스터 기본값은 다음과 같다.
LED0=0x0610, LED1=0x0620, LED2=0x0640.
이 값은 LED0=10M, LED1=100M, LED2=1000M 링크 표시로 보인다.
세 LED 모두 송수신 중에 깜박인다.
RJ45 커넥터의 LED가 어느 PHY 핀에 연결됐는지는 회로도로 확인해야 한다.

## 출처와 라이선스

- `main/ethernet_iperf_main.c`, `main/cmd_ethernet.c`는 ESP-IDF `examples/ethernet/iperf`를 고친 것이다.
  원본 라이선스는 Unlicense 또는 CC0-1.0이다.
- `main/eth_s31.c`의 YT8531 초기화 순서는 ESP-IDF `examples/ethernet/basic`을 따랐다.
  원본 라이선스는 Apache-2.0이다.
- `cmd_system` 컴포넌트는 빌드할 때 ESP-IDF 설치 경로에서 가져온다.
