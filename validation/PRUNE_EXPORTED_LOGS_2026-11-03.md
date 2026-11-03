# 내보낸 기록을 CMU에서 지움 (2026-11-03, 소유자 요청)

## 문제

메뉴 3(터치 `SAVE LOGS`)은 지금까지 기록을 USB에 복사만 하고 CMU에 그대로 두었습니다. 상시 BETA 기록은 약 48 MiB(trace 16 MiB × 3)까지 쌓이므로, 이미 분석한 부팅이 매번 다시 내보내져 내보내기가 오래 걸렸습니다. 2026-11-03 내보내기에는 39 MiB가 들어 있었는데, 그중 주행 부팅 네 개는 모두 이미 분석한 것이었습니다.

## 바뀐 동작

- 새 `packaging/prune_logs.sh`(USB 묶음에만 들어가며 CMU에 설치하지 않음). 메뉴 3의 `finish_and_export`(`packaging/trial`)는 `finish_exit=0`이고 `export_exit=0`일 때만 이 스크립트를 부릅니다. 결과는 화면과 `trial-result.txt`의 `prune_scope=exported_journals`, `prune=...`, `prune_exit=...`에 남습니다. 둘 중 하나라도 0이 아니면 `prune=skipped`이고 아무것도 지우지 않습니다.
- 지우는 것은 회전하는 기록 파일뿐입니다: `logs/trace.N.jsonl`, `logs/collector.N.jsonl`, `logs/pimu.N.jsonl`.
- 지우지 않는 것: `installed.txt`, `guard/`, `backups/`, 메뉴 표식, `logs/g3-heading.v1`, `logs/g3-last-start`, `logs/capture.done`, 잠금·정지 표식.
- 삭제 조건(하나라도 어긋나면 `nothing deleted`로 끝남):
  1. 이번 부팅의 기록이 멈춤: `logs/capture.done`이 현재 boot ID와 같고, `logs/collector.pid`가 없고, collector 잠금이 풀려 있음. 런타임은 `capture.stop`을 받으면 남은 행을 모두 쓰고 fsync한 뒤 파일을 닫고 `capture.done`을 씁니다(`src/runtime/runtime.cpp`). 그 부팅 동안은 다시 쓰지 않습니다.
  2. USB 묶음이 `export_logs.sh`가 쓴 것: `sync` 뒤 USB의 tar를 다시 읽은 SHA-256이 옆의 `.sha256`과 같음. 묶음은 영구 저장소 밖(USB)에 있어야 함.
  3. 지울 파일마다 `tar -tvf` 목록에 같은 경로와 같은 크기로 들어 있음.
- 다음 부팅의 런타임과 collector는 새 `trace.0.jsonl`, `collector.0.jsonl`을 만들어 씁니다.
- `export_logs.sh`의 마지막 안내를 "이 내보내기는 원본을 남김, 메뉴 3이 이어서 지울 수 있음"으로 바꿨습니다.

## 남는 위험

- 지운 뒤에는 USB의 tar(와 소유자가 옮긴 사본)가 유일한 사본입니다. 크기·목록·전체 해시는 확인하지만 파일 내용을 하나씩 다시 풀어 비교하지는 않습니다. 전체 해시가 맞고 기록이 멈춘 뒤에 묶었으므로 내용이 다를 경로는 tar 자체의 결함뿐입니다.
- 지우기 도중 전원이 끊기면 일부만 지워질 수 있습니다. 남은 파일은 다음 내보내기에 다시 들어갑니다.
- 순정 BusyBox v1.19.2의 `tar -tvf` 출력은 호스트 BusyBox v1.36.1과 같은 형식(크기가 셋째 열)이라고 보고 썼습니다. 순정 BusyBox에서는 실행하지 않았습니다. 형식이 다르면 크기 검사가 실패해 아무것도 지우지 않습니다(fail closed).

## 시험

- `tests/packaging/test_trial_menu.py`: 53 OK(새 시험 7개). dash와 BusyBox v1.36.1 ash(PATH 앞에 `sh` → busybox, applet `tar`·`awk` 사용) 모두에서 실행했습니다. ash에서는 이 7개와 관련 메뉴 3 시험, 모두 11개를 돌렸습니다.
  - 정상: 다섯 기록 파일이 묶음에 들어가고 CMU에서 지워짐. G3 기록·`g3-last-start`·`capture.done`·`installed.txt`·`guard/`는 남음.
  - 확인 안 된 freeze: `prune=skipped`, 모두 남음.
  - 체크섬 불일치, 내보낸 뒤 커진 파일, 묶음에 없는 새 파일, collector PID 표식, 다른 부팅의 `capture.done`, `capture.done` 없음, 영구 저장소 안의 묶음: 모두 거부하고 아무것도 지우지 않음.
- 기존 `assert_export`는 `prune=done`이면 원본이 지워졌는지, 아니면 남았는지 확인하도록 바꿨습니다.

실행하지 않은 것: 순정 BusyBox 하네스(G7 `g7_cmu_emulation`·`g7_touch`), 실차.
