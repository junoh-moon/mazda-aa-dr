# 터치 설치 경로 (키보드 없음) — 2026-10-31

**범위.** 호스트 구현과 오프라인 검증만 수행했습니다. 차량, 폰, 주행, QEMU/CMU 에뮬레이션은 하지 않았습니다.
커밋, 푸시, 릴리스 게시도 하지 않았습니다.

## 요청과 변경 전 흐름

요청은 `Miatafy/TouchTune`을 참고해 설치 UX를 개선하고, 특히 키보드 없이 설치되게 하는 것이었습니다.

변경 전 BETA 설치는 다음 입력이 필요했습니다.

1. MP3 재생으로 OEM 진단 셸 열기(터치).
2. 키보드로 `sh /tmp/mnt/sda1/trial` 입력.
3. 숫자와 Enter를 `1`, `5`, `2` 등으로 여러 번 입력.

설치, 재부팅, 점검, 제거, 삭제 때마다 같은 과정이 반복되었습니다. 키보드 입력은 약 5~9회였습니다.

## 참고: TouchTune 1.2.1

- 저장소: https://github.com/Miatafy/TouchTune (GPL-3.0-or-later). 2026-10-31에 `gh api`로 읽었습니다.
- 진입: USB 루트의 `.up` 파일 이름을 셸 식으로 평가하는 CMU 업데이트 스캐너가 USB 안의 설치 스크립트를 실행합니다. 같은 펌웨어 계열(NA 74.00.324A)의 벤치 보고가 있습니다.
- 화면: `jci-dialog`의 `--confirm`, `--3-button-dialog`, `--info`로 버튼 선택과 진행·완료·실패 문구를 표시합니다.
- 재부팅: 설치 검증 뒤 Mazda SafeReboot(D-Bus)를 요청하고, 로그는 USB의 파일에 남깁니다.

**이 프로젝트에서 한 일.** 코드와 설정 값은 복사하지 않았습니다. 기법만 참고해 이 프로젝트의 파일로 새로 작성했습니다.
`launcher/*.ini`의 키는 같은 구조이지만 `PackageID` 등 값은 이 프로젝트 것입니다. 재부팅은 기존 `reboot_cmu.sh`(`/sbin/reboot`)를 그대로 씁니다.

## 변경 파일

| 파일 | 내용 |
| --- | --- |
| `packaging/touch/touch.sh` | 터치 흐름. 버튼 선택을 기존 `trial` 메뉴의 입력 줄(`1 0`, `2 0`, `3`, `4`, `5`, `6 6`)로 옮겨 stdin에 넘깁니다. 설치·점검·내보내기·제거 로직은 새로 만들지 않았습니다. |
| `packaging/touch/make_entry.py` | 진입 ZIP을 만듭니다. 파일 이름은 `$(...)`.up 식이며 `MX5DR-TOUCH.ID`가 하나 있는 USB의 `touch.sh`만 실행합니다. |
| `packaging/touch/MX5DR-TOUCH.ID`, `packaging/touch/launcher/*.ini` | 식별 파일과 스캐너용 설정(고정 값). |
| `packaging/make_bundle.sh` | 셸 전용이 아닌 번들에만 `touch.sh`, `MX5DR-TOUCH.ID`, 빈 `jci-autoupdate`, 진입 ZIP을 넣습니다. `INSTALL_KO.md`는 터치 안내, `SHELL_INSTALL_KO.md`는 기존 숫자 메뉴 안내가 됩니다. 셸 전용 번들은 그대로입니다. |
| `packaging/TOUCH_START_KO.md` | 터치 설치 안내(입력 없음). 화면 문구는 `touch.sh`의 실제 문자열과 대조합니다. |
| `tests/packaging/test_touch_entry.py` | 27개 테스트. |

## 화면 흐름

- 설치 전: `INSTALL` / `CANCEL` → `INSTALL` 뒤 진행 문구 → `Installed.` 대화상자에서 `RESTART` 또는 `LATER`.
- 설치 후: `CHECK` / `REMOVE` / `MORE`.
  - `CHECK`: 점검 결과 `Check: GO` 또는 `Not GO` 안내.
  - `REMOVE`: 확인 뒤 제거, 재시작 선택.
  - `MORE`: `SAVE LOGS` / `RESTART` / `DELETE ALL`. 완전 삭제는 두 번 확인합니다.

## 설계 결정

