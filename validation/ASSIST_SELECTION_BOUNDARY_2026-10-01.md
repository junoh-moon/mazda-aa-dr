# ASSIST 선택 후 generation 철회 경계 — 2026-10-01

**선택을 마친 송신은, 원본 함수 진입 전에 generation이 바뀌어도 이미 선택한
DR payload를 전달할 수 있습니다.** 변경하지 않은 adapter 소스와 실제 pthread
철회로 이 경계를 재현했습니다. 현재 구현은 마지막 generation 검사를 선택의
기준 시점으로 명시하며, 이후 OEM teardown 동기화까지 보장하지 않습니다.
이번 결과는 그 계약의 확인이며 제품 결함 수정이나 live ASSIST 허용 근거가 아닙니다.

## 대상과 작성 조건

- 소스: `56a704236139b45ccdc8b1c56c992997f6a895de`.
  `src/adapter/adapter.cpp`의 실행 전후 SHA-256은
  `8be7f8ff2f76171bc64e280cea04693fa5218d3e52b7748c7ef468eb22650a4d`로 같습니다.
- 기존 host GCC 13.3.0에서 해당 소스를 수정하지 않고 private fixture와
  빌드했습니다. 실제 배포 ARM DSO를 실행한 검사는 아닙니다.
- 작성한 qualification, 고정 clock, 유효한 DrSnapshot을 사용했습니다.
  발행과 철회는 각각 별도 pthread에서 실제 `publish_snapshot()`과
  `invalidate()` API로 실행했습니다.
- 기존 48바이트 `memcpy` 호출을 link wrapper로 관측했습니다. 원래 복사를
  수행한 뒤 선택 전 또는 선택 후의 지점에서 최대 5초의 작성 barrier로
  송신 스레드를 멈추고, 철회 API의 반환을 확인한 다음 재개했습니다.
- 빌드의 관측 관련 옵션은 `-O2 -g -fno-builtin-memcpy -fno-pie -no-pie
  -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wl,--wrap=memcpy`입니다.
  private host 실행파일에서 fortified builtin의 인라인화를 막기 위한
  조건이며, 제품 소스·빌드 설정·펌웨어 검사는 변경하지 않았습니다.
- 원본 송신 대역은 받은 인자·payload·진입 generation만 기록하고
  `-713`을 반환했습니다. 이 함수 안에서 철회하거나 callback을 주입하지
  않았습니다. 실제 OEM 송신 함수를 실행한 것으로 세지 않습니다.

## 실행 결과

세 대조의 선택 대상 generation은 모두 3입니다.

| 작성 스케줄 | 철회 후 generation | 원본 대역 진입 generation | 실제 선택 / 사유 | 원본 호출 수 |
| --- | ---: | ---: | --- | ---: |
| 철회 없음 | 해당 없음 | 3 | DR_REPLACEMENT / PASS | 1 |
| 선택 전 복사에서 철회 완료 | 4 | 4 | ORIGINAL / EPOCH_MISMATCH | 1 |
| 선택 후 복사에서 철회 완료 | 4 | 4 | DR_REPLACEMENT / PASS | 1 |

세 대조는 각각 exit 0입니다. 철회 대조는 원본 대역의 호출 수가 아직 0일 때
`invalidate()`가 3→4를 반환했음을 barrier 안에서 확인했습니다. 복사 호출의
return PC를 소스 위치와 대조해 선택 전은 `adapter.cpp:255`, 선택 후는
마지막 generation 검사 뒤의 `adapter.cpp:276`임을 확인했습니다.
시각 근접성으로 실행 순서를 추정한 결과가 아닙니다.

모든 경우 원본 입력 payload는 바뀌지 않았습니다. 선택 전 철회는 원본
wrapper와 bytes를 그대로 전달했고, 선택 후 철회는 이미 만든 대체 wrapper와
bytes를 전달했습니다. 원본 대역 진입의 `errno=EDOM`, 반환값 `-713`과
반환 뒤 `errno=EINPROGRESS` 보존도 확인했습니다.

별도 부정 검사는 “철회가 원본 함수 진입 전에 완료되면 반드시 ORIGINAL이어야
한다”는 **더 강한 계약**을 요구했습니다. 선택 후 철회에서 exit 2로 실패했으며
실패 로그를 보존했습니다. 현재 문서화된 선택 시점 계약의 회귀 실패로 세지 않습니다.

첫 fixture 빌드는 host libc의 fortified builtin이 복사를 인라인화하여 barrier에
도달하지 않았습니다. 기준 실행의 관측 복사 수는 0이었고 철회 스레드는 5초
진단 timeout으로 실패했습니다. 이 계측 실패도 보존했습니다. 이후 위 private
관측 옵션으로 세 복사 지점을 확인한 실행만 결과 근거로 사용했습니다.

## 계약 해석과 남은 범위

`choose_dr()`의 마지막 generation load는 `adapter.cpp:96–99`에 명시된
선택 기준 시점입니다. 그 뒤 대체 wrapper를 구성하고 원본 함수를 호출합니다.
[기존 세션 철회 계약](SESSION_PREDICTION_LIFETIME_2026-09-30.md)도
선택 뒤 OEM teardown 동기화나 원본 호출 취소를 보장하지 않습니다.
기존 `prediction_cached_inflight` 회귀는 선택 전 session reader에서 멈추므로
이번 마지막 구간과 구별합니다.

원본 호출 직전에 atomic 검사를 한 번 더 넣어도 그 검사와 실제 호출 사이의
스케줄 변경은 남습니다. 더 강한 중단 보장을 원한다면 진행 중 송신의 수명과
OEM lifecycle이 함께 따르는 계약부터 정의해야 합니다. 이번 조사에서는
추가 guard·동기화·송신 변경을 구현하지 않았습니다.

실제 ARM DSO의 같은 구간, OEM teardown 동시성, 물리 시간·센서·차량·폰 수용은
이번 검사에서 실행하지 않았습니다. 제품 live ASSIST는 계속 비활성이며,
작성한 qualification이나 journal의 선택 기록을 물리 자격으로 승격하지 않습니다.
새 릴리즈 발행 또는 차량 사용 승인을 뜻하지 않습니다.

작성 fixture, 첫 계측 실패, 세 대조와 더 강한 계약의 부정 결과, 빌드 옵션 및
전후 해시는 비공개 `evidence/lds-field-lineage-20261001/`의
`scripts/selection-boundary.cpp`, `results/selection-boundary-r1.json`,
`results/selection-boundary-r2.json`에 보존했습니다.
