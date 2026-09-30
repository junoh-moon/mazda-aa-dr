# 요청 경로 관측의 고정 ARM 자원 측정 — 2026-09-30

`e79ca52c95d04cddd10174af9a96b61bec2d374b`와
`d0c74d32f144cccfaa42cb5a338251477c659196`의 작성 소스를 비교했습니다.
새 기능 구현이나 제품 수정이 아니라 [요청 경로 변경](REQUEST_ROUTE_REVIEW_2026-09-30.md)의
자원 비용을 확인한 후속 기록입니다. x86 리뷰 수치를 ARM 수치로 재사용하지 않았습니다.

고정 GCC 4.9.1·ARMv7 Cortex-A9·NEON softfp를 사용했습니다. 두 커밋의 Makefile과
`src/`를 별도 복사하고 각 제품 빌드의 입력 해시와 대조했습니다. `sizeof` 측정
실행파일은 해당 sysroot의 `qemu-arm`에서 직접 실행하여 pointer width 32를
확인했습니다. ELF section/TLS는 이전에 검증한 실제 제품 두 개에서 읽었습니다.

## 타입과 정적 저장 공간

단위는 바이트입니다. 각 행은 독립적으로 더할 수 있는 메모리 항목이 아닙니다.
예를 들어 Observer에는 Ledger가, GpsHoldout에는 별도 Pipeline이 들어 있습니다.

| ARM 타입 | 이전 | route 반영 후 | 증가 |
| --- | ---: | ---: | ---: |
| Issue | 72 | 336 | 264 |
| Trace | 256 | 520 | 264 |
| Observation | 528 | 792 | 264 |
| Ledger | 33,376 | 67,168 | 33,792 |
| Observer | 33,408 | 67,216 | 33,808 |
| JournalQueue&lt;Observation,256&gt; | 137,248 | 204,832 | 67,584 |
| WorkerContext / WorkerScope | 264 / 280 | 528 / 544 | 각각 264 |
| Pipeline | 109,848 | 145,224 | 35,376 |
| GpsHoldout | 150,440 | 194,528 | 44,088 |
| MotionBatch | 6,160 | 6,160 | 0 |

| 실제 제품 ELF | 이전 | route 반영 후 | 증가 |
| --- | ---: | ---: | ---: |
| `.bss` | 198,180 | 299,588 | 101,408 |
| `.tbss` / PT_TLS memory size | 2,404 | 4,516 | 2,112 |
| `.text` | 154,876 | 155,956 | 1,080 |

queue와 Observer storage는 위 `.bss`에 이미 포함됩니다. TLS 값은 스레드별
제품 TLS image의 크기이며 loader 정렬·TCB·다른 모듈 TLS까지 합한 할당량이
아닙니다. `.bss`와 TLS를 CMU RSS 측정값이나 여유 메모리로 읽지 않습니다.
`.bss` 증가 101,408은 queue 67,584 + Observer storage 33,808 +
RequestBindings 16바이트로 설명됩니다. TLS 증가는 adapter의 context 여덟 개에
각각 추가된 Trace 264바이트와 일치합니다.

## 제품과 같은 객체의 스택 사용량

Makefile의 실제 compile 명령을 추출해 `-fstack-usage`만 추가했습니다.
runtime, request_trace, request_observer, adapter, request_hooks, session_hooks
여섯 translation unit을 두 버전에서 컴파일했습니다. **12개 객체 모두 기존
제품 빌드의 해당 `.o`와 SHA-256이 일치**했습니다. 계측 옵션을 붙인 별도
코드 생성 결과를 제품 수치로 간주하지 않았습니다.

GCC가 보고한 함수 자체의 local stack frame입니다. 모두 `static` 분류이며
호출한 다른 함수·OEM 코드·재귀·signal frame을 포함한 전체 최대치가 아닙니다.

| 함수 | 이전 | route 반영 후 |
| --- | ---: | ---: |
| runtime `worker_at` | 273,288 | 359,120 |
| `format_observation` | 2,344 | 4,640 |
| `format_request_trace` | 512 | 776 |
| Observer `request_begin` | 112 | 448 |
| Ledger `request_begin` | 128 | 392 |
| Ledger `worker_post` | 352 | 616 |
| Ledger `position_take` | 320 | 584 |
| adapter `position_enter` | 584 | 848 |
| adapter `send_vehicle_data` | 744 | 1,008 |
| `mx5_request_work_call` | 296 | 560 |

