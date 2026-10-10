# 기록 보존: 짧은 부팅이 주행 기록을 밀어내던 문제 (2026-10-10)

범위: 런타임 trace 회전 규칙, 상시 실행(persistent) 설치 설정, 메뉴 `2` 상태 문구, 분석기 판독.
제품 결정(BETA/SHADOW 계산, OEM 전달 계약, 저장 공간 부족 시 fail-closed)은 바꾸지 않았습니다.
차량에서 실행한 적이 없습니다.

## 증거: 무엇이 왜 사라졌나

- 소유자가 v1.0.0-beta.7로 주행한 뒤 CMU를 여러 번 재시작하고 나서 회수했습니다. 회수 tar
  (비공개, `mx5dr-logs-19700101T000141-10252.tar`)에는 `trace.1.jsonl`(64,736 B, 329행,
  부팅 후 38.8~88.8 s)과 `trace.0.jsonl`(175,968 B, 990행, 42.9~100.3 s)만 있었고, 둘 다
  바퀴 속도 0인 짧은 부팅입니다. 주행 기록은 없었습니다.
- 설치된 `mx5dr.conf`: `max_log_bytes=8388608`, `max_log_files=2`, `log_profile=persistent`
  (`packaging/common.sh` `set_config`의 persistent 정책 = trace 8 MiB×2).
- 원인: `src/runtime/runtime.cpp`의 `Journal::rotate()`는 파일이 찼을 때뿐 아니라 **매 부팅의 첫
  쓰기**(`f`가 아직 없음)에서도 호출되어 `trace.N-2 → trace.N-1`로 밀었습니다. 90초짜리 부팅도
  예외가 아니었으므로 파일 2개 설정에서는 주행 뒤 **두 번째** 부팅이 주행 파일을 지웠습니다.
- 용량도 작았습니다: 8 MiB는 조용한 profile의 약 2 KB/s에서 약 68분, 터널 모드(약 4.3 KB/s,
  `validation/TUNNEL_UNBOUNDED_2026-10-09.md`)에서 약 33분입니다.

## 변경

1. **부팅마다 이어 쓰기** (`runtime.cpp` `Journal::reopen_small`, `write_line`):
   한 `Journal` 객체의 첫 쓰기에서만, 기존 `trace.0.jsonl`이
   - `O_RDWR|O_APPEND|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC`로 열리고(없으면 만들지 않음, 심볼릭 링크는
     따라가지 않음, FIFO가 있어도 열기에서 멈추지 않고 fstat에서 거부; 판정 뒤 blocking으로 되돌림),
   - 정규 파일, 링크 수 1, 소유자가 현재 euid, group/other 권한 없음(회전 경로가 만드는 0600 파일과 같은 조건),
   - 크기가 `max_log_bytes` 미만(아직 차지 않음)이면
   회전하지 않고 그 파일 뒤에 이어 씁니다. 따라서 링은 부팅 수와 관계없이 **가장 새 행들의 FIFO**
   (`max_log_files`×`max_log_bytes`)입니다. `written`을 기존 크기로 시작하므로 상한에 닿으면 평소처럼
   회전합니다. 마지막 바이트가 `\n`이 아니면(이전 부팅이 행 중간에서 끊김) 먼저 `\n`을 써서 행이
   붙지 않게 하고 그 1바이트도 상한에 셉니다.
   - 크기 조회·마지막 바이트 읽기·blocking 복귀 실패, 찬 파일, 위 조건 불일치 → 기존 회전(동작 그대로).
   - 구분 `\n` 쓰기 실패, `fdopen` 실패 → `fail()`(기존 저장 실패와 같은 fail-closed).
   - 같은 부팅 안의 회전(파일이 참)과 `close_durable()`(capture stop) 뒤의 쓰기는 항상 기존 회전입니다.
   - 저장 공간 검사(`storage_space`)는 이 판단보다 먼저 그대로 수행됩니다. capture.stop이 있으면
     런타임은 기존대로 아무것도 쓰지 않습니다.
   - 초안의 고정 512 KiB 임계값은 독립 검토(HIGH)에서 기각했습니다: 20분 주행 뒤 약 100초 재시작
     8번이면 주행이 0 % 남았고, 소유자의 셸 부팅 + 메뉴 `5` 재부팅 + 시동 반복 패턴에서도 사라졌습니다.
