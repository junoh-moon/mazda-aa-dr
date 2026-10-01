# 원본 LDS 경로의 CPU 루프 제거 재실행 — 2026-10-01

[앞선 원본 LDS 경로 실행](LDS_PATH_VM_2026-10-01.md)은 수정한 NMEA
fixture에서 원본 `GetPosition`의 유효→무효→재획득을 확인했지만, 9개 원본
서비스의 두 완주에는 진단용 CPU 유휴 방지 루프가 들어 있었습니다. 이 루프가
없는 동일 조건을 별도로 재실행하여 기본 QEMU에서의 재현성을 확인했습니다.

QEMU 7.2.22의 `sabrelite` 보드, vCPU 2개, 원본 커널, 원본 서비스 9개와
유지한 의존성 4개, 합성 NMEA PTY, 원본 LDS USB GPIO 되읽기의 진단용
`pass`/`mirror` 비교를 사용했습니다. 네 실행의 커널 SHA-256은
`57fe0cda7c1f994a6ca6b3cce7d9cf247f3b5607ebf1ddc12f02598662d5e240`,
initrd SHA-256은
`25d7cdb62ad042d6689ae0fe79a18ddcae7e31bde7cdd915b0c7847a2bb86d48`로
같습니다. 앞선 수정 fixture와 비교해 진단 init 스크립트에서 CPU 루프의
시작·확인·종료와 해당 설명만 제거하고 빌드 범위 설명도 맞췄습니다.
원본 커널·서비스, NMEA 공급기, GPIO 진단 interposer와 제품 바이너리는
변경하지 않았습니다. 호스트 네트워크·
장치·공유 디렉터리는 VM에 연결하지 않았습니다.

| 실행 | 제한 | 마지막 guest 질의 진행 표식 | 그 밖의 직접 관측 | 종료 |
| --- | ---: | ---: | --- | --- |
| `pass` | 240초 | 3 | 두 vCPU가 앞선 실행과 같은 커널 WFI 경로에 있음 | GDB 관측 도중 QEMU `-11`; 완료 없음 |
| `mirror-r1` | 240초 | 25 | 질의 8에서 `READ_STARTED` 표식 | 제한 종료; 완료 없음 |
| `pass-r2` | 180초 | 9 | 외부 GDB 관측 없음 | 제한 종료; 완료 없음 |
| `mirror-r2` | 180초 | 6 | 외부 GDB 관측 없음 | 제한 종료; 완료 없음 |

모든 실행에서 machine ID의 기존 진단 조정과 guest의 서비스 시작 표식을
확인했고, private console 파일의 SHA-256을 실행 메타데이터와 다시
대조했습니다. 어느 실행에도 guest 완료 표식이나 최종 질의 덤프가
없습니다. `mirror-r1`의 `READ_STARTED`는 유효 `GetPosition` 응답을
뜻하지 않습니다. 이 실행들에서 mode 1 응답·A/B/C 주기 대조를 새로
입증했다고 세지 않습니다. 제한 종료의 QEMU 반환값 0도 성공이 아닙니다.

첫 `pass`의 live GDB는 두 vCPU가 앞선 정지와 같은 원본 커널 WFI 경로에
있는 순간을 포착했습니다. 그 뒤 여러 monitor 조회 중 물리 메모리 probe와
맞물려 QEMU가 `-11`로 종료했습니다. probe가 원인인지 이미 진행 중인
결함과 우연히 겹쳤는지는 분리되지 않았습니다. 외부 GDB 없이 실행한
나머지 세 번도 guest 질의가 멈췄으나 그 순간의 CPU·서비스 thread 상태는
측정하지 않았습니다. `pass`/`mirror` 사이의 정지 질의 번호도 일정하지
않습니다. 따라서 GPIO 진단 보정이나 CPU 루프 중 어느 하나가 정지의
충분한 원인이라는 결론은 내리지 않습니다.

현재 확인된 범위는 **진단용 CPU 루프를 제거한 수정 fixture의 네 번 모두
끝까지 진행하지 못했다**는 것입니다. 앞선 루프 포함 실행의 완주는
기본 QEMU·정상 전체 SM·실제 CMU 기동의 재현성 증거로 승격하지 않습니다.

## 호스트 실행 방식과 WFI 분리

같은 no-loop 이미지를 QEMU `-accel tcg,thread=single`로 실행한
`mirror`는 질의 7 이후 180초 제한까지 완료하지 못했습니다. 호스트
TCG의 다중 스레드 실행만 제거하는 것으로 이 표본은 해결되지 않았습니다.
그 실행의 종료 코드 0은 제한에 따른 QEMU 종료이며 guest 완료가 아닙니다.

