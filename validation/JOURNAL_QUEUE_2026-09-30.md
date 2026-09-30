# Journal 큐의 정상 동시 실행 손실 제거 — 2026-09-30

기준 `05ddd5f`의 journal 큐는 consumer가 잠금을 잡고 마지막 항목을 꺼내는
동안 producer가 들어오면, **빈 큐에서도 관측을 버리고 MODEL을 중단**했습니다.
작성 runtime의 실제 unlock 경계에서 이를 재현하고 잠금 없는 유한 큐로
교체했습니다. 최종 제품의 원본 VM 두 실행은 합성 raw 936·855건과 계산·종료
검사를 완료했고 journal drop·audit fault·요청 loss는 모두 0이었습니다.

이는 v1.0 완료나 차량·폰 검증이 아닙니다. 공개 ZIP은 `v0.3.1-shadow.1` 그대로이며
ASSIST와 qualified provenance는 비활성입니다. 원본 바이너리·전체 로그·비공개
fixture는 게시하지 않습니다.

## 재현한 결함과 과거 실패의 구분

[이전 요청 관측 검사](REQUEST_CONTENTION_2026-09-30.md)의 product-r1에는
`dropped=1`, `audit_fault=1`이 있지만 기존 counter는 trylock 실패와 용량 초과를
구분하지 않았습니다. 이 과거 실행의 정확한 원인을 사후 확정하지 않습니다.

이번 결정적 재현은 실제 작성 `runtime.cpp`의 pop unlock 직전만 정지합니다.
pop은 유일한 항목을 이미 복사·제거한 상태입니다. 새 sink 호출은 `qsize=0`인데도
drop 1·audit fault 1과 OBSERVE 복귀를 일으켰습니다. 큐 용량 부족은 아닙니다.

기존 코드에 **우리 큐의 busy/full 및 잠금 보유자 counter만** 추가한 진단 VM을
두 번 실행했습니다. 두 실행 모두 caller exit 0, 실제 LDS 위치 10건씩이며
busy/full은 0이었습니다. 계측의 시간 영향과 잠금 획득/해제 사이의 owner 미확인
구간이 있으므로 과거 실패가 재현됐다고 하지 않습니다. 최종 제품에는 이 진단을
넣지 않았습니다.

## 변경과 동시 실행 계약

- 고정 256개 슬롯과 하나의 consumer를 유지합니다. producer는 atomic64의
  CLOSED/count에서 용량을 먼저 예약한 뒤 atomic32 ticket을 받습니다.
- 각 슬롯에 Observation 전체를 소유 복사한 후 ready를 release 공개합니다.
  consumer는 ticket 순서의 ready를 acquire 확인하고 복사를 끝낸 뒤 슬롯과
  용량을 반환합니다. producer끼리 겹쳐도 mutex trylock 손실은 없습니다.
- ticket RMW의 acquire/release는 예약 후 오래 지연된 producer에게 그 사이
  발생한 슬롯 소비·재사용 순서를 전달합니다. 단순 relaxed ticket으로 바꾸면
  이 메모리 순서 근거를 잃습니다. uint32 wrap과 용량 256의 나눗셈 관계도 검사합니다.
- FULL 시도는 ticket을 발급받지 않습니다. 손실 counter와 sticky loss를 먼저
  공개하고 실패 예약을 반환합니다. 실제 포화 이후 아직 반환되지 않은 실패
  시도는 일시적으로 용량을 차지할 수 있으나 최초 포화를 만들어내지는 않습니다.
- close는 새 성공 예약을 금지합니다. close 전에 예약했지만 아직 복사하지 않은
  항목도 종료 판단에 포함합니다. worker만 최대 100회의 배출·1ms sleep 시도를
  수행하고, 끝나지 않으면 `capture_incomplete`와 완료 표식 부재로 끝납니다.
  `capture_end`와 durable `capture.done`은 closed/count 0 확인 뒤에만 작성합니다.
- FULL 반환 뒤 sink의 fault 설정이 지연돼도, 최종 worker가 큐의 sticky loss를
  다시 반영하므로 마지막 health가 손실을 숨기지 않습니다.

