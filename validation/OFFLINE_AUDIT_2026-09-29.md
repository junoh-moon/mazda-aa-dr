# USB 수정 이후의 독립 감사와 오프라인 검증

이 기록은 `e1f7ea2624317ba630c4a0077e561ab8a998d587` 이후의 작업입니다.
과거 시험 횟수를 수정하지 않습니다. 설치 성공이나 SHADOW 계산 성공을
Galaxy S25·무선 AA·네이버 지도의 터널 안내 성공으로 판정하지 않습니다.
최종 완료 조건은 [v1.0 작업 목록](../docs/V1_READINESS_KO.md)에 있습니다.

## 재현한 결함과 변경

- ARM 객체에 전이 헤더 의존성이 없어 헤더를 고쳐도 오래된 ELF가 남았습니다.
  컴파일러 `.d`와 누락 시 재컴파일을 추가했습니다. 회귀 6개는 이전 Makefile에서
  모두 실패하고 수정 후 통과했습니다. guard/hash도 같은 객체 규칙을 사용합니다.
- ZIP 빌더가 비 ELF 텍스트 파일도 받아 현재 소스·고정 컴파일러 빌드로 표시했습니다.
  새 디렉터리에서 실제 컴파일한 입력·도구체인·결과물 기록을 생성하고 ZIP에서
  대조합니다. ARM ELF, EABI, GLIBC 버전과 의존성도 확인합니다. 이 기록은 서명이
  아니며, 오래된 파일이나 다른 빌드가 섞이는 실수를 검출하는 용도입니다.
- 독립 리뷰에서 기록한 Makefile 대신 GNUmakefile이 선택되는 경우와 검증 도중
  소스가 바뀌는 경우를 재현했습니다. Makefile을 명시하고 컴파일 입력 및 패키징
  입력 전체를 다시 비교합니다. 정상 ZIP 대조와 세 실패 회귀를 함께 검사합니다.
- 상속된 `LD_RUN_PATH`가 실제 고정 GCC의 실행 파일·DSO에 RPATH를 넣었습니다.
  해당 환경을 제거하고 RPATH/RUNPATH 산출물을 거부합니다. ZIP은 같은 파일시스템의
  임시 파일에서 검증한 뒤 기존 파일을 덮어쓰지 않고 게시합니다. Git의 index flag나
  status 설정이 변경된 소스를 숨겨도 HEAD blob과 비교하여 수정 후보로 표시합니다.
- macOS의 대소문자 구분 없는 저장소에서는 도구체인의 `xt_CONNMARK.h`와
  `xt_connmark.h`가 덮어써졌습니다. Linux 컨테이너 내부 저장소에 옮겨 2,124개
  도구체인 blob을 다시 검증·복구했습니다. fetcher는 해당 저장소를 사전에 거부합니다.
- 분석기가 과거 collector 종료 개수로 현재 열린 세션을 완료 처리했습니다.
  시작 경계·PID·단조 시각으로 종료를 연결합니다. 종료 기록에 boot ID가 없다는
  한계는 유지하며 누락·역순·중복·회전 경계를 성공으로 바꾸지 않습니다.
- 정차 GPS가 기존 READY heading을 지웠습니다. 연속 센서와 일치하는 위치가
  있을 때만 기존 heading을 보존합니다. 휠 최댓값과 해당 GPS 시각까지 실제
  적분한 yaw를 사용하며, 새 seed·보정·수명 연장은 만들지 않습니다.
  센서별 64개 이력에서 실제 수신된 최신 증거를 원래 lease·age로 선택합니다.
  정상 정차 GPS만 기존 250ms 한계 안에서 yaw를 기다리고, 대기 중 출력은
  억제합니다. 만료·후속 불량 GPS·새 세션과 원래 60초 수명 검사를 유지합니다.
- 처음부터 50ms 지연된 transport MODEL 입력은 이전 유효 센서가 있어도 새
  GPS anchor를 만들지 못했습니다. 원래 증거를 별도로 전달하고 같은 후진 값을
  GPS pair와 body heading에 사용합니다. 원래 거리 학습의 입력·시간축은 유지합니다.
