# 원본 LDS VM 타이머·IRQ 후속 대조 — 2026-10-01

[앞선 T1~T8 계측](LDS_TIMER_CAUSALITY_2026-10-01.md)은 원본 진단 VM의
정지와 GPT 비교값 경합을 기록했습니다. 이번 T9~T12는 같은 경합이
호스트 시간·vCPU 스레드 구성에 좌우되는지, 그리고 종료 직전 GPT
출력선까지 어느 단계가 관찰되는지를 분리했습니다. **어느 실행도
원본 진단 질의를 완료하지 않았으며 차량·폰 시험이 아닙니다.**

원본 커널 SHA-256은
`57fe0cda7c1f994a6ca6b3cce7d9cf247f3b5607ebf1ddc12f02598662d5e240`,
진단 initrd SHA-256은
`25d7cdb62ad042d6689ae0fe79a18ddcae7e31bde7cdd915b0c7847a2bb86d48`로
네 실행에서 같습니다. QEMU 7.2.22 `sabrelite`·vCPU 2개·RAM 1024MiB,
원본 서비스 9개와 유지 의존성 4개, 합성 NMEA PTY와 진단용 USB GPIO
`pass` 조건도 같습니다. 제품 DSO·물리 차량·휴대폰은 연결하지 않았고
호스트 네트워크·장치·공유 디렉터리도 연결하지 않았습니다. 변경은
비공개 **호스트 QEMU 계측**과 명시한 가속 옵션뿐입니다. GPT 기록은
메모리 순환 버퍼를 종료할 때 출력했고 실행 중 파일 trace는 0바이트입니다.

| 실행 | 변경 조건 | 제한·마지막 질의 | 관찰 |
| --- | --- | --- | --- |
| T9 | T8의 GPT ring QEMU에 `-icount shift=auto` | 120초·질의 전 | 원본 커널이 `clocksource_done_booting`에서 `mxc_timer1` 전환을 출력한 뒤 제한 종료. GPT 계산 10건뿐이며 LDS 경합 비교가 불가능했습니다 |
| T10 | 실시간 MTTCG, GPT MMIO의 vCPU·PC 추가 | 120초·13 | 마지막 OCR1은 같은 vCPU의 직전 TCN보다 254 tick 앞이었으나 QEMU 계산 시 87 tick 지났습니다. `next_int=0`·롤오버 timeout, 후행 TCN 읽기 미관측 |
| T11 | T10과 같은 QEMU, `-accel tcg,thread=single` | 120초·15 | 마지막 OCR1은 계산 시 203455 tick 미래였고 내부 GPT callback은 정확히 OCR1에서 실행됐습니다. 콜백이 실행 제한 직전이어서 후속 guest 동작은 판정할 수 없습니다 |
| T12 | 단일 TCG 스레드, GPT SR·CR·IRQ 출력 전이 추가 | 120초·11 | 마지막 OCR1은 같은 vCPU의 직전 TCN보다 254 tick 앞이었으나 계산 시 42 tick 지났습니다. 이후 GPT IRQ 출력은 low였고 추가 계산·질의는 없었습니다 |

