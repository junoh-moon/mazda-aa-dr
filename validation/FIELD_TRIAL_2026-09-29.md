# 통합 시험 준비 검증 — 2026-09-29

기준은 master `a319166dfe1b4a175d713291447947113506f2e4`이며 이번 변경은 `feature/first-trial-evidence`에 있다. PR #16의 PC 항법 재생 코드는 별도 변경이다. 이 기록은 호스트·합성 ARM 검증이며 차량, OEM 프로세스 실행, 네이버 지도 수용 검증이 아니다.

## 검증한 계약

- SHADOW 원본 수집은 후크/항법 모델 초기화와 분리된다. 감사 오류 후 계산은 차단하지만 기록기가 정상인 동안 raw 진단 수집은 계속한다. AA worker 자체가 시작되지 않은 경우의 수집을 보장하지 않는다.
- 인증·디코딩한 거부 입력을 `motion_rejected`로 보존하되 항법 모델·accepted 순번에 넣지 않는다. 잘못된 자격 주장, 필드 범위, 거부 사유, 미래 시각에 대한 PC 검사를 포함한다. 실제 C++ formatter 출력을 Python으로 검사한다.
- 수집 종료 시 새 AA 관측을 막고 진행 중 GPS holdout을 `capture_stop` ABORT로 끝낸다. 제한된 잔여 drain, `capture_end`, 마지막 health, 현재 trace 파일 flush/fsync/close 후 현재 boot UUID 완료 표식을 쓴다. 실패 시 완료 표식을 내지 않는다. 정지 표식이 남으면 새 worker도 journal을 열거나 회전하지 않는다.
- `finish_capture.sh`는 runtime 완료와 collector PID 표식 제거를 기다린다. collector의 정상 종료 기록과 회전 파일 전체의 전원 단절 내구성까지 보장하지 않는다.
- `trial_status.sh`는 현재 부팅과 최근 기록만 읽고 설정/예약을 바꾸지 않는다. 실제 collector의 필드 순서를 사용하는 smoke 검사를 포함한다. `/proc/uptime`의 centisecond 절삭에만 10ms 진단 허용치를 두며, 항법 freshness 조건은 바꾸지 않는다.
- 기본 bundle은 OBSERVE를 유지한다. 명시적 SHADOW bundle의 검증된 모드 파일로 `sh ./install.sh`를 사용할 수 있다. shell 코드로 source하지 않으며 ASSIST는 허용하지 않는다.

## 실행 결과

| 검사 | 결과 |
| --- | --- |
| 전체 `make test -j4` | exit 0. Python 156개 중 136 통과, 설치 fixture 관련 20개 skip |
| journal Python | 50개 통과. 새 거부 입력/종료 검증 9개 포함 |
| packaging Python | 53개 중 33 통과/20 skip. 실제 host collector journal smoke 포함 |
| 기타 Python | recovery 22, tools 20, collector 10, loader 1 통과 |
| 호스트 C/C++ | core 1,425; navigation 604; raw→MODEL→adapter 815; gyro 2,644; GPS/wheel 2,432; holdout 5,443 checks 통과. runtime/journal/adapter/forwarding/codec 검사 통과 |
| pinned GCC 4.9.1 `make arm -j4` | exit 0, 네 ARM payload 빌드 |
| `tests/run_arm_all.sh` | exit 0. 동일 합성 core/navigation/calibration/holdout, ARM veneer, journal 종료/거부 입력 등 회귀 통과 |
| 커널 Unix datagram 통합 | host/ARM 모두 환경 EPERM으로 skip. 순수 codec/cursor/인증 후 거부 분류 검사 통과 |
| SHADOW bundle 조립 | 모드 파일, 세 helper, 네 binary checksum, 모든 shell 문법 확인 |
| `libmx5dr.so` ELF | 외부 프로젝트 진입점 `dlopen`, 요구 GLIBC symbol version은 2.4뿐. libstdc++/D-Bus 의존성 없음 |
| PR #16 병합 호환성 | head `2c03e4ca`와 로컬 합성 병합 충돌 없음. 결합한 journal 58, tools 32, 재생기 8개 시험 통과. 원격 병합은 수행하지 않음 |
| `git diff --check` | 통과 |

20개 skip은 비공개 순정 identity fixture 부재 때문이다. 실제 payload 설치, 기존 touch 공존 설치, 펌웨어 해시/설치 transaction의 해당 통합 시험을 실행했다고 간주하지 않는다. 합성 shell fixture 통과로 이 생략을 대신하지 않는다.

PR #16과 같은 위치에 삽입하던 초기화/출력/문서 문단을 옮겨 기능 변경 없이 충돌을 없앴다. 이후 이 브랜치의 journal 50개와 tools 20개도 다시 통과했다. 결합 시험 수는 위 전체 156개와 별개다.

ARM은 `docs/toolchain.md`의 고정 toolchain commit `61ec0343de84f6fc7c46840056df1d600d44be8a`, Cortex-A9 ARMv7 softfp와 동봉 sysroot를 사용했다. 실행기는 QEMU 8.2.2다. OEM 바이너리는 실행하거나 공개하지 않았다.

## 조립에 사용한 ARM 산출물

아래 해시는 이번 오프라인 조립 검증의 산출물 식별자이며 공개 릴리즈 또는 설치 승인 표시가 아니다. 릴리즈는 커밋을 다시 고정하고 전체 manifest/build-info와 ZIP을 검증해야 한다.

| 파일 | SHA-256 |
| --- | --- |
| `libmx5dr.so` | `389b42e4724ed81aa2295fb1bff5b2c5d278896bdd66e0f4dec87aba5334ba85` |
| `libmx5dr-vimtap.so` | `4f4867fc33a924a5dfd02b0b877142cc4376b982daab1f7919d1a23225f457b4` |
| `mx5dr-collector` | `a3b5c6b95478a36cd18d562534688fc375c12b038388f7a169d665586116b42e` |
| `mx5dr-guard` | `ba77b78a1aced130456a563e8b0249dedcc974c62e1a08f14c864f97353dbf6f` |

## 독립 검토와 수정

OpenAI Codex가 구현을 통합하고 Astra 하위 에이전트가 독립 검토했다. Claude를 이번 검토에서 실행하지 않았다.

- 수집 종료 중인 holdout에 terminal ABORT가 빠지는 문제를 수정했다.
- 완료 시점 cutoff의 범위와 bounded drain 표시를 PC에서 검증하도록 보완했다.
- collector 정상 종료·회전 파일 저장 보장에 대한 문서의 과도한 표현을 고쳤다.
- 실제 collector와 상태 helper의 통합 검사에서 시각 정밀도 차이로 생기는 간헐적 판정을 수정하고 정확한 10ms 경계 회귀 검사를 추가했다.

현재 확인한 미해결 차단 결함은 없다. 이는 설치 안전성이나 관성항법 정확도 판정이 아니다. [한 번의 시험 계획과 남은 확인](../docs/FIELD_TRIAL_KO.md)을 따른다. 생산자 시각·품질, 실제 센서 보정, CMU 기동/복귀, 폰/앱 수용은 실차 근거가 필요하고 ASSIST는 계속 차단한다.