- 자이로 보정 후보의 실제 수신 시각이 anchor보다 뒤여도 적용됐습니다. 후보에
  포함된 마지막 휠/yaw 수신 시각을 기록하여 앞선 anchor에서는 후보를 보류합니다.
- 직진 중 한 휠만 정차한 값이 지속돼도 네 휠 평균으로 계속 계산했습니다.
  나머지 세 휠이 이동하며 서로 일치하고, 정렬된 yaw가 직진 범위인 모순이
  신선한 이벤트로 sample-age 한계를 넘어 지속될 때만 MODEL 계산을 초기화합니다.
  정상 코너링·타이어 차이·짧은 감속 시차·늦게 수신한 회전 yaw를 별도 검사합니다.
- 로더 smoke는 `LD_PRELOAD` 실패 경고 뒤에도 libc만 검사하고 성공할 수 있었습니다.
  지정한 생산 DSO가 실제 `dlopen`을 제공하는지 확인합니다. 비 PIE 실행파일의
  canonical PLT 때문에 `RTLD_DEFAULT`를 사용한 중간 검사는 오탐으로 실패했고,
  `RTLD_NEXT`와 실제 ARM 바인딩 대조로 수정했습니다.
- ARM runner가 외부 `CPATH`의 헤더를 받아 `assert(false)`를 성공 처리했습니다.
  명시한 QEMU sysroot 밖의 `libm.so.6`도 `LD_LIBRARY_PATH`로 로드됐습니다.
  실제 고정 compiler에서 두 경로를 재현한 뒤 상속 검색 경로와 guest loader
  환경을 정리했습니다. 릴리즈 검사는 전후에 소스·다섯 산출물·compiler·sysroot를
  대조하고 `ARM_TEST_INPUTS`를 기록합니다. 개발 실행은 release_verified=false입니다.

## 확인한 실행

중간 후보의 native 및 pinned ARM 합성 검사는 navigation 2,108, gyro 2,677,
GPS/wheel 12,507, GPS holdout 5,443 checks를 통과했습니다. 별도 raw parser →
channel → 계산 → adapter 합성 시험은 815 checks와 원본 7회 전달을 확인했습니다.
이는 물리 단위·측정 시각·실차 정확도의 증거가 아닙니다.

그 뒤 `1f57a38`의 정차 이력 수정은 독립 native 524개 사례·320,083개 검사를
통과했습니다. 이 중 432개 조합은 센서 순서·GPS 위상·지연·이력 순환입니다.
같은 고정 소스의 native GPS/wheel은 64,594 checks입니다. 이 결과를 앞선
ARM 실행 결과와 합쳐 최신 ELF의 ARM 성공으로 표시하지 않습니다.

새 release-input 회귀는 27개, 헤더 의존성 회귀는 6개, ARM test-input 회귀는
8개입니다. release-input 시험의 ELF는 실행하지 않는 합성 fixture입니다.
RPATH 검사는 별도로 실제 고정 compiler의 실행 파일·DSO 6개 조건을 검사했습니다.
collector 분석기 회귀는 30개입니다. 전체 최종 빌드·실행은 릴리즈 기록을 따릅니다.

2026-09-30의 `f58b0bd`까지 항법 변경은 독립 새 anchor 16군·4,384 checks와
정차 524군·320,083 checks, 거리 학습 대조 1,016 checks를 통과했습니다.
같은 코드의 새 고정 ARM 빌드와 전체 ARM 검사가 통과했고, GPS/wheel은
84,601 checks입니다. runner 전후 release_verified=true와 다섯 해시가 일치했습니다.
생산 preload SHA-256은 `79d0e60684c50378e0d8c57195eda284a9a8d63f543a5e61325851306020c4c3`입니다.
같은 후보의 전체 host `make test`도 firmware·release fixture를 지정하여 생략 없이
통과했습니다. 빌드 41, motion 50, recovery 28, loader 1, collector 10,
packaging 93, analyzer 30 Python tests와 C/C++ 실행을 포함합니다.
첫 host 재실행은 검증 컨테이너의 Git 부재로 실패했고 설치 후 새 BUILD에서
통과했습니다. 해당 실패 로그를 성공 결과로 덮어쓰지 않았습니다.
이어 `bb814a6`의 깨끗한 clone 검사는 journal formatter의 경로 전달 누락을
드러냈습니다. `test-motion-journal`이 선택한 BUILD를 전달하지 않아 앞선 작업
폴더에서는 기존 기본 build의 formatter를 사용했습니다. 그 이전 host PASS를
모든 새 formatter 바이트의 검사로 해석하지 않습니다. 경로를 수정하고 깨끗한
clone의 전체 결과를 최종 릴리즈 기록에 따로 남깁니다.

