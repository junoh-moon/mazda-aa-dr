# 원본 LDS 직렬 입력의 기동 경계 — 2026-09-30

NA 74.00.324A 원본 LDS에 작성한 NMEA를 가상 직렬 포트로 공급했습니다.
원본 SM·SYSTEM·USB 관리자까지 실행 범위를 넓혀 SYSTEM 상태 응답과 LDS의
USB 장치 조회를 확인했습니다. **유효한 원본 GPS 응답이나 이 입력에서의
제품 MODEL 계산은 아직 검증하지 못했습니다.** 실제 응답은 계속
READ_NOT_READY=5, mode·UTC·좌표 0입니다. ASSIST와 공개 ZIP은 변경하지 않았습니다.

## 입력과 실행 범위

기준 소스는 `2548312ce1b4b20dc5aff6d69a56aca08c4f0ad1`입니다. 이전에 검증한
제품의 입력 57개·산출물 5개와 고정 도구체인을 다시 대조했습니다. 제품 ELF는
`08b5bab9a793a2e8b28ae0c7fa54ca1daab55c83b540a2e25da5273c6dab8a73`입니다.
이번 여섯 VM에는 제품 preload나 AA caller를 실행하지 않았습니다. 작성한
후속 caller는 빌드했지만 실행하지 않았으므로 제품 통합 증거로 세지 않습니다.

기존 원본 커널·rootfs의 격리 QEMU VM, 진단 PID1을 사용했습니다. CMU entry의
r1 machine ID 조정 외 커널 바이트를 바꾸지 않았습니다. VM에 네트워크·호스트
장치·공유 폴더를 연결하지 않았고 차량·폰·동글도 사용하지 않았습니다.
원본 LDS·SM·SYSTEM·USB 라이브러리와 반환값은 그대로입니다.

작성한 feeder는 자신이 만든 PTY에만 GSA/GGA/RMC를 200ms 간격으로 씁니다.
합성 날짜·좌표·속도와 체크섬을 사용하며 `valid → invalid → valid` 문장을
생성합니다. native dump의 체크섬·필드 수·문장 길이를 별도로 대조했습니다.
이는 **입력 생성기 검사**이며 원본 파서가 문장을 소비했다는 증거가 아닙니다.

원본 `lds.xml`은 보존하고 별도 진단 XML을 지정했습니다. r1–r3은 primary
포트와 `GPSHardwareControl=0` 두 항목, r4 이후는 primary 포트만 변경합니다.
StartupMode=1, invalid timeout=5초, 재시도·진단 설정은 원래 값입니다.
ReadControl(0/1)은 원본 D-Bus 제어 API로 요청하며 실제 응답을 보존했습니다.

SM 구성은 r2–r4에서 dbus_service, dbus_hmi, stage_1, settings, usb_drivers,
jciUSBMGR, jciLDS, NNG의 8개를 선택했습니다. r5–r6은 원본
system_mazda_my14를 추가한 9개입니다. 원본 전역·서비스 속성, watchdog,
재시도·시간 제한과 내부 의존성을 유지했습니다. 선택하지 않은 서비스의
생략 외 간선 변경은 autorun=no인 NNG→jcinavi 하나이며, NNG를 시작하거나
실행 중인 것으로 대신하지 않았습니다. 정상 전체 CMU 기동 구성이 아닙니다.

## 실행별 결과

| 실행 | 변경·실제 관찰 | 위치 조회 결과 |
| --- | --- | --- |
| r1 | standalone LDS, HWControl=0. SM/NNG 연결 오류, Idle에서 시작 요청 미처리 | 26건 모두 mode 0, 읽기 상태 5 |
| r2 | 원본 SM의 8개 서비스 구성. LDS 미생성, SM watchdog 중단 보고. 시간 한계 전 최종 로그 수집에 미도달 | 24건 모두 ServiceUnknown |
| r3 | 기존 진단의 `taskset 0x02`로 SM 실행. LDS 시작·NNG state 9 응답, 여전히 Idle | 6건 모두 mode 0, 상태 5 |
| r4 | HWControl=1 복원. 제어 호출 반환 0이나 Idle에서 ReadStart 미처리 | 16건 모두 mode 0, 상태 5 |
| r5 | 원본 SYSTEM 추가. GPIO 경로 부재로 HSRM 초기화 실패, SM이 종료 제한 후 SIGKILL | 16건 모두 mode 0, 상태 5 |
| r6 | 원본 GpioChip 모듈 추가. SYSTEM 시작, LDS가 실제 system state 2 수신 후 USB 장치 목록 요청 | 16건 모두 mode 0, 상태 5 |

