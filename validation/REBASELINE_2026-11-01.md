# rebaseline.sh: 다른 도구가 sm.conf를 바꾼 뒤 재설치 없이 다시 등록 (2026-11-01)

상태: **미발행 후보**(브랜치 `release/v1.0.4`, v1.0.3 태그 `4df9d5b` 기준). 제품 바이너리와 guard 소스는 바꾸지 않았습니다. 실차, 정확한 ARM 빌드, QEMU, 순정 BusyBox 하네스, 릴리즈 게이트는 실행하지 않았습니다(아래 "실행하지 않은 것").

## 문제

mazda-cmu-tune 1단계는 `/jci/sm/sm.conf`와 `sm_WCP.conf`를 바꿉니다. 상시 BETA의 guard는 설치할 때 두 파일의 원본 해시와 trial 템플릿을 묶어 두므로, 바뀐 다음 부팅부터 `baseline_edited`로 순정 부팅합니다(fail closed). 지금까지 mazda-cmu-tune은 USB에 묶은 mazda-aa-dr의 메뉴 1(재설치)로 다시 등록했습니다. 이 방식에는 세 가지 문제가 있었습니다.

- mazda-cmu-tune 릴리즈가 mazda-aa-dr의 특정 릴리즈 형식에 묶입니다. 실제로 v1.0.3의 `SHA256SUMS` 변경으로 mazda-cmu-tune의 묶음 빌드가 실패했습니다(그쪽 세션에서 재현, 그쪽 커밋 9ef5d65에서 해결).
- 차의 mazda-aa-dr가 USB에 묶인 판으로 조용히 교체됩니다(다운그레이드 가능).
- 메뉴 1은 소유자 스위치를 기본값으로 되돌립니다.

두 레포 세션과 소유자가 2026-11-01에 계약을 합의했습니다. 내용은 `docs/INTEROP_KO.md`(contract 1)에 있습니다.

## 바뀐 것

- `packaging/rebaseline.sh`(신규). 메뉴 1이 `tools/`에 설치합니다. 동작은 다음과 같습니다.
  - 상시 BETA 설치본만 다룹니다.
  - 사전 검사: guard 상태, trip, 런타임 중지 요청, 잠금, 펌웨어 해시, OEM 파일 안의 토큰, `installed.txt`의 preload 해시, persistent 설정 텍스트.
  - 사전 검사를 통과하면 템플릿과 원본 해시를 다시 만들고, 동시 수정 여부를 대조합니다. 이전 `persist-state`·`last-decision`·원본 해시는 `backups/persist-evidence/<n>/`에 남깁니다. 그다음 교체 → guard `check` → `enable` → `verify` 순서로 진행합니다.
  - 마지막 stdout 줄은 `result=` 한 줄이고 종료 코드는 계약 표와 같습니다. `--check`는 읽기 전용이며 trip을 불일치보다 먼저 보고합니다.
- `packaging/bundle-version`(신규, `1.0.4`). 묶음과 `build-info.json`의 `"version"`에 들어가며, 설치기는 `installed.txt`에 `version=` 줄을 씁니다. 형식이 틀리면 아무것도 바꾸지 않고 설치를 거부합니다. 개발용 묶음처럼 이 파일이 없으면 줄을 쓰지 않습니다.
- `tests/gates/g5g6_zipcheck.py`: `rebaseline.sh`가 있는지, `bundle-version` 형식이 맞는지, build-info와 일치하는지, `--version`(= 태그)과 일치하는지 검사합니다. `tools/release/zip.sh`가 `--version "${TAG#v}"`를 넘깁니다.
- `make_bundle.sh`, `make_usb_zip.py`, `install.sh`(tools 목록, 버전 줄), `test_shell_bundle.py`(기대 파일 목록).

## 설계에서 정한 것

- **trip은 풀지 않습니다.** 다른 도구의 변경과 aa-dr이 같은 부팅에 처음 함께 들어가면 guard는 원인을 구분할 수 없습니다. 다시 켜는 것은 소유자가 직접 고르는 메뉴 1뿐입니다.
- **probation은 생략하지 않습니다.** 바뀐 sm.conf에 preload를 더한 trial은 guard가 처음 보는 조합입니다.
- **런타임 중지 요청을 따로 확인합니다.** 개발 중 발견한 문제입니다. 기준 파일이 바뀐 상태에서 guard의 `verify`는 `runtime_disabled_pending`보다 `baseline_edited`를 먼저 보고합니다. 그래서 처음 구현은 중지 요청을 놓치고 enable까지 진행했고, 그 뒤 `verify`에서야 `partial:verify`로 끝났습니다. 지금은 `logs/disable-next-start`를 직접 보고, 등록은 `precheck_failed:runtime_disabled`, `--check`는 `unknown:runtime_disabled`로 거부합니다.
- **메뉴 1이 설치한 바이트만 다시 묶습니다.** enable은 preload와 설정의 현재 해시를 새로 묶으므로, 이를 `installed.txt`와 persistent 설정 텍스트로 먼저 대조합니다.
- **guard가 `baseline_has_token`을 먼저 보고하는 경우**도 `precheck_failed:baseline_has_token`으로 맞췄습니다.

