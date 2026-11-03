# 미확인 부팅으로는 차단하지 않음 (2026-11-03, 소유자 결정)

## 문제

상시 BETA guard는 제품을 선택한 뒤 autostart 블록이 90초 뒤 `mx5dr-guard confirm`을 부를 때 그 부팅을 확인(C)합니다(`packaging/edit_autostart.awk`). 그 전에 부팅이 끝나면 미확인(U)입니다. v1.0.4까지는 다음 두 경우 제품을 끄고 메뉴 1 전까지 순정으로 부팅했습니다.

- 미확인 3번 연속(`tripped=unconfirmed`).
- 설치(또는 `rebaseline.sh` 재등록) 뒤 첫 부팅(probation)이 미확인(`tripped=probation`).

2026-11-03 차량 USB 내보내기(cmu-tune 0.7.2 + v1.0.4, 비공개)에서 부팅 하나(`fc7b0ee6…`)가 cmu-tune의 재부팅 요청으로 uptime 78초에 끝나 `recent=CUC`가 되었습니다. 다음 부팅이 확인돼 카운터는 0으로 돌아왔지만, 소유자는 주차·짧은 이동으로 시동을 짧게 켰다 끄는 일이 연달아 생기면 정상 사용만으로 제품이 꺼진다고 지적했습니다. probation은 한 번만으로 꺼집니다.

## 결정

소유자 결정(2026-11-03 대화): 미확인 부팅은 차단 사유에서 뺍니다. cmu-tune이 바꾼 설정이 SM을 망가뜨리는 위험은 cmu-tune이 맡습니다.

## 바뀐 동작 (`src/guard/guard.cpp`)

- `evaluate()`: 미확인(U)은 `recent`와 `unconfirmed_count`(최대 3에서 포화)에 기록만 하고 `tripped`를 바꾸지 않습니다.
- probation은 **확인되지 않은 시도가 실패(R: SM 리셋 보고 변화, B: healthy 규칙, 이 빌드에서는 꺼짐)** 일 때만 `tripped=probation`이 됩니다. 미확인이면 `probation=yes`가 그대로이고 다음 제품 부팅이 다시 probation입니다. 확인된 시도 뒤 리셋은 예전처럼 `fail_count`로 셉니다.
- 리셋 보고 규칙은 그대로입니다: 마지막 확인 뒤 실패 2번(`reset_reports`), 최근 10번 중 3번(`reset_reports_repeated`), 런타임 중지 요청(`runtime_disabled`).
- `parse_state()`: 활성 상태에서 `unconfirmed_count=3`을 허용합니다(이전에는 3이면 상태 무효). 4 이상은 여전히 무효입니다.
- 이전 빌드가 남긴 `tripped=unconfirmed`·`tripped=probation` 상태는 메뉴 1까지 순정 그대로입니다.
- 상태 화면: `fail count N of 2, unconfirmed N (info)`(39칸), `persist_unconfirmed_trip_at=off`.
- 상태 파일 형식(`mx5dr-persist-state-v2`)과 `persist` 매니페스트 형식은 바뀌지 않았습니다.

## 잃는 보호

리셋 보고를 남기지 않고 확인 전에 끝나는 고장은 더 이상 자동으로 막지 않습니다. 예: SM이 trial 설정을 거부해 실행되지 않는 경우, 보고 없는 커널 패닉·워치독 리셋, 화면이 멈춰 소유자가 전원을 끊는 경우. 이런 고장이 생기면 매 부팅 같은 시도가 반복될 수 있습니다. 지금까지 실차 기록에서 이런 경우는 관측되지 않았습니다. 이것은 소유자의 위험 수용이며 검증 통과가 아닙니다.

## 시험 (호스트 빌드 guard, `-DMX5DR_GUARD_TESTING`)

- `tests/recovery/test_guard_persistent.py`: 44 OK. 새 시험·바뀐 시험:
  - `test_probation_unconfirmed_attempt_keeps_probation`: 미확인 probation 두 번 뒤에도 차단 없음, 다음 확인 부팅에서 probation 종료.
  - `test_probation_reset_trips_immediately`: 확인되지 않은 probation 시도 뒤 리셋 보고 → `tripped=probation`.
  - `test_unconfirmed_attempts_never_trip`: 미확인 12번, 차단 없음, 카운터 3 포화, 확인 뒤 0.
  - `test_earlier_unconfirmed_trip_stays_until_menu_1`: 예전 `tripped=unconfirmed` 상태는 순정, 메뉴 1 뒤 다시 선택.
  - 기존 `test_only_a_confirmed_attempt_resets_the_unconfirmed_counter`, 전달 실패 시험은 기대를 "차단 없음"으로 바꿈.
- `tests/recovery/test_guard.py`, `tests/recovery/test_guard_bt.py`: OK.
- `tests/packaging/test_persistent_beta.py`의 상태 화면 기대값과 "count above cap"(이제 4)을 맞춤.

- `tests/gates/g7_multiboot.py`의 시나리오 5·6을 새 규칙으로 고쳤습니다(미확인 probation은 차단 없음, 미확인 4번 뒤에도 제품·카운터 3). 이 하네스는 비공개 순정 트리가 있어야 돌아서 이번에는 실행하지 않았습니다.

실행하지 않은 것: 고정 도구체인으로 만든 ARM guard를 QEMU에서 돌리는 시험, 순정 BusyBox 하네스(G7–G9, G16), 실차.