worker의 증가 85,832바이트에는 커진 항법/holdout 저장 공간과 JSON 관련 임시
공간이 반영됩니다. 타입 크기를 이 frame에 다시 더하면 중복 계산입니다.
기존 코드도 항법 큐를 worker 스택에 두고 있었으며, 현재 worker 생성은
`pthread_create`의 기본 attr을 사용합니다. 실제 CMU의 thread stack limit·
다른 호출을 포함한 최대 사용량·남은 여유는 이번 측정으로 확정하지 않습니다.

## 전체 관측값의 ARM 큐 경계 실행

검토자의 native fixture를 참고한 별도 작성 fixture를 같은 ARM 제품 compile
옵션으로 빌드했습니다. 실제 `JournalQueue<Observation,256>` 타입을 사용하고
원본 producer 변수를 enqueue 직후 덮어쓴 뒤 dequeue 값을 독립 기대값과
대조했습니다. 여섯 문자열 각각의 63바이트·NUL·known/complete, 요청/worker ID,
sequence와 원본 48바이트를 모두 검사했습니다.

- 정상 조건: 큐를 256개씩 채우고 비우는 네 회전, 1,024개 보존, 누락 0.
- full 조건: 같은 네 회전에서 매번 257번째 입력을 추가했습니다. 그 네 입력만
  FULL, 기존 1,024개 보존, 누락 계수 정확히 4였습니다.
- 두 조건 모두 종료 뒤 STOPPED와 drained를 확인했고 누락 계수도 유지됐습니다.

두 별도 프로세스가 exit 0이었습니다. 기존 review의 x86 결과를 ARM 실행으로
세지 않았습니다. 단일 producer의 크기·소유 복사·slot 재사용 검사이며, 새로운
동시성 전체 증명이나 실제 callback 처리 시간 측정은 아닙니다.

## 고정 근거와 한계

| 파일 또는 기록 | SHA-256 |
| --- | --- |
| 이전 제품 `libmx5dr.so` | `04e180226c9aa59bf4506e9ca0552917588d83fc07e48ef15a5b2f5b569887fb` |
| route 제품 `libmx5dr.so` | `b276b58b02282a359d9c37552f146848199483b3676355899cafb2bacf23da27` |
| ARM queue fixture | `fb8b123d598c9d9ae848f09106b2c1788a396a39fee00347ecb1537e9725058e` |
| `summary.json`: 타입·section·stack | `d8527c4ac2a43d0f17e71ebb008fb0d2f9c3d819195e4c4dc89c4d6824e13603` |

정확한 source snapshot·명령·`.su`·실행 로그는 ignored
`evidence/arm-route-resources-20260930/`에 있습니다. 초기 측정용 fixture가
구형 libstdc++에 없는 `is_trivially_copyable`을 사용해 컴파일에 실패한 기록도
보존했습니다. 측정 fixture만 기존 제품이 사용하는 no-throw copy trait로
바꾸어 재실행했으며 제품 컴파일 실패로 세지 않습니다.

이번 후속 단위는 제품 소스를 변경하지 않았고 전체 host/ARM suite, 새 원본 VM,
USB ZIP 설치·게시를 반복하지 않았습니다. 해당 제품의 기존 전체 검사·원본 VM
실행은 앞선 기록을 따릅니다. 실차 메모리 여유·thread stack 최대 사용량과
callback latency, 위치 정확도·폰 수용은 여전히 미검증입니다. 이 수치만으로
스택 크기나 큐 용량을 임의로 바꾸지 않았으며 v1.0 승인으로 판정하지 않습니다.

독립 리뷰어 한 명이 작성 소스 해시 102개, `.su` 해시 12개, 원시 로그와 공개
표를 대조했습니다. 수치·중복 합산·범위 해석의 확정 P1/P2는 없었으며 요약
JSON의 파일명을 명확히 했습니다. 새 ARM 실행은 root가 수행했고 리뷰어는
자료 검토만 했습니다. 최종 리뷰 SHA-256은
`599537d1cf3becc37312526d1e607cf69f32b906b4628ca496827c5ec0b43c18`입니다.

마지막 SSH fetch에서 외부 `4037b5be0e9856d240016764bcfd8bdf050dc69a`의
[버스 수명 관측 조사](https://github.com/junoh-moon/mazda-aa-dr/blob/4037b5be0e9856d240016764bcfd8bdf050dc69a/validation/BUS_CONNECTION_2026-09-30.md)를
확인했습니다. 이 변경은 다음 독립 검토 대상이며 이번 제품 측정에는 포함하지
않았습니다. 그 작성자의 원본 VM 실행도 이번 실행 수치로 가져오지 않았습니다.
