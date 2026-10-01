# v0.3.9 첫 실차 회수: 기록 0바이트 — 2026-10-02

## 확인한 사실

사용자는 v0.3.9-shadow.1의 메뉴 `1`로 설치한 뒤 차량 시동을 껐다 다시
켰습니다. CMU Linux 자체의 재부팅 여부는 확인하지 않았습니다. USB에 연결한
무선 Android Auto 동글로 짧은 시험을 한 뒤, 기존 메뉴 `3` 오류를 보았고
메뉴 `4`로 제거했습니다.
이후 v0.3.9-shadow.2의 파일로 USB를 교체하여 메뉴 `3`을 다시 실행했습니다.
현재 셸은 닫혀 있으며 추가 차량 확인은 요청하지 않았습니다.

제공된 두 번째 USB ZIP은 경로 중복·탈출·심볼릭 링크·CRC 오류가 없습니다.
패키지 36개 파일은 공개 v0.3.9-shadow.2 ZIP과 모두 바이트 동일합니다.
이 대조는 **회수에 사용한 USB 파일**에 대한 것이며, 앞선 주행 때 차량에
설치된 제품은 v0.3.9-shadow.1입니다. 두 릴리즈의 다섯 ARM 바이너리는
동일하지만 메뉴 `3`의 shell 파일은 다릅니다. 제출 사진·원문·개인 공유
주소와 부팅 ID는 비공개 `evidence/`에만 보존합니다.

`trial-result.txt`에는 `one_boot=unconfirmed`, `retained_bytes=0`,
`status_exit=1`, `finish_exit=1`, `export_exit=0`이 기록됐습니다.
회수 tar와 `.sha256`은 일치하고 tar 구조도 정상입니다. 그러나 tar에는
`logs/collector.lock`(0바이트), 메뉴 `3`이 만든 stop 디렉터리 두 개와
현재 `mx5dr.conf`의 `mode=OFF`만 있습니다. `trace.*.jsonl`,
`collector.*.jsonl`, 저장 중단 기록은 **하나도 없습니다**. `mode=OFF`는
메뉴 `4` 제거 후 현재 설정이지 앞선 주행의 실행 모드가 아닙니다.
`tools/analyze_logs.py --json`은 종료 2, `status=inconclusive`, 입력 파일과
기록 건수 모두 0을 반환했습니다. `no_location_samples`는 이 빈 입력의
결과이지 위치 입력만 빠졌다는 진단이 아닙니다. `export_exit=0`은 남아 있던
파일의 USB 복사 성공이지 센서 수집이나 SHADOW 계산 성공이 아닙니다.

## 원인 범위와 남은 구분

설치기는 guard를 **다음 CMU autostart**에 예약하며 현재 SM이나 collector를
시작하지 않습니다. 정상 모드의 원본 autostart에서 SM 직전 guard가 임시
설정을 선택한 경우에만 collector의 자동 시작을 요청합니다. 실제 tar에 collector의
첫 `collector_boot`조차 없으므로 센서 품질·무선 동글·MODEL 계산 단계보다
앞선 기동/선택/시작 경계를 먼저 확인해야 합니다. 시동 OFF/ON이 CMU Linux
재부팅과 같았는지는 이 자료로 판정할 수 없습니다. guard 선택이 실패했거나
collector와 runtime이 각각 시작하지 못했을 가능성도 남습니다. 무선 연결을
원인으로 단정하지 않습니다.

메뉴 `4`는 `guard/arm`을 지우지만 `guard/consumed`와 `guard/last-boot`는
지우지 않습니다. 현재 릴리즈의 export는 `guard/`를 담지 않고, 현재 부팅의
`one_boot=unconfirmed`는 과거 부팅의 선택 여부를 말하지 않습니다. 따라서
다음 **기존의 주차 중 셸 기회**에 그 두 표식의 존재를 읽으면 guard 소비
여부를 구분할 수 있습니다. 셸이 닫힌 상태에서 새 주행이나 재설치를 요구할
근거는 없습니다. 표식이 있다 해도 collector와 AA runtime의 기동 성공을
추가로 증명해야 합니다.

## 오프라인 재검사와 절차 수정

공개 v0.3.9-shadow.2 ZIP을 다시 내려받아 SHA-256·CRC·manifest·고정 소스와
대조했습니다. ZIP을 순정 ARM BusyBox/libc chroot에서 설치·guard 선택·
collector·회수·제거까지 실행한 검사는 종료 0이었습니다. 공개 소스의
독립 `make test`도 host Python 426개 및 C/C++ 검사에서 종료 0,
생략 0이었습니다. 이들은 작성된 부팅/센서 조건과 선택 helper 경로를
검사하며, 실제 차량의 시동 주기·정상 전체 OEM autostart 실행을 검증하지
않습니다. 이 한계를 넘어 실차 성공으로 올리지 않습니다.

후속 소스는 빈 로그에서도 `guard/last-boot`의 현재/다른 부팅 여부와
`guard/consumed`의 존재를 읽기 전용 상태 출력에 추가합니다. 공개
v0.3.9-shadow.2에는 이 출력이 없습니다. 관련 Linux 상태 검사 43개와
수정한 묶음의 순정 ARM BusyBox/libc 설치·가드 선택·수집기·상태·회수·제거
검사는 통과했습니다. 순정 OEM autostart 전체나 실차 시동 주기를 실행한
검사는 아닙니다. 다음에 별도로 허용된 차량 시험이
있다면 **주차 상태에서 CMU 부팅 뒤 메뉴 `2`를 실행**해
`one_boot=consumed_this_boot`, 현재 collector 기록 및 최근 poll을 확인한
후에만 이동 단계로 넘어가도록 안내를 고칩니다. 정차 중 센서 미수신으로
메뉴 `2`의 종료 코드가 1일 수 있으므로, 코드 0 대신 개별 기동 근거를
읽습니다. 기동 근거가 없으면 메뉴 `3`으로 가능한 자료를 회수하고, 메뉴가
종료되면 같은 한 줄로 다시 들어가 `4`로 예약을 해제합니다. 이 절차 수정은
과거 빈 기록을 복구하거나 새 실차
기회를 승인하지 않습니다. ASSIST는 계속 비활성입니다.

후속 소스의 전체 Linux `make test`는 순정 fixture와 시험 묶음을 지정해
Python 427개 및 C/C++ 검사를 종료 0·생략 0으로 통과했습니다. 이는
설치기 마지막 안내 문구를 고치기 전 실행입니다. 그 문구를 포함한 최종
소스에서는 설치 관련 Python 178개를 다시 종료 0·생략 0으로 통과했고,
순정 ARM BusyBox/libc 시험 묶음의 설치 메시지·상태·회수·제거도 다시
종료 0으로 확인했습니다. 두 shell 파일의 `sh -n`과 `git diff --check`도
종료 0입니다. 선택적으로 실행한 ShellCheck는 기존의 동적 `common.sh`
source/공유 변수와 `CDPATH=` 구문에 관한 경고 3개로 종료 1이었으며
이를 통과로 세지 않습니다.

앞선 회수 대안 검토에서는 Claude Code가 공개 v0.3.9-shadow.2의 존재를
독립적으로 찾아, 별도 미검증 회수 스크립트 대신 해당 고정 ZIP으로
회수하도록 방향을 바로잡았습니다. 이번 빈 기록 분석과 새 상태 출력의
비판 검토도 CLI에 두 번 요청했으나 응답 파일이 생성되지 않아 각 실행을
종료했습니다. 이번 변경에 대한 Claude의 독립 검토 결과를 받은 것으로
기록하지 않습니다.