T9의 `icount`는 QEMU의 가상 시계 기준을 바꾸고 동시에 다중 TCG
스레드를 끕니다([QEMU TCG 선택 코드](https://gitlab.com/qemu-project/qemu/-/blob/v7.2.22/accel/tcg/tcg-all.c)).
따라서 T9의 조기 정지는 원래 경합을 재현하거나 해소한 증거가 아닙니다.
고정 shift 비교도 수행하지 않았습니다.

T10의 마지막 32개 GPT 계산 중 OCR1 쓰기 19건을 대조하면, 18건에서
같은 vCPU 스레드의 후행 TCN 읽기가 기록됐습니다. 그 가운데 9건은
첫 후행 읽기의 TCN이 OCR1 **이전**이고, 9건은 OCR1 **이후**였습니다.
앞선 9건은 해당 GPT 비교 인터럽트 이후의 읽기일 수 없지만, 어떤
커널 호출인지와 OEM `-ETIME` 검사·재시도 여부는 입증하지 못합니다.
마지막 늦은 쓰기 뒤에는 제한 종료까지 후행 TCN 읽기가 관찰되지
않았습니다. MMIO에서 읽은 guest PC는 translation block의 시점과
어긋날 수 있어, 기록된 PC가 같다는 이유만으로 동일 함수·명령
경로라고 단정하지 않습니다. 비공개 로그의 주소는 공개하지 않습니다.

T11은 **미래 비교값의 예약과 내부 callback 실행**까지 확인합니다.
마지막 callback의 QEMU 가상 시각은 119.891초로 120초 실행 제한
직전입니다. 그 뒤 GPT 재계산이나 다음 질의가 없다는 사실은 관찰
창이 너무 짧아 정지 증거로 사용할 수 없습니다. 따라서 T11은 늦은
비교값이 원인인지에 대한 대조 결론을 내리지 못합니다. 이 실행에서는
status register의 OF1 설정, GPT 출력선 상승, GIC IRQ 87 전달과
vCPU 수락도 계측하지 않았습니다.

T12의 마지막 GPT 계산에서는 SR=0, IR=1, CR=0x289였고
`next_int=0`, timeout=`0xffffffff`였습니다. 앞선 GPT 출력선
상승·하강은 반복 기록됐지만 마지막 과거 OCR1 뒤에는 상승이
기록되지 않았습니다. **이는 T12의 마지막 비교값에 대한 QEMU GPT
출력 미발생**까지 확인한 것이며 GIC·CPU 결함 판정은 아닙니다.
호스트 경과 약 92~102초에 읽은 QMP 상태는 실행 중이었고
`info irq`·`info pic`은 빈 값을 반환했습니다. 마지막 GPT 사건과
동기화된 스냅샷이 아니므로
GIC IRQ 87의 pending/enable/active, CPU별 IRQ 마스크·WFI 상태를
판정하는 데 쓰지 않습니다. 제한 종료 시점의 QEMU 가상 시각도
측정하지 않아 마지막 비교값 이후의 가상 관찰 구간을 환산할 수 없습니다.

Claude Code에는 도구를 쓰지 않는 독립 비판을 요청했습니다. 특히
T10의 첫 후행 읽기는 타이머 IRQ **전후를 구분해야** 하고, T11의
예약 성공도 GIC 전달을 뜻하지 않는다는 지적을 받아 원본 QEMU
계측과 대조했습니다. T11의 내부 callback 실제 실행 여부는
QEMU의 callback 호출 기록으로 확인했습니다. Claude가 OEM 코드나
실차를 실행한 것은
아닙니다. 다음 오프라인 판별에는 질의 무진행 또는 마지막 GPT
사건과 동기화하여 GPT 출력→GIC IRQ 87 상태→vCPU 수락을 같은
실행에서 기록해야 합니다. 합성 NMEA·QEMU의 시간과 GPIO 조건은
실제 센서의 측정 시각, 출처, 위치 정확도, 폰 수용을 검증하지 않습니다.

비공개 원본 로그·QMP 스냅샷·진단 바이너리는
`evidence/oem-timer-20261001/`에만 보존했습니다. QEMU 바이너리
SHA-256은 T9
`430471d014d3538e89e2571d3c869544709973602700f7ab2b911af040dd57b6`,
T10/T11
`39b59df36bacb0ab1386c71db7fffbdb486959c5defde2cefb6702aa7f736d6f`,
T12
`758680fa69e358f7f9725c11d56d8a89eebfc4d2429141904d25cb75053e2dee`입니다.
콘솔 SHA-256은 T9
`4e3f420846a8329df71c76e62e49cd6fc2b4792686d77f23dd419fb966776e7a`,
T10
`ce2215a934ff98cc3b0b7f0520fbb7f53feea632ac468e544fde65d6f0d04a42`,
T11
`2c98b2da927a7a5a2a26b7f2bb22878d6a26af6099543c71aa1d1a67ff766e94`,
T12
`135d2aec4ec0112e97a7a25eced59a6fb9b7690a2f5c67f3bc117d133ec741ae`입니다.
T12의 QMP 기록 SHA-256은
`ce449504e524c7755888b3a27e1da0bd841d79adf58cbf0e6b838900beca3d5f`입니다.
네 실행은 모두 제한 종료했고 종료 코드 0이나 QEMU 내부 callback을
진단 완료로 세지 않습니다. 제품 코드 변경이 없어 `make test`와
고정 ARM suite는 다시 실행하지 않았습니다.