r2와 r3의 차이를 CPU 배치만의 확정 원인으로 해석하지 않습니다. r3은 최종
로그를 확보하도록 관측 시간을 줄였습니다. 원본 상태 이벤트에서 HWControl
검사를 정적으로 확인했지만 r4의 실패는 그 값만 복원해 기동을 해결할 수
없음을 보여 줍니다.

r5의 `/sys/class/gpio/CAN_Ignition_Status`와 `Power Hold` 부재는 진단 PID1이
원본 GpioChip을 누락한 조건이었습니다. 원본 `gpio_drivers.sh`와 기존 OEM
진단의 모듈 목록을 확인하고 r6에 그 모듈을 추가했습니다. 모듈 반환 0과 실제
sysfs 경로 생성, SYSTEM STARTED를 확인했습니다. 성공값을 반환하는 파일이나
GPIO 대역을 만들지 않았습니다.

r6은 `LDS_SYSTEM_POWER_CONNECT_SUCCESS`, 원본 current-state callback의
0→2와 `USBMGR_USBDevGetDevList` 호출까지 진행했습니다. 이후에도 Idle이며
선택 GPS는 0입니다. 동시에 SYSTEM에는 thermal_zone0의 temp·trip point 부재,
over-temperature event와 standby 전환 시도가 남았습니다. 이 온도 경로가
LDS 정지의 유일한 원인이라고 입증한 것은 아닙니다. USB 목록의 응답·callback과
legacy receiver 선택으로 진행하지 못한 원인은 추가 분리가 필요합니다.

feeder는 r1/r3/r4/r6에서 정상 종료했으며 수신한 명령 바이트는 0입니다.
r5에서는 PTY write 길이 검사가 실패해 exit 134였고 마지막 입력 기록까지
보존했습니다. 구체적인 write errno는 기록하지 않았으므로 EAGAIN 등으로
단정하지 않습니다. r2는 최종 feeder 기록을 수집하지 못했습니다.

## 판정과 남은 작업

여섯 helper는 모두 정해진 관찰 시간으로 종료했습니다. timeout, QEMU 종료 0,
제어 호출 반환 0은 GPS 성공 조건이 아닙니다. helper 종료 뒤 확정된 console
해시를 검증하고 아홉 GetPosition 반환 필드의 실제 타입과 값을 파싱했습니다.
r2를 제외한 다섯 실행은 진단 종료 표식에 도달했습니다.

- 원본 USB 목록 요청의 실제 응답·callback과 receiver 선택 경계를 확인해야 합니다.
- 원본 직렬 읽기·NMEA 파서에서 나온 유효 fix, 단절·복귀는 미검증입니다.
- 이 GPS 응답을 제품 요청 관측·MODEL에 연결하는 후속 caller는 미실행입니다.
- 정상 전체 기동·물리 센서·전원 복구·폰/앱 수용은 계속 미검증입니다.

생산 코드는 바뀌지 않았으므로 host/ARM 전체 회귀는 이번 기동 진단에서
반복하지 않았습니다. 앞선 [MODEL 버스 검증](MODEL_BUS_2026-09-30.md)의
제품 입력 확인과 이번 실제 원본 실행을 구분합니다. 조사 중 fetch에서 외부
`00c6c5e`를 발견했고 그 작성 signal-reuse 회귀가 현재 제품 소스에서 실패함을
확인했습니다. 해당 통합·수정 검증은 별도 작업입니다.

원본 파일·역어셈블·전체 로그는 공개하지 않습니다. 작성 fixture·이미지·실행
기록은 ignored `evidence/lds-input-20260930-2548312/`에 보관합니다.

| 최종 r6 입력·출력 | SHA-256 |
| --- | --- |
| 순정 커널 | `57fe0cda7c1f994a6ca6b3cce7d9cf247f3b5607ebf1ddc12f02598662d5e240` |
| 원본 LDS | `0009af4c01d7628a0be491212ca6d73cfd3210b564b97446111435e3bac6a7ea` |
| 진단 init | `1e909259024104bb3748152747155e1d8cc18db7a8a768f8f4035172681d4906` |
| 비공개 initrd | `d2764c5d6afed7c1e806f3ab7eabc0757f1d1679f50762f7dfa0c2eab21238ce` |
| 최종 console | `45846e2ceb26e1fccedd2a71c53ebe76285d53a9ff863e7ff65571d1faa66047` |

추가 도구는 기존 이미지에서 만든 전용 컨테이너 안에만 설치했습니다.
이 체크포인트에서는 같은 브랜치의 후속 버스 통합 검사에 계속 사용 중입니다.
최종 작업 종료 전에 컨테이너·고정 도구체인·임시 폴더를 제거하고 설치 전후
목록을 대조해야 합니다. 현재 이 문장을 제거 완료 기록으로 해석하지 않습니다.