callback에 allocation·I/O·sleep·blocking lock·소스 CAS 재시도 루프를 추가하지
않았습니다. errno와 원본 forwarding 경로는 유지했습니다. compiler의 exclusive
원자 재시도나 OS 선점까지 포함한 wait-free·엄격한 시간 상한을 주장하지 않습니다.
용량 포화, journal I/O 실패와 incomplete 관측의 검사는 제거하지 않았습니다.

독립 리뷰는 전역 큐의 동적 생성자가 초기 preload 호출 뒤에 실행될 수 있는
새 회귀 위험을 발견했습니다. 큐와 슬롯을 constexpr로 초기화하고 실제
Observation의 constant initialization을 검사했습니다. 고정 ARM `runtime.o`에는
큐의 `_GLOBAL__sub_I`, `.init_array`, `.ctors`가 없습니다.

## 검사와 독립 리뷰

최종 GNU host `make test`는 Python 270개와 C/C++ 검사를 생략 없이 통과했습니다.
고정 GCC 4.9.1의 전체 ARM 검사도 생략 없이 통과했습니다. 실제 제품 DSO의
기존 위치 wrapper 8개·요청 wrapper 13개 예외/취소 검사도 포함합니다.

큐 회귀 다섯 그룹은 consumer 복사 정지 중 여유 슬롯의 정상 입력, 미공개 head의
FIFO와 종료 거부, 정확한 용량·ticket wrap, 실제 Observation의 소유 복사,
4개 producer의 25,600건 전달을 검사합니다. 최종 소스의 native·ASan/UBSan·TSan
실행도 독립 리뷰에서 통과했습니다.

별도 GNU 경계 회귀 일곱 그룹은 작성 runtime TU만 `-fno-inline-atomics`로
컴파일합니다. **테스트 실행 파일에서만** static libatomic 연산을 감쌉니다.

1. 이른 constructor의 관측이 이후 C++ 생성자 단계에서도 유지됩니다.
2. 예약 후 ticket 전에 지연된 producer가 다른 1,025건의 입력/소비 뒤 재개합니다.
3. FULL 손실 공개 전에는 마지막 슬롯을 배출해도 완료로 표시하지 않습니다.
4. ready 공개 직후 producer가 멈춰도 배출·완료가 가능합니다.
5. 초기 closed 확인과 예약 사이에 close되면 그 입력은 STOPPED입니다.
6. 실제 runtime의 미완성 예약은 종료 한도 뒤 incomplete/no-ack이고 정상 세션은 완료됩니다.
7. 실패 예약 반환 후 sink fault 호출 전에 종료해도 마지막 health의 audit fault는 1입니다.

일곱 그룹은 host와 ARM 모두 통과했습니다. Clang은 명시적 exit 77로 이 GNU
계측을 생략합니다. 최종 전체 host는 GNU여서 생략하지 않았으며 ARM runner는
생략을 성공으로 취급하지 않습니다. 제품의 빌드 flags나 원자 연산을 시험용으로
바꾸지 않았습니다.

독립 리뷰에서 조기 consumer 용량 반환 변형이 기존 검사를 통과하는 공백을
찾았습니다. consumer가 복사 중인 슬롯까지 찬 상태에서 다음 입력이 FULL인지
검사하도록 보강했습니다. 최종 검사에서 조기 ready, FULL의 잘못된 ticket 발급,
성급한 drained, 조기 consumer 용량 반환의 네 변형을 거부했습니다. 주 에이전트의
GNU 검사도 동적 전역 생성자와 최종 sticky 손실 반영 삭제 변형을 각각 exit 134로
거부했습니다.

relaxed ticket 변형은 sanitizer에서 검출되지 않았습니다. 동기화한 시험 scheduler가
추가 happens-before를 만들 수 있으며 별도 완화된 scheduler에서도 검출하지
못했습니다. TSan 성공을 C++ 메모리 순서의 증명으로 쓰지 않습니다. 별도 SC 모형의
19,562개 상태·62,512개 전이에서 반례가 없었던 결과도 실제 C++ 실행과 구분합니다.

설계, 테스트 거짓 통과, 고정 ARM, 최종 코드 검토의 네 독립 리뷰를 완료했습니다.
설계 리뷰의 constant initialization 수정, 테스트 리뷰의 조기 용량 반환 검사,
ARM 리뷰의 아래 장벽 수정과 최종 리뷰의 STOPPED 종료 한계를 반영했습니다.
최종 검토 범위에서 미해결 P1/P2를 발견하지 못했다는 결론이며 모든 실행의
무결함 보증은 아닙니다. 각 리뷰의 소스 해시·명령·실행 범위를 별도로 보존했습니다.

