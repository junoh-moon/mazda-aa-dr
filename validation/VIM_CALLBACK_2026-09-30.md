# 원본 VIM 등록·CAN 콜백 실행 — 2026-09-30

NA 74.00.324A의 원본 VIM/VBS와 `v0.3.1-shadow.1` production tap을
격리 ARM VM에서 실행했습니다. 실제 원본 등록과 명시적인 합성 입력의 전달을
확인한 기록입니다. 물리 센서·생산 시각·항법 정확도·휴대폰 수용을 검증한
기록은 아닙니다. 제품 소스와 게시 ZIP은 변경하지 않았습니다.

## 실제 등록과 실패 대조

진단 tracer → 배포 tap → 원본 API 순서로 호출했습니다. 원본 VBS가 제공한
callback의 소유자는 `libjcimod_can.so`, 원본 IPC에 등록된 wrapper의 소유자는
배포판 `libmx5dr-vimtap.so`였습니다.

| 실행 | 실제 반환 |
| --- | --- |
| 순정 커널의 VIM/VBS | VIMC_Open=100, VIMC_AddClient=100, IPCAPI_AddClient=0 |
| 별도 서버 없는 격리 VM | 원본 Open=102, 원본 및 tap 경유 AddClient=102 |
| 별도 null callback table | 원본 AddClient=104 |

errno=2는 해당 호출이 남긴 스레드 상태이며 성공/실패는 각 API 반환값으로
구분했습니다. 서버 없는 경우에도 tap route가 할당될 수 있으므로 route나
관측 준비 표식만으로 원본 등록 성공을 판단하지 않습니다. 실제 성공 실행에서는
원본의 44바이트 등록 enqueue와 VIM receive도 관찰했습니다.

## 합성 입력과 실제 전달

추가 진단 스레드는 원본 VIM 프로세스에서만 동작합니다. 실제 등록 성공,
AA worker 활성화, 원본 CAN ReadyHandler 진입을 관찰한 뒤 원본
`VIMS_AppEventCallback`을 세 번 호출했습니다. 원본 API가 application
envelope와 IPC fanout을 만들며 기존 클라이언트만 사용합니다.

| ID | 입력 길이 | 명시적인 정차 가정 fixture |
| --- | --- | --- |
| 0x100 | 9 | 네 wheel raw=10000 |
| 0x116 | 4 | yaw raw=2047, count=1 |
| 0x118 | 2 | reverse raw=0 |

이 값은 물리 정차를 측정한 결과가 아닙니다. 새 subscriber나 readiness/reset
제어 메시지, 원본 callback 교체, OEM 코드 변경, 성공 stub은 사용하지 않았습니다.

디버거 없는 `synthetic-r2`에서 원본 VIM의 57/52/50바이트 MQ send가 각각
반환 0, VBS의 대응 receive, tap의 64바이트 datagram 세 건을 확인했습니다.
실제 AA worker의 `motion_batch` 세 건이 각 raw 값과 관측 순번 1/2/3을
보존했습니다. 순번은 tap의 수신 순번이며 생산자의 순번이 아닙니다.
`source_mono_ms=0`, producer time unknown도 유지했습니다.

이 실행의 health 44건은 hook=true/audit_fault=0, SHADOW 355건은 모두
model_valid=false/E_NO_SEED였습니다. 센서 복사·수신은 검증했지만 유효한
위치 계산을 성공한 것은 아닙니다.

## 원본 CAN callback의 직접 실행 관찰

별도 `synthetic-gdb-r3`에서는 실제 runtime mapping을 확인하고 QEMU의
Z1 hardware breakpoint로 다음을 각 입력에 대해 관찰했습니다.

- 원본 CAN callback 진입과 정확한 client/server·합성 메시지 인자
- 원본의 정상 CAN event 경로 진입
- production tap으로 복귀한 PC와 원래 stack 복원
- 같은 원본 VBS clone thread의 MQ receive와 tap datagram send