2. **persistent 설치 설정**: `max_log_bytes=16777216`, `max_log_files=3`(trace 48 MiB; 이전 16 MiB).
   collector는 persistent에서 1 MiB×2로 스스로 제한하며 **지금도 부팅마다 회전합니다**.
3. **메뉴 `2`**(`trial_status.sh`): `trace_cap_bytes`/`collector_cap_bytes`를 설치된 설정에서
   계산합니다(persistent 설치: `50331648`/`2097152 (48+2 MiB)`; 설정 확인 불가면 `unconfirmed`).
   persistent 설정이면 보존 안내도 새 규칙과 숫자로 바뀝니다. 일회성 시험 문구는 그대로입니다.
4. **분석기**(`tools/analyze_logs.py`): 이어 쓴 파일 안에서 끊긴 행 뒤에 boot 행이 오면, 파일
   끝의 끊긴 행과 똑같이 `partial_final_line`도 보고합니다(한 줄 미리 읽기). JSON 오류 상세는
   행 끝 `\n` 없이 판독해 두 배치에서 같은 문구가 됩니다. `tools/replay_beta.cpp`의 기본 부팅은
   `trace.0`의 **마지막** boot 행(가장 새 부팅)입니다.
5. 문서: `docs/FIELD_PROCEDURE_BETA_KO.md`, `packaging/USB_START_KO.md`, `packaging/SHELL_START_KO.md`.

## 보존 계산 (추정, 차량 측정 아님)

| | 이전 persistent (8 MiB×2, 매 부팅 회전) | 새 persistent (16 MiB×3, 찬 파일만 회전) |
|---|---|---|
| 링 전체 | 16 MiB | 48 MiB |
| 주행 뒤 짧은 부팅(65~176 KB, 이번 증거) | 2번째 부팅에서 주행 손실 | 횟수와 무관하게 보존 |
| 주행이 사라지는 조건 | 부팅 2번 | 그 뒤 새 기록이 링을 넘칠 때 |
| 그 새 기록량, 보통 주행 약 2 KB/s | – | 약 4.6~6.9시간 |
| 그 새 기록량, 터널 모드 약 4.3 KB/s | – | 약 2.2~3.2시간 |

회전은 가장 오래된 16 MiB 파일을 통째로 버리므로 남는 양은 32~48 MiB 사이입니다(48 MiB =
50,331,648 B ÷ 2,000 B/s ≈ 6.9 h, ÷ 4,300 B/s ≈ 3.2 h; 32 MiB는 약 4.6 h / 2.2 h).
collector 파일(persistent 1 MiB×2)은 별도로 부팅마다 회전합니다.

저장 공간: 설치 전 검사(`require_trial_space`)는 설정과 무관하게 trace 40 MiB×3 + collector
4 MiB×2의 남은 할당과 8 MiB + 64 KiB + 1 MiB 여유를 요구하므로 48 MiB 링은 그 안에 있습니다.
런타임 검사는 행마다 남은 공간 ≥ 8 MiB + 64 KiB + 다음 행이며 상한과 무관합니다. 차량에서 본
약 513 MB 여유에서 48 MiB는 충분합니다.

## 시험 (호스트, 이 변경의 사본에서)

- `tests/runtime/test_journal.cpp` `retention_tests`(기본 실행과 `--retention`): 첫 부팅 생성(0600);
  짧은 부팅 이어 쓰기(이동 없음, `written` = 파일 크기); 2 MiB 주행 파일 뒤 짧은 부팅 8번(각 약 100 KB)이
  `trace.1`을 만들지 않고 주행 행이 맨 앞에 남음; 상한−100 B 파일은 첫 쓰기에서 정상 회전, 상한 크기
  파일도 회전, 끊긴 행으로 끝난 찬 파일은 구분자 없이 그대로 `trace.1`; 짧은 부팅 100번이 `trace.0`에
  남다가 찬 뒤 한 번 회전하고 `trace.1`이 그 100부팅으로 시작; 끊긴 행 뒤 `\n` 구분; 빈 파일; 이어 쓴
  파일이 상한에 닿으면 정상 회전하고 같은 부팅 안의 다음 회전도 정상; 심볼릭 링크를 따라가지 않음;
  0644 파일·하드 링크 파일·FIFO는 회전; writer thread 경로; `close_durable()` 뒤 쓰기는 회전.
  기존 시험 중 한 디렉터리에서 `Journal`을 여러 번 열고 자기 행만 읽던 6곳은 각 경우 시작 전에
  `trace.0`을 지우도록 고쳤습니다(검사 내용은 그대로).