독립 ARM 검사는 state offset 0·loss counter offset 16과 8바이트 정렬,
atomic64 lock-free를 확인했습니다. 64비트 fetch_add/sub/or는 LDREXD/STREXD와
DMB로 생성되며 작성 `runtime.o`와 독립 빌드 DSO에 외부 `__atomic`/`__sync`
helper·libatomic 의존성이 없습니다. 기존 epoch load/store 검사 결과를 RMW
검증으로 재사용한 것이 아닙니다.

이 검토는 고정 GCC 4.9.1이 **64비트 acquire load 뒤의 장벽을 생성하지 않는
문제**도 발견했습니다. 최소 작성 probe와 실제 `runtime.o` 모두 state의 LDREXD와
후속 lost의 LDRB 사이에 DMB가 없었습니다. QEMU 실행은 통과했지만 실제 ARM의
약한 메모리 순서에서 최종 손실 공개 계약을 입증하지 못하는 코드였습니다.
state/손실 counter의 64비트 load 세 곳을 seq_cst로 강화했습니다. bool ready/lost,
32비트 ticket과 64비트 RMW는 별도로 생성 명령을 확인했습니다. 다른 작성 소스의
64비트 epoch 읽기는 이미 seq_cst였고 이 변경 대상이 아닙니다.
수정 뒤 독립적으로 재빌드한 실제 runtime의 순서는
`DMB → LDREXD(state) → DMB → LDRB(lost) → DMB`이며 필요한 후행 장벽을
확인했습니다. 독립 제품 DSO는 아래 최종 제품과 바이트 단위로 일치했습니다.
주 에이전트의 host/ARM 전체 검사와 아래 새 VM 두 실행을 모두 다시 수행했습니다.
독립 native/sanitizer·네 변형 검사와 주 에이전트의 두 GNU 변형 검사도 최종
header로 재실행했습니다. 물리 ARM에서 순서 위반 자체를 재현한 것은 아닙니다.

## 실제 제품의 원본 VM

정확한 NA 74.00.324A 커널/rootfs의 기존 격리 VM에서 원본 VIM/settings/VBS/LDS/
aap_service, 기존 VBS callback, 원본 AA manager/queue와 실제 제품 DSO를
실행했습니다. 센서·GPS 입력만 합성하며 주 에이전트가 직접 실행했습니다.
userspace debugger·호스트 NIC/장치/공유 폴더는 없습니다. 진단 PID1이며 정상
차량 전체 boot를 입증하지 않습니다.

| 최종 제품·같은 이미지 | product-r3 | product-r4 |
| --- | ---: | ---: |
| caller 검사 / 종료 | 9,108 / 0 | 8,609 / 0 |
| 연속 raw 관측 | 936 | 855 |
| 유효 MODEL snapshot | 68 | 68 |
| 실제 LDS mode 0 위치 / 명시적 합성 GPS | 10 / 4 | 9 / 4 |
| journal drop / audit fault / 요청 loss | 0 / 0 / 0 | 0 / 0 / 0 |
| 현재 boot durable capture 완료 | 확인 | 확인 |

기존 독립 검증기를 수정하지 않고 정차 영점·직진/회전·yaw 단절·새 GPS 기준점
복구, raw와 합성 API의 일대일 대응, preview 48바이트와 E7/속도/파생 UTC를
대조했습니다. 일정 속도·각속도 해석식 잔차 최대 약 1.49e-6m는 **합성식 일치**이며
물리 위치 정확도 수치가 아닙니다.

두 실행 모두 native LOCATION 송신은 0건입니다. 일반 분석기도 의도한 단절·
reset·holdout abort와 LOCATION 부재의 여섯 항목으로 exit 2/inconclusive입니다.
시나리오 검증 성공으로 이 판정을 완화하지 않았습니다. runner timeout 124와
QEMU 종료 0 자체도 통과 근거가 아닙니다.