## 실행한 검증

호스트: Linux x86-64, Python 3, dash(`/bin/sh`), BusyBox v1.36.1(Ubuntu), host 빌드 guard(`-DMX5DR_GUARD_TESTING`, `src/guard/guard.cpp` 변경 없음).

- `tests/packaging/test_rebaseline.py`: 12/12 통과(dash). 다루는 경우는 다음과 같습니다.
  - `--version`, 사용법 오류
  - 버전 줄 기록, 잘못된 버전 거부, 개발용 묶음
  - 일치할 때 아무것도 바꾸지 않음
  - 다른 도구가 고침 → `--check` 4 → 다음 부팅 순정 → 등록 0 → `--check` 0 → 다음 부팅 trial에 새 내용과 preload 3개, probation=yes, 증거 보관
  - 스위치 표식과 휴대폰 채널 형태 보존
  - trip 우선과 비변경
  - 사전 검사 7종(locked, pending, payload, config, baseline_has_token, runtime_disabled, unsupported_policy)의 비변경
  - 미설치, enable 안 됨
  - enable 실패 → `partial:enable`, `--check` binding_changed, 순정 부팅, 재시도 성공
  - 템플릿을 만드는 중의 동시 수정 → `precheck_failed:concurrent_edit`, 교체 없음
- 같은 시험을 BusyBox ash(`sh` → `busybox sh`)로 실행: `test_rebaseline` + `test_persistent_beta` 23개 중 22개 통과. 실패 1개는 동시 수정 시험으로, 원인은 시험 장치였습니다. 이 BusyBox 빌드는 PATH에 넣은 awk 대신 내장 applet을 실행하므로(`PATH=shim:… busybox sh -c awk` → applet), 시험이 끼워 넣는 동시 수정이 실행되지 않습니다. 기존 install 동시 수정 시험도 같은 이유로 이 방식에서는 재현되지 않습니다. dash에서는 통과합니다.
- `python3 -m unittest discover -s tests/packaging`: 469개 실행, 실패 0, SKIP 49. SKIP 이유는 다음과 같으며, 모두 이 기계에 비공개 입력이나 도구체인이 없어서입니다.
  - 순정 identity fixture 없음: 39
  - 고정 ARM 컴파일러 없음(bds 파싱): 6
  - `MX5DR_RELEASE_BUNDLE` 없음: 2
  - host collector 빌드 없음: 1
  - 여섯 산출물 릴리즈 묶음 없음: 1
  - `make test-packaging`은 이 기계에 D-Bus 개발 헤더가 없어 collector 빌드 단계에서 멈춥니다. 그래서 unittest를 직접 실행했습니다.
- 비공개 입력으로 한 일회성 시험(커밋하지 않음, OEM 파생 파일)은 아래와 같습니다.
  - 입력: 차량 내보내기 `cmu-20261025`의 `sm.conf`·`sm_WCP.conf`(md5 `70ccd40a…`/`9ed4aec3…`)와, 그 파일로 만든 mazda-cmu-tune 1단계 결과(md5 `8643e2b2…`/`db5f1d19…`).
  - 결과: 설치와 enable 뒤 1단계 파일로 교체 → `--check` `mismatch:baseline_edited:sm.conf` → 다음 부팅 순정 → 등록 `result=done` → `--check` `match` → 다음 부팅 trial.
  - trial과 1단계 파일의 차이는 4줄뿐입니다. VBS·LDS preload 추가, 그리고 AA preload를 기존 `libpatch-blmjciaapa.so` 앞에 붙인 것입니다.
- 1단계 파일에서 `edit_service.awk`를 직접 실행한 것(2026-11-01 같은 날, scratch): preload 3개만 추가되고, AA 토큰을 넣었다 빼면 원본과 바이트 단위로 같습니다.

## 실행하지 않은 것 (통과로 세지 않음)

- 차량. 이 기능은 실차에서 한 번도 실행되지 않았습니다.
- 정확한 ARM 빌드와 QEMU에서 실제 ARM guard로 `check`/`enable`/`verify`. guard 소스가 바뀌지 않았고 rebaseline이 부르는 명령은 메뉴 1과 같지만, 이것은 대체 증거일 뿐입니다.
- 순정 BusyBox 하네스(G7 등)와 릴리즈 게이트 G0–G18. 발행이 요청되면 `docs/RELEASING_KO.md` 절차로 실행합니다.
- 실차와 비슷한 /data_persist 읽기 전용 remount 경로. 시험 장치는 remount를 하지 않습니다.

## 남은 한계

- rebaseline 직후 첫 부팅은 probation입니다. 다른 도구의 변경과 aa-dr이 함께 처음 들어가므로, 실패하면 trip이 되고 원인은 구분되지 않습니다. 원인을 가리려면 두 변경 사이에 확인된 부팅을 한 번 두십시오.
- `--check`의 `match`는 guard 입력이 일치한다는 뜻일 뿐, 다음 부팅이 성공한다는 뜻이 아닙니다.
- 상시 BETA가 아닌 설치본(one-boot SHADOW 등)은 다루지 않습니다(`unsupported_policy`). 메뉴 1을 쓰십시오.