- 변이: 고정 512 KiB 임계값으로 되돌림, 찬 파일 검사 제거, `\n` 구분 제거, `written` 초기화 제거,
  "첫 열기에서만" 조건 제거 → 각각 실패.
- `tests/tools/test_analyze_logs.py`: 두 부팅을 한 파일에 이어 쓴 결과 = 회전된 두 파일의 결과,
  끊긴 행 + 이어 쓴 부팅, boot가 뒤따르지 않는 깨진 행은 `partial_final_line` 아님.
- `tests/packaging`: persistent 설치 설정 문자열, 메뉴 `2`의 상한 문구(persistent, 일회성, 기본값, 무효).

분석기 실측 비교(입력 이름·행 번호만 제외한 `--json` 전체와 문자 출력이 동일):
이번 증거의 두 짧은 부팅, 이전 BETA 주행 회수 기록의 두 부팅(비공개, 22.5 MB + 6.1 MB, BETA 미작동 구간), 그리고
`build/log_rate` persistent 합성 두 부팅(BETA ENGAGED 9회, 정차 94개 표 포함), 각각 끊긴 행 변형까지.
변경 전 분석기는 끊긴 행 변형에서 이슈 1건(`partial_final_line`)이 달랐습니다.

## 소유자가 계속 할 일

- 주행 직후, 가능하면 시동을 끄기 전에 메뉴 `3`으로 회수하십시오.
- 새 설정(16 MiB×3)은 이 변경을 포함한 빌드로 메뉴 `1` 설치를 해야 적용됩니다. 이어 쓰기 규칙은
  런타임에 있으므로 이전 설정(8 MiB×2)에서도 동작합니다(그 경우 링은 16 MiB).
- 첫 주행 확인 목록:
  1. 메뉴 `1` 재설치 뒤 메뉴 `2`에 `trace_cap_bytes=50331648`과 `guard_config_binding=matched`.
  2. 주행 뒤 재시동 1~2번 후에도 메뉴 `2`의 `retained_bytes`가 줄지 않음.
  3. 메뉴 `3` 회수본: `trace.0`에 주행의 boot_id를 포함한 여러 boot 행이 있고, `trace.0`이 16 MiB가
     되기 전에는 `trace.1`이 없음.
  4. PC 분석기가 재시동을 반복한 뒤에도 주행 부팅을 보고함.

## 한계

- 48 MiB(실제로는 32~48 MiB)를 넘는 새 기록이 쌓이면 주행도 회전으로 사라집니다. 한 번의 긴
  부팅도 같은 이유로 자기 앞부분을 잃습니다.
- collector 파일(`collector.0/1`, persistent 1 MiB×2)은 이 규칙을 쓰지 않고 부팅마다 회전합니다.
  주행의 collector 기록은 짧은 재시작 두 번 뒤 사라질 수 있습니다.
- 한 파일에 여러 부팅이 들어갑니다. 분석기와 `replay_beta`(boot 행으로 구분)는 처리하지만,
  `trace.0`의 첫 행이 이번 부팅의 boot 행이라고 가정하는 외부 도구는 맞지 않습니다.
- `trace.0`이 찰 때까지 같은 파일이 여러 부팅 동안 열립니다. 이전 부팅의 끊긴 행은 구분자로만
  막으며 복구하지 않습니다.

## 미검증

차량 실행(실제 eMMC에서의 재시작 반복, 찬 파일 판정과 이어 쓰기, 실제 주행 크기), CMU 부팅 중 첫 쓰기
지연, 실제 tunnel 모드 기록 속도. QEMU·호스트 성공은 차량 검증이 아닙니다.