스레드 귀속은 TLS와 clone stack으로 대조했습니다. Z1 설치/제거 각 9회가
확인됐고 모두 제거한 뒤 detach하여 journal과 inspection 종료까지 계속
실행했습니다. 관찰한 원본 entry 바이트도 전후 일치했습니다. runtime 관측기는
register/code를 쓰거나 Z0 software breakpoint로 대체하지 않았습니다.
공통 runner의 kernel-entry r1 machine-ID 조정은 별도 부팅 조건으로 남습니다.

같은 실행의 MQ send/receive, tap datagram, AA raw 기록이 각각 세 건이며
fixture 값도 일치했습니다. health 45건은 hook=true/audit_fault=0,
SHADOW 363건은 모두 model_valid=false/E_NO_SEED였습니다. tap은 원본 전달과
AA 수집용 복사를 함께 수행하며 AA worker는 비동기입니다. 이 기록으로
AA journal 작성이 원본 callback 복귀 뒤에 일어난다는 순서를 보장하지 않습니다.

이 실행은 91.935초에 관찰을 마쳤고 timed_out=false였습니다. VM 종료값으로
성공을 판정하지 않고 각 entry·인자·복귀와 별도 transport 기록을 대조했습니다.
디버거 stop/resume과 strace·추가 스레드는 타이밍을 바꾸므로 지연 상한이나
경쟁 부재의 검증이 아닙니다. 하위 ExtractData의 다인자 ABI는 호출·검증하지
않았습니다. 원본 CAN readiness의 peer timeout도 정상 차량 기동으로 세지 않습니다.

## 입력·실패 이력·검증 범위

순정 커널 SHA-256은
`57fe0cda7c1f994a6ca6b3cce7d9cf247f3b5607ebf1ddc12f02598662d5e240`입니다.
QEMU 7.2.22/Sabrelite, `nohlt enable_wait_mode=off`, 진단 PID1과 원본
standalone launcher를 사용했습니다. VM NIC·호스트 장치·공유 디렉터리는
없습니다. 원본 전체 SM 기동의 성공으로 해석하지 않습니다.

| 비공개 산출물 | SHA-256 |
| --- | --- |
| synthetic-r2 initrd | `5726f1a3e76baad927e63546bea8227d721a234344e3d17f1fb6686f8599f6d6` |
| synthetic-r2 console | `42e4c76837314fcbead80486fbbcbfd3475e311f1157e678c7786cfda2bae0ed` |
| synthetic-gdb-r3 initrd | `dfd072a90cc8ddaab0b2ef8f4df715215ec4597ffe7a1b3f6170fd1240e0d2f4` |
| synthetic-gdb-r3 console | `a9fe15de4af34a1e4a5cd4d8d1e063e377c0773794f6bfe91fd4941829c749cd` |
| synthetic-gdb-r3 observer | `56ba8cf2b5ad2e7e4fa5c6a3ed064ebd06c1fafbe7720d103f5b25c3d37a8a3b` |

실패한 진단도 보존했습니다. 첫 tracer의 RTLD_LOCAL symbol 해석 실패는 원본
등록 결과가 아닙니다. 초기 injector의 helper 프로세스 상속을 제한하고,
정확한 exe 경로 길이 비교도 고쳤습니다. 첫 요약기의 잘못된 health/shadow
필드명은 수정판에서 필수 필드 누락 검사와 함께 수정했습니다. 원래 r2
요약은 보존했으며 수정 요약의 필수 필드 누락은 0입니다.

GDB 첫 실행은 준비 표식을 관찰하지 못해 inconclusive입니다. 다음 실행은
QEMU XML include 처리 오류로 breakpoint·합성 입력 전에 끝났습니다.
관측기 파서를 수정한 세 번째 실행이 위 직접 실행 근거입니다. 앞선 두 번을
OEM 실패나 callback 성공으로 다시 분류하지 않았습니다.

진단 소스·재현 명령·요약·실패 이력은 작업 공간의 비공개 증거에 보관했고
28개 authored/요약 파일의 해시를 대조했습니다. OEM binary, 전체 console,
maps, register/memory dump는 공개하지 않습니다. 이 진단은 설치 USB에
포함되지 않으며 실제 센서 시간·보정, 정상 기동·복구, 폰 수용 및 ASSIST
연결은 [v1.0 완료 조건](../docs/V1_READINESS_KO.md)에 남습니다.