1. **기존 `trial`을 그대로 재사용합니다.** 입력 줄만 주입하므로 설치 검사, 롤백, 상태 판정, 내보내기, 삭제 거부 조건은 바뀌지 않습니다.
2. **버튼을 누르기 전에는 아무것도 바꾸지 않습니다.** 대화상자가 실패하거나 알 수 없는 상태 코드를 주면 아무 동작도 하지 않습니다.
3. **전역 실행 가드를 두지 않습니다.** `common.sh`의 EXIT trap이 `cleanup`으로 마운트를 복구하므로, 새 trap을 추가하거나 같은 이름의 함수를 정의하면 복구가 깨집니다. 동시 실행은 설치 잠금과 `trial`/`purge`의 잠금이 막습니다.
4. **셸 경로는 그대로 둡니다.** 대화상자가 뜨지 않는 경우의 대비입니다.
5. **기능 선택 메뉴는 없습니다.** 소유자의 "설치 시 기본값만 사용" 지침에 따릅니다.
6. **명령을 입력하지 않습니다.** `touch.sh`에는 `read`나 `/dev/tty` 사용이 없음을 테스트합니다.

## 검증 (호스트)

| 항목 | 명령 | 결과 |
| --- | --- | --- |
| 터치 경로와 진입 | `python3 -m unittest discover -s tests/packaging -p test_touch_entry.py` | 27개 OK |
| 설치 안내 규칙 | `-p test_install_guide.py` | 5개 OK |
| 셸 번들 | `-p test_shell_bundle.py` | 9개 OK |
| 전체 패키징 스위트 | `MX5DR_TEST_BUILD=build/adapter-arm-tests python3 -m unittest discover -s tests/packaging -p 'test_*.py'` | 465개 실행, 51개 건너뜀(기존 조건부 테스트), 실패 0 |

테스트가 확인하는 내용은 다음과 같습니다.

- 버튼마다 기대한 `trial` 줄이 가고, 취소와 대화상자 실패는 `trial`을 호출하지 않습니다.
- 설치 실패나 `Installation finished` 줄 누락이면 재시작을 요청하지 않습니다.
- `DELETE ALL`은 두 번 확인해야 하고, 중간에 취소하면 아무것도 실행하지 않습니다.
- 진입 식을 임시 디렉터리에서 실제 `sh`로 실행합니다. `MX5DR-TOUCH.ID`가 하나일 때만 `touch.sh`가 실행되고, 둘 이상이거나 없으면 실행되지 않습니다.
- 진입 ZIP에는 두 개의 ini 파일만 들어가며 내용이 원본과 같습니다.
- 전체 번들에는 터치 파일과 두 안내가 들어가고, 셸 전용 번들에는 들어가지 않습니다.
- 안내 문서는 입력할 명령이 없고, 인용한 화면 문구가 `touch.sh`에 실제로 있습니다.

## 검증하지 않은 것 (실차 전 필수)

- **업데이트 스캐너가 이 CMU에서 `.up` 이름 식을 실행하는지.** TouchTune 벤치 보고에 근거하며, 이 MX-5 CMU에서는 확인하지 않았습니다.
- **`jci-dialog` 플래그와 반환 코드.** TouchTune 소스의 사용법을 따랐습니다. 이 CMU에서의 실제 동작은 모릅니다. 특히 3버튼 대화상자의 두 번째와 세 번째 코드(1, 2)가 가정과 같은지 확인해야 합니다.
- **대화상자의 `\n` 줄바꿈 처리.**
- **USB 자동 실행 중 재마운트 가능 여부.** `mount -o rw,remount`가 실패하면 `touch-log.txt`는 남지 않고 `trial`의 보고서만 남습니다.
- **재시작 경로.** `reboot_cmu.sh`(`/sbin/reboot`)를 그대로 썼습니다. TouchTune의 SafeReboot 요청과 같은 효과인지는 모릅니다.
- **`jci-autoupdate` 빈 파일의 스캐너 의미.** 참고 구성과 같게 두었습니다.
- **주행 중 안전.** 대화상자는 USB를 꽂을 때 뜹니다. 주차 규칙은 안내 문서에만 의존합니다. 차량 신호로 막지는 않습니다.
- **실제 화면 표시.** 대화상자 글자 수와 줄바꿈이 CMU 화면에 맞는지 모릅니다.

## 남은 위험과 결정 사항

- 이 경로는 OEM 업데이트 스캐너가 파일 이름을 셸 식으로 평가하는 동작을 이용합니다. 소유자의 차량에서만 쓰며, 공개 배포는 별도 요청과 승인 뒤에만 합니다.
- 셸 경로와 터치 경로가 같은 `trial`을 쓰므로, 그 쪽의 수정이 두 경로에 함께 영향을 줍니다.
- `docs/OWNER/*`, `docs/STATUS_KO.md`, 공개 v1.0.0 문서는 바꾸지 않았습니다. 터치 경로는 아직 공개 판에 들어가지 않은 변경입니다.
- 커밋은 하지 않았습니다. 변경은 작업 트리에 있습니다.
