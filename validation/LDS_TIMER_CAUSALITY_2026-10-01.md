# 원본 LDS 진단 VM의 타이머 정지 경계 — 2026-10-01

[앞선 WFI·GIC 기록](LDS_WFI_REPLAY_2026-10-01.md)은 진단용 CPU
유휴 방지 루프를 뺀 VM에서 원본 LDS 질의가 멈추고, 두 vCPU가 WFI에
머무는 표본과 정지 뒤 GIC 이벤트 0건을 보존했습니다. 이번에는 같은
증상에서 QEMU 가상 시계, 타이머 예약·만료, 개인 타이머 MMIO 쓰기,
GPT 비교값을 직접 구분했습니다.

모든 실행은 QEMU `sabrelite`, vCPU 2개, RAM 1024MiB, 앞선 기록과
같은 원본 서비스 9개·유지한 의존성 4개, 합성 NMEA PTY와 진단용 USB
GPIO `pass` 조건입니다. 제품 DSO·폰은 연결하지 않았습니다.

공식 QEMU 7.2.22 소스 압축 파일의 SHA-256은
`c74b398c1950526686cb7784e23bd8945ef138b0e6541ba0495015aad3ac9532`입니다.
일회용 ARM64 Debian 12 컨테이너의 GCC 12.2.0으로 `sabrelite`
대상만 빌드했습니다. 계측 변경은 **호스트 QEMU**의 `ptimer`, i.MX
GPT, Cortex-A9 개인 타이머 tracepoint와 `info status`의 가상 시계
출력에 한정했습니다. 제품 소스, OEM 커널·서비스 ELF, 기본 진단
initrd는 고치지 않았습니다. 게스트에 호스트 네트워크·장치·공유
디렉터리를 연결하지 않았습니다.