다음 `pass` 실행은 별도 QMP 소켓으로 실행 중과 정지 뒤를 관찰했습니다.
질의 10 이후 진행하지 않은 동안 QMP `query-status`는 `running`을
반환했고, `info registers`를 5초 간격으로 세 번 읽자 두 vCPU 모두 앞선
커널 WFI 지점의 같은 프로그램 카운터에 있었습니다. 호스트의 두 vCPU
스레드도 대기 상태였습니다. GDB monitor 물리 메모리 probe는 사용하지
않았고 QEMU는 180초 제한 종료까지 살아 있었습니다. 이 사실은 정지 중
CPU가 WFI에 머문다는 관측을 독립 경로로 재현하지만, QEMU가 깨워야 할
인터럽트를 잃었는지, guest 서비스가 실제로 대기 중인지 구분하지 못합니다.
이 QEMU 버전의 `query-cpus-fast`에는 halted/PC 필드가 없고 `query-cpus`는
지원되지 않았습니다. HMP `info irq`/`info pic`도 이 보드에서 정보를
출력하지 않았습니다. 인터럽트/타이머 원인은 계속 미분리입니다.

## 진단용 `nohlt`에서의 조건부 완주

원본 커널과 no-loop initrd를 그대로 두고 커널 인자 `nohlt`만 추가한
동일 이미지 `pass`/`mirror` 비교를 수행했습니다.
[상류 Linux 3.0 ARM 코드](https://raw.githubusercontent.com/torvalds/linux/v3.0/arch/arm/kernel/process.c)는
이 인자에서 idle의 저전력 대기 대신 polling을 선택합니다.
이번 원본 커널의 QMP 표본에서도 두 vCPU가 앞선 WFI 지점에 고정되지
않고 계속 실행됐습니다. 이 비교의 QEMU 명령은 GPIO 인자만 다르며,
별도 QMP 소켓을 사용했습니다. 이는 원본 차량의 부팅 인자나 정상
에뮬레이터 조건이 아닙니다.

| 조건 | 원본 API 질의 | 진단 관측 | guest 결과 |
| --- | ---: | --- | --- |
| `pass nohlt` | 30회 중 응답 29건 | GPIO 출력/방향 1이지만 원본 되읽기 0, PTY 미개방 | 완료, mode 0 유지 |
| `mirror nohlt` | 37회 중 응답 36건 | GPIO `0`→진단 전달 `1`, `ReadControl(0)` 1회·PTY 개방 | 완료, 합성 A/B/C 위치 전이 |

두 guest의 공급기 종료 0, 완료 표식, 원본 API 응답 형식, 같은 커널·
initrd 해시와 console 메타데이터 해시를 사후 검증기로 확인했습니다.
`pass`는 mode 0/selected 0/READ_NOT_READY가 계속됐고, `mirror`는
READ_STARTED 뒤 합성 유효 A 7건, 무효 B 4건, 재획득 C 17건을
보였습니다. 그중 실제 작성되고 대기열이 빈 고유 공급 주기와 UTC가
같은 질의는 각각 7·3·15건입니다. `mirror`의 질의 9는 **mode 1이지만
UTC 0에 시작 전 좌표**가 남았고, 질의 17의 B 좌표·질의 21의 C 좌표는
각각 직전 A·B 주기의 UTC와 결합됐습니다. 검증기는 이 세 건을 명시적으로
자격 없는 경계 응답으로 분리했으며 A/B/C의 신선한 일치 표본에 넣지
않았습니다. 한 응답의 mode·좌표·UTC가 동일한 생산 측정이라는 보장은
없습니다.

두 실행에서 QEMU helper는 180초 제한으로 종료됐지만, guest는 그 전에
완료 표식과 최종 질의 덤프를 출력했습니다. 따라서 helper의 timeout 반환을
성공 근거로 쓰지 않았습니다. 이번 조건부 완주는 idle 경로와 VM 진행
사이의 연관을 보여 줍니다. 인터럽트 유실·커널 timer·guest 대기의
정확한 원인, 기본 QEMU에서의 안정성, 실제 수신기의 생산 시각·품질과
제품 ASSIST 자격을 증명하지 않습니다. live ASSIST는 비활성이며 별도
차량 시험을 요청하지 않습니다.

원본 코드를 수정하지 않는 다음 판별은 WFI 중의 타이머/IRQ 전달과
guest thread 대기를 동시에 관찰하고, 커널 idle 옵션과 서비스 CPU
배치의 영향을 따로 비교하는 것입니다. Claude의 비도구 독립 검토도
WFI 표본만으로 인터럽트 유실과 정상 대기를 구별할 수 없다고 지적했고,
`nohlt`와 TCG 단일 스레드 비교를 가설 분리 시험으로 제안했습니다.
두 번째 검토의 일반적인 `mode >= 2` 제안은 이 원본 API의 0/1 mode
응답을 확인하지 않은 것이므로 제품 자격 조건으로 채택하지 않았습니다.

## IRQ 전달 구간 관측과 추가 음성 대조

원본 커널·진단 이미지를 유지한 다른 실행에서 QEMU simple trace의
`gic_set_irq`, `gic_update_set_irq`, `gic_acknowledge_irq`만 QMP로
짧게 켰습니다. 먼저 guest 질의 2가 진행 중인 3초에는 각각 1,286·
3,061·1,172건의 이벤트가 기록됐습니다. 이 양성 대조 구간의
`gic_set_irq`는 IRQ 87이 646건, IRQ 29가 640건입니다. IRQ 번호의
장치 의미는 이 실행에서 확정하지 않았습니다.

같은 VM에서 guest 질의 24 뒤 진행 표식이 15초간 바뀌지 않을 때
동일한 tracepoint를 다시 8초 켰지만 파일 크기가 226,856바이트로
그대로여서 선택한 GIC 이벤트는 **0건**이었습니다. 그 뒤 별도 QMP
`info registers`를 5초 간격으로 세 번 읽은 표본에서 두 vCPU가
앞선 WFI 지점에 머물렀고, 240초 제한까지 guest 완료는 없었습니다.
양성/정지 창의 시작·끝과 trace byte offset은 비공개 측정 기록에
보존했습니다. trace 자체가 guest 진행 타이밍에 영향을 줄 수 있으며,
WFI 프로그램 카운터의 반복 표본은 그 사이 잠깐 실행했다가 같은
위치로 돌아온 경우까지 배제하지 않습니다.

질의 스크립트에는 1초 D-Bus reply timeout, 2초 프로세스 timeout,
3초 sleep이 있고 공급기도 1초 sleep을 사용합니다. 정지 구간에
GIC IRQ가 8초간 하나도 기록되지 않은 사실은 이 가상 실행의
타이머/IRQ 발생·프로그래밍 경로를 우선 조사해야 함을 보여 줍니다.
다만 게스트가 마지막으로 어떤 clockevent를 예약했는지와 그 만료
시각을 관찰하지 않았습니다. 따라서 QEMU의 IRQ 유실, 원본 커널의
timer 재프로그래밍, 다른 서비스 대기 중 어느 것이 선행 원인인지
확정하지 않습니다. 이 기록을 차량의 타이머나 제품 코드 결함으로
해석하지 않습니다. trace SHA-256은
`f4041376baf974de452b7451fe525d07681a0aa25f676008a422a252363d79f8`,
해당 VM console SHA-256은
`1c7de9a22a6c042eff6e85d0a04fa2f1a0444394658251213a375c84c2d84f86`입니다.

`nohz=off`만 추가한 2-vCPU 실행도 질의 14 이후 제한 종료했고,
QMP 표본의 두 vCPU는 WFI 지점에 있었습니다. 그러나 원본 커널의
`CONFIG_NO_HZ` 설정을 확인하지 못해 해당 부팅 인자가 실제 idle
경로를 바꿨는지는 미확정입니다. `maxcpus=1` 실행에서는 부팅 초기에
원본 IPU 작업 스레드의 커널 Oops가 발생했습니다. 뒤에 guest 질의
30개가 완료됐지만 이 결함 있는 부팅을 CPU 수의 깨끗한 대조로
세지 않습니다. QEMU trace의 첫 시도에서는 진행 중 양성 기록만
얻고 정지 창을 읽기 전에 제한이 끝났습니다. 위 3초/8초 비교는
이를 보완한 별도 실행입니다.

비공개 이미지·콘솔·GDB·QMP 기록과 사후 검증기는
`evidence/oem-progress-20261001/`에 보존했습니다. nohlt 조건의
console SHA-256은 `pass`
`151405fcd87865dbc4b3700c00560c11acef5b700c1d4afb1927459663f70d8f`,
`mirror` `2c3bee1f4d2bf1e9d2775eb1faba6e76327abd88d4b7ae2527de6fb1ef41e52d`입니다.
이 작업은 제품 코드를 수정하지 않았으므로 전체 `make test`와 고정 ARM
suite는 다시 실행하지 않았습니다. 원본 VM 실행, 메타데이터·해시·
사후 검증기와 QMP/trace 양성·정지 구간 대조가 이번 검증 범위입니다.