| 고정 산출물 | SHA-256 |
| --- | --- |
| 최종 queue header | `105375c316af712727936afcbf5ebf05b641f468d22f0f5269186a3d0f85e07b` |
| libmx5dr.so | `ed6821129a3b437e484a212a546c96a81259578f97ccbd5dea337fd041376832` |
| 최종 외부 caller | `9aec4bc0cd045f2d682fe90be83aa2bff54b5319870dc7c661f669d8da306fc7` |
| 두 실행의 initrd | `8677cb908f0872bbe89f2af17b9ec9abb0a9ede1b6bfee901ee0c6b723899724` |
| product-r3 console | `daebbda8bcd593ddf94f08f37e566044a1f6043df7b383bceea8ad97382c6556` |
| product-r4 console | `b5cc6ea267e0d01e13d71651259be1b87d0e85e79e6fa03ae7086d3bf0e7c40f` |
| 독립 검증기 | `7f7335bdb78fff7d10ba60061abad1ecfe83defd5ecbc74605fa66ef27837e12` |

장벽 수정 전 product-r1/r2도 raw 929·990건, journal/요청 loss 0과 시나리오 검사를
통과했습니다. 하지만 후속 생성 코드 검토에서 문제가 발견됐으므로 이 두 실행을
최종 바이너리의 근거로 쓰지 않습니다. 이전 제품은
`4fdf5d7f2115af27ce896f01e5b42681b7dde4807cf30d0aa5dc9cbe2bd1fef4`, 이전 initrd는
`40b5348aa77122c63b0581263224b9387bf0f725047b74b9ec8df8e2f0aeca79`입니다.
이전 전체 검사와 VM 기록도 삭제하거나 최종 결과로 덮어쓰지 않았습니다.

## 독립 작업의 새 기록 확인

SSH fetch로 독립 브랜치의 새 자료를 확인했습니다. 아래는 다른 작업의 기록을
읽은 결과이며 이번 주 에이전트 또는 리뷰어가 같은 실행을 했다는 뜻이 아닙니다.

- `bcf69f4`의 [펌웨어 재생 기록](https://github.com/junoh-moon/mazda-aa-dr/blob/bcf69f44798e56fc97865dc59fb37af4eb75c062/validation/FIRMWARE_REPLAY_2026-09-30.md)은
  같은 원본의 부분 OEM 실행과 mode 0/READ_NOT_READY를 확인했습니다.
- `76ea17a`의 [원본 세션 callback 기록](https://github.com/junoh-moon/mazda-aa-dr/blob/76ea17ac922b454210b578008c8ea96ea664a2f0/validation/SESSION_EVENTS_2026-09-30.md)은
  Create/Start/Send의 반환 0과 실제 INVALID callback을 구분했습니다.
  실제 폰 연결·세션 자격·제품 연결의 증거는 아닙니다.
- `098662b`의 [상태 조회 병행 회귀](https://github.com/junoh-moon/mazda-aa-dr/blob/098662b31ffccf73a7980dece062ac36a2c6c0aa/validation/REQUEST_STATUS_POLL_2026-09-30.md)는
  상태 조회 두 스레드와 요청·worker lifecycle의 경합에서 기존 실패와
  `05ddd5f` 수정 후 통과를 보고했습니다. 이 회귀의 소스도 읽었습니다.
- `fb8f215`에는 펌웨어 파일 기반 개발과 실험 설치 요청을 제한하는 문서 변경이
  있습니다. 이 checkpoint는 위 브랜치를 병합하거나 새 probe를 제품에 연결하지
  않습니다. 후속 소스 검토와 통합 검사를 별도 작업으로 진행합니다.

## 남은 범위

과거 product-r1의 busy/full 구분은 미확정입니다. 정상 consumer/producer mutex
경합 경로는 제거했지만 임의 실행의 무손실을 보장하지는 않습니다. 오래 멈춘
producer, 실제 포화, I/O 실패는 여전히 불완전한 세션을 만듭니다. close를
가로지른 STOPPED 시도가 count 반환 전에 정지하면 새 데이터 손실 없이도
보수적으로 종료 확인이 실패할 수 있습니다. 예약을 시간만으로 회수하지 않습니다.

요청 관측기의 같은 표 안 경합, 물리 센서 단위·부호·생산 시각·품질, 유효 GPS,
정상 전체 기동·물리 복구와 실제 폰 수용은 별도 미완료 조건입니다. 이번 작업에서
Claude를 직접 실행하거나 실차를 사용했다고 주장하지 않습니다.
비공개 원본 실행과 실패·리뷰 원문은
`evidence/journal-queue-20260930/`에 보존했습니다.