모든 실행의 원본 커널 SHA-256은
`57fe0cda7c1f994a6ca6b3cce7d9cf247f3b5607ebf1ddc12f02598662d5e240`입니다.
T3 이외 실행의 진단 initrd SHA-256은
`25d7cdb62ad042d6689ae0fe79a18ddcae7e31bde7cdd915b0c7847a2bb86d48`입니다.
공식 [QEMU i.MX6 보드 연결](https://gitlab.com/qemu-project/qemu/-/blob/v7.2.22/hw/arm/fsl-imx6.c)과
[GPT IRQ 정의](https://gitlab.com/qemu-project/qemu/-/blob/v7.2.22/include/hw/arm/fsl-imx6.h)에
따라 외부 GPT 입력 55는 GIC IRQ 87입니다.
[Cortex-A9 연결](https://gitlab.com/qemu-project/qemu/-/blob/v7.2.22/hw/cpu/a9mpcore.c)은
개인 타이머를 PPI 29에 연결합니다. 원본 guest의 `/proc/timer_list`도
CPU별 `local_timer`와 `mxc_timer1` broadcast clockevent를 표시했습니다.

| 실행 | QEMU SHA-256 앞 12자리 | 제한·마지막 질의 | 직접 관측 |
| --- | --- | --- | --- |
| T1, 타이머·GIC | `ccb27e7cccb7` | 210초·6 | 가상 57.383초 뒤 개인 타이머 예약·IRQ 중단. 가상 시계는 208.534초까지 증가. GPT 내부 콜백은 201.009초에 실행됐지만 GIC IRQ는 발생하지 않음 |
| T2, 개인 타이머 MMIO 추가 | `8c0eab59ea70` | 125초·15 | CPU 0·1의 개인 타이머 제어 레지스터에 각각 `0x5`→`0x0` 쓰기. 마지막 정지는 가상 93.964초, 시계는 121.133초까지 증가. GPT 다음 내부 만료는 208.647초 |
| T3, guest timer_list 추가 | `8c0eab59ea70` | 150초·17 | 1·5·10·15번 질의에서 CPU별 clockevent와 broadcast 전환 관측. 정지는 가상 101.833초, 시계는 148.319초까지 증가. GPT 다음 내부 만료는 210.353초 |
| T4, GPT 비교값 MMIO 추가 | `aa03bbf8c5eb` | 160초·서비스 질의 전 | 가상 31.122초에 OCR1에 쓴 값이 QEMU가 읽은 현재 count보다 220 tick 뒤처졌습니다. QEMU는 `next_int=0`, 다음 만료를 롤오버 쪽 195.440초로 잡았고 시계는 157.565초까지 증가했습니다 |
| T5, GIC trace 제거 | `aa03bbf8c5eb` | 150초·서비스 질의 전 | GPT·개인 타이머 trace만 남겨도 부팅 중 과거 OCR1과 롤오버 예약이 재현됐습니다. 시계는 148.117초까지 증가했습니다 |
| T6, 무이벤트 GPT 기록 | `571d7c4de32a` | 150초·5 | 파일 trace를 모두 끄고 마지막 GPT 계산 32건만 종료 시 출력했습니다. 질의 5 뒤 기록된 OCR1은 당시 count보다 171 tick 뒤였고, 늦은 QMP 표본의 시계는 98.579→107.602초 증가했습니다 |
| T7, CPU 스레드별 TCN 읽기 결합 | `2ca29e3ba2b8` | 120초·17 | 마지막 같은 스레드 TCN 읽기보다 OCR1은 **254 tick 앞**이었지만, QEMU가 계산할 때 count는 OCR1보다 **295 tick 앞**이었습니다. 가까운 timeout 대신 `0xffffffff`, `next_int=0`이 선택됐습니다 |

QEMU 실행 파일의 전체 SHA-256은 T1
`ccb27e7cccb785b4f076c644ecfb3be9d8e57680d350fb689cd57fe0e8dddba9`,
T2/T3 `8c0eab59ea700fd1435f2d7e035f950dfa5b99e6fb21023862ba2738192d8b94`,
T4/T5 `aa03bbf8c5ebc6550a318af790d335e3a0ff4a28004a029c634c7034652007b4`,
T6 `571d7c4de32ae793a46530c58124c3c43af234acf10d5661fe67e47647e12211`,
T7 `2ca29e3ba2b8c1d264b61202e147b34094832e1334aa7d3b33c8c16ece75b41b`입니다.
각 실행 직전 해시를 기록했습니다. 빌드 출력은 단계마다 덮어써서 중간
실행 파일은 남지 않으며, 마지막 T7 바이너리와 최종 계측 소스·원본
QEMU 압축 파일만 비공개로 보존했습니다.

T1·T2·T4~T7은 같은 원본 진단 initrd입니다. T3만 게스트 `/init`에
`/proc/timer_list` 출력 일곱 줄을 추가한 별도 이미지
(`055ececa8601689e94c486f54c6cfc9c8ba6ce9314fc45524966487253fe9163`)입니다.
T3의 첫 목록에서 `mxc_timer1` broadcast의 다음 event는 무한대로
표시됐고, 두 `local_timer`는 활성화돼 있었습니다. 질의 5와 10에는
`tick_broadcast_oneshot_mask`가 각각 CPU 0과 CPU 1을 가리키면서
`mxc_timer1`의 가까운 다음 event가 기록됐습니다. 질의 15에도 CPU 0의
broadcast event가 약 2ms 뒤로 예약됐습니다. 따라서 진행 중에는
CPU별 타이머와 GPT broadcast가 모두 사용됐습니다. **정지 뒤의 guest
timer 목록은 수집하지 못했습니다.**

T1에서 개인 타이머 두 개가 멈춘 뒤에도 가상 시계는 흐르고 GPT의 먼
내부 만료만 실행됐습니다. T2에서 두 개인 타이머 정지는 guest의
MMIO 제어값 `0x0` 쓰기와 직접 연결됐습니다. 그러므로 이 표본을
"가상 시계가 얼었다"거나 "예약된 개인 타이머 콜백을 QEMU가 잃었다"고
설명할 수 없습니다. 다만 왜 guest가 가까운 wakeup event 없이 두 타이머를
비활성화했는지는 아직 분리되지 않았습니다.

T4는 서비스 진입 전의 별도 정지입니다. QEMU
[imx_gpt_find_limit()](https://gitlab.com/qemu-project/qemu/-/blob/v7.2.22/hw/timer/imx_gpt.c)은
현재 count보다 큰 비교값만 가까운 timeout 후보로 고릅니다. T4의 OCR1
쓰기 `0x28879516` 뒤 QEMU count는 `0x288795f2`였고, 선택된 timeout은
`0xffffffff`, `next_int=0`이었습니다. 이 실행에서는 대량 계측으로
부팅 시점부터 타이밍이 달라졌고 원본 서비스 질의가 시작되지 않았습니다.
이미 지난 비교값을 다음 롤오버로 보내는 모델 동작은 확인했습니다.
하지만 [상류 i.MX clockevent 드라이버](https://android.googlesource.com/kernel/common/+/3c4cfadef6a1665d9cd02a543782d03d3e6740c6/arch/arm/plat-mxc/time.c)는
모드 전환 때 의도적으로 OCR을 `counter - 3`으로 써서 먼 미래를
표시합니다. OEM 커널의 동일 구현 여부와 T4가 이 모드 전환인지
확인하지 않았으므로, **과거 비교값만으로 QEMU의 만료 누락 결함을
선언하지 않습니다.** 이 사건이 T1·T2의 늦은 정지와 같은 원인인지도
증명하지 못했습니다.

T5는 GIC 이벤트의 대량 파일 쓰기를 제거해도 T4처럼 부팅 중 멈췄습니다.
그래서 T6·T7은 QEMU 파일 trace를 모두 끄고 GPT의 최근 계산만 메모리
순환 버퍼에 남겼습니다. 두 실행은 각각 질의 5·17까지 진행한 뒤 제한
종료했지만 완료하지 못했습니다. T6의 전역 마지막 TCN 값은 다른 CPU
읽기와 섞일 수 있어 비교에 쓰지 않았습니다. T7은 **같은 TCG CPU
스레드**의 최근 TCN 읽기를 OCR1 쓰기와 연결했습니다. 마지막 대상값은
`0x6ed3e80c + 254 = 0x6ed3e90a`였고, QEMU 계산 시 count는
`0x6ed3ea31`이었습니다. 이 값은 직전 읽기보다 `-3`인 단순 모드
전환 표식과 맞지 않으며, 약 254 tick의 가까운 event가 QEMU 계산
전 이미 지나갔음을 보여 줍니다. 그 뒤 추가 GPT 계산 기록은 없고
guest 질의도 재개되지 않았습니다. T7에서는 GIC trace를 켜지 않아
IRQ 건수를 직접 세지 않았습니다.

이 사실은 **이미 예약된 QEMU 타이머 콜백의 유실**과 다릅니다.
QEMU는 계산 시 지나간 OCR1을 가까운 예약으로 선택하지 않았습니다.
OEM `v2_set_next_event`의 정확한 호출 경로, OCR1 쓰기 뒤 TCN 재읽기,
`-ETIME` 반환과 kernel clockevent 재시도는 아직 관찰하지 않았습니다.
따라서 이 한 건을 QEMU 제품 결함이나 차량의 타이머 결함으로
일반화하지 않습니다. 다만 계측 부하가 줄어도 원본 VM의 WFI·무IRQ
정지가 반복되며, 가까운 GPT 비교값이 시간 경합으로 무효화되는
구체적인 경계를 포착했습니다.

Claude Code의 별도 비도구 검토는 MMIO 비교값·현재 카운터·QEMU가 선택한
deadline을 같은 VM에서 맞춰 보라고 제안했습니다. 후속 논의에서 상류
드라이버의 `counter - 3` 모드 전환 표식을 지적했고, 이를 T7의 CPU별
읽기 대조로 분리했습니다. 이 제안을 그대로 결론으로 쓰지 않고
원본 QEMU 소스와 비공개 trace·ring 기록을 직접 대조했습니다. 이 작업은
실차 또는 폰을 실행하지 않았고, ASSIST 입력 자격이나 위치 정확도를
검증하지 않습니다. 기본 QEMU의 정상 완주도 아직 입증되지 않았습니다.

비공개 QEMU 빌드 패치·guest 이미지·전체 콘솔·trace·QMP 시계 기록은
`evidence/oem-timer-20261001/`에만 보존했습니다. T1/T2 trace SHA-256은
각각 `06ff451e69ea44aa56760c8914654d4aa6cc4a24e6d4da113e3a533816fdd44d`,
`ebfe7c9f87f3af200cf1817b9bbc1120cbf43181f507474e1c73afaf9d50d32f`이고,
T3/T4는 각각 `79f481626290374a6559150c03134bd26d94cd6c090cbca4bccb0cdcb64b9c76`,
`ada64858d0a9040b95ad2f4c5ca46b0048447d2fe904512328bd83e06d5d0529`입니다.
T5 trace SHA-256은
`c5b2f54a85cd7cb11572cc812894f1a66fd3a2999984f31e9a076a72ebea4329`,
T6/T7의 비공개 console SHA-256은 각각
`472abfbd9a582471be3213941a37ed2364cc0c49059dd4a20c9c1a7cd4ac3673`,
`5c884fbd1d1f469cf0ebfe4e6547444690b8aef3203f1c249d850cb3c0aaa07d`입니다.
T1~T7은 모두 제한 종료했고 guest 완료 표식이 없습니다. 반환값 0을
완료로 세지 않습니다. 제품 코드를 바꾸지 않아 `make test`와 고정 ARM
suite는 이번에 다시 실행하지 않았습니다.