독립 리뷰어가 실제 ARM 로더 시험의 정상 preload, symlink, 상대 경로와 누락,
빈 파일, 비인터포저, 잘못된 expected, preload 순서 등 12개 조건을 대조했습니다.
`LD_DEBUG=bindings`에서도 시험 실행파일의 실제 `dlopen [GLIBC_2.4]` 호출이 생산
DSO로 연결됨을 확인했습니다. OEM 후크 설치 시험과는 다른 범위입니다.

수정 후보 ZIP은 실제 순정 ARM BusyBox/libc chroot에서 설치·제거·재설치,
일회 가드·collector UID/종료·로그 회수·기존 touch 설정 보존을 통과했습니다.
마운트와 일부 상태 로그는 fixture로 모델링했으며 실제 flash remount·전원 차단은
검증하지 않았습니다. 최종 게시 ZIP의 동일성 확인은 릴리즈별 기록을 따릅니다.
독립 감사에서 OBSERVE 묶음도 SHADOW로 기대하던 검사 오류와 해시 도구를
빠뜨린 네 파일 대조를 수정했습니다. OBSERVE·SHADOW를 각각 실행하고
다섯 산출물 모두 실제 설치된 바이트와 대조합니다.

실제 OEM SM의 명시적 재시작과 지연 SIGKILL은 [별도 실행 기록](SM_RETRY_2026-09-29.md)
을 따릅니다. 해당 OEM 실행은 e1f7ea2 payload를 사용했으므로 이번 항법 ELF의
차량 검증으로 소급하지 않습니다. 다음 물리 watchdog 재부팅은 확인하지 못했습니다.

서로 다른 Codex 하위 에이전트 네 개가 항법·OEM 계약·빌드/ZIP·릴리즈 검사를
분담했고 수정 후 다른 리뷰어가 재현했습니다. 실제 Claude Code의 읽기 전용
단일 프롬프트 정적 리뷰도 완료했습니다. 그 범위는 빌드·ZIP·도구체인·ARM runner와
loader이며 새 항법 이력 코드나 차량 실행을 Claude가 검증한 것은 아닙니다.
첫 Claude 호출은 응답 없이 중단됐고, 완료된 두 번째 호출만 리뷰 근거로 셉니다.

실제 OEM API/CAN DSO를 사용하는 제한된 chroot probe에서는 GLOBAL/LOCAL
로드 두 조건의 resolver·원본 주소/해시·callback table·SHADOW 초기화가 통과했습니다.
probe는 생산 tap 코드를 포함하되 exported hook 이름을 바꾼 진단 실행파일입니다.
원본 AddClient 등록이나 센서 입력을 호출·주입하지 않았습니다. 원래 VM 로그는
필터된 syscall과 마지막 40줄이므로 누락된 초기화 호출을 실패로 단정할 수 없습니다.
다음 VM부터 관련 파일·socket 호출을 출력에 포함하도록 진단 필터를 보완했습니다.

## 남은 구현과 근거

live 요청 출처 연결, 자격을 갖춘 물리 센서 입력, 여러 휠의 동시 오류·미끄러짐·
6MT rollback 판별은 미구현입니다. VBS 등록 성공과 실제 callback 부재를 구분하는
현장 진단도 보완 대상입니다. 현재 live ASSIST는 계속 비활성입니다. GPS holdout
차이를 실제 위치 오차나 정확도 보증으로 바꾸지 않습니다.

네이버 지도 수용, 실제 이동 경로의 정확도, 정상 전원 주기와 물리 복구, 기존
touch 입력·km/L 화면의 실제 결과가 남아 있습니다. 차량 접근 없이 만들 수 있는
증거와 실제 차량·휴대폰이 있어야 답할 수 있는 질문을 구분합니다.
