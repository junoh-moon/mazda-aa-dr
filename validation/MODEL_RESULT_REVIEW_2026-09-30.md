# MODEL 결과 의미 검사·생산자 대조 — 2026-09-30

기준 master는 `cb65fd5971052235d0b726f31574630cfa22c93d`입니다.
분석기가 `model_valid=true`이면서 UNSEEDED·E_NO_SEED·null 좌표인 행도
정상으로 판정하던 공백을 재현·수정했습니다. 실제 작성 계산기의 정상 경계값과
무효 출력을 함께 검사합니다. 이 변경은 PC 분석기와 시험 코드이며 제품의
계산·설치·OEM 전달 동작을 변경하지 않습니다. ASSIST와 공개 ZIP은 그대로입니다.

## 재현과 수정

깨끗한 boot·position/send 바이트 쌍·health를 갖춘 입력에 위 모순 행을 넣으면
기존 CLI는 `local_checks_pass`, exit 0이었습니다. 수정 후 같은 입력은
`shadow_result_inconsistent` violation 한 건, exit 1입니다. 다른 누락 때문에
원래 inconclusive였던 자료를 거짓 통과 재현으로 사용하지 않았습니다.

`get_snapshot`과 `Pipeline::diagnostic`의 결과 계약을 확인하여 다음을 검사합니다.

- MODEL 유효성은 query 결과 OK와 일치해야 합니다. 유효 해는 ACTIVE 상태,
  양의 frontier와 event/interval counter, 현재보다 늦지 않은 frontier가 필요합니다.
- 유효 위도는 (-85,85), 경도는 [-180,180], 방향은 [0,2π]이며 속도는 비음수,
  오차는 [0,100]입니다. null·bool·비유한 수치·binary64 범위를 넘는 정수는
  유효한 수치가 아닙니다. stopped이면 속도 0이어야 합니다.
- E_TIME/E_STALE/E_LIMIT query는 ACTIVE와 양의 frontier를 필요로 합니다.
  E_CONFIG 출력은 상태 0입니다. E_STALE의 frontier는 query 이후일 수 없습니다.
  OK/E_LIMIT는 stale 검사 뒤에 나오므로 모든 지원 config의 150ms query-age
  상한을 만족해야 합니다. 알려지지 않은 query/pipeline 이름은 불완전 자료입니다.

반대로 ACTIVE가 항상 유효하거나 pipeline이 항상 OK여야 하는 것은 아닙니다.
무효 해에는 이전 좌표·속도 또는 100을 넘는 오차가 남을 수 있습니다. E_STALE은
센서 lease가 끝나면 age 1ns에도 나올 수 있고, E_TIME은 frontier가 과거여도
receipt가 미래이면 나올 수 있습니다. 수신 시각을 생산·적분 시각으로 바꾸지 않습니다.

경도 +180과 방향 2π는 실제 binary64 연산에서 생기는 정상 출력입니다.
아직 정지 유지 시간을 채우지 않은 유효 속도 0도 가능하며 preview가 없을 수
있습니다. 일반 core config에는 속도 100m/s의 고정 상한이 없으므로 로그에
없는 설정을 가정하지 않습니다. 기존 preview 검사는 유효성·48바이트 모양의
계약을 유지하며 전체 바이트와 계산값의 대응 감사로 확대하지 않았습니다.

기존 session/bus 회귀의 잘못된 양성 fixture도 유효 상태·수치를 가진
`valid_shadow`로 바꿨습니다. 경계 위반을 다른 의미 오류로 가려 통과시키지 않습니다.

## 실제 생산자 출력과 공개 회귀

`test_journal --emit-model-results`는 실제 Pipeline 입력 10개와 직접 core
query 8개를 출력합니다. 코어 상태를 강제로 채우거나 성공 반환을 대체하지 않습니다.
Pipeline 행은 실제 status counter를 사용합니다. 직접 core 행은 별도
`test_core_snapshot`으로 표시하고, Python에서 실제 snapshot 필드만 명시적인
작성 metadata와 결합합니다. 그 metadata를 worker 관측으로 주장하지 않습니다.
수치/hex helper는 생산 코드의 것이지만 fixture의 printf는 전체 worker formatter가
아닙니다. 기존 실제 worker 로그의 호환성은 아래에서 따로 검사했습니다.

공개 12개 Python 메서드는 개별 필드 모순, 알려지지 않은 상태, malformed 수치,
정상 무효 상태와 수치 경계를 포함합니다. 실제 출력 대조에서는 valid·WAITING,
대기 GPS/native의 즉시 철회, unseeded/native 상태, 정지 전 속도 0, 정지 확정,
+180/2π, 오차·시간 제한을 확인합니다. age 시험은 50ms step을 사용하여
query age 150ms/150ms+1ns 모두에서 250ms 센서 lease가 살아 있도록 분리했습니다.
GNU와 Clang의 작은 속도 잔차 차이는 허용합니다. raw wheel 10000의 같은 입력이
GNU에서는 정확히 0, Clang에서는 약 5.78e-16이 될 수 있습니다.

## 독립 리뷰

세 리뷰어는 작성 소스만 별도로 검토했습니다. 아래 수치는 각 리뷰어의 실행이며
root의 host/ARM 실행과 합산하지 않습니다.

- 독립 의미 matrix 125개(정상 28·음성 97)에서 기준선의 거짓 통과 68개를
  확인했고 최종 분석기는 불일치 0이었습니다. 검사 제거 또는 과잉 제한 변형
  27개를 독립 matrix와 공개 회귀 양쪽에서 검출했습니다. 별도 GNU 빌드의
  공개 producer 18개와 독립 probe의 같은 13개 필드를 대조했습니다. 실제
  core의 query-age 검사만 제거한 변형도 새 공개 회귀가 검출했습니다.
- 다른 리뷰는 실제 작성 worker의 유효 행에서 필드 하나씩만 바꾼 거짓 통과
  19개를 재현했습니다. 최종본은 의미 위반 18개·미지 결과의 불완전 판정 1개로
  거부하며 정상 대조 7개를 유지했습니다. 공개 회귀 60개와 변형 12개를 검사하고
  후속 producer 두 메서드를 별도로 실행했습니다. 정상 상태의 명시적 assert와
  age/lease 분리 제안을 반영했습니다.
- 생산자 계약 리뷰는 실제 core/Pipeline/preview의 경계 출력 34개를 native,
  ASan/UBSan, GNU에서 각각 실행했습니다. 경도·방향 반올림, 짧은 lease 만료,
  무효 값 보존과 preview 표현 한계를 확인했습니다. ARM/OEM 실행은 아닙니다.

범위 내 남은 확정 P1/P2는 보고되지 않았습니다. 별도 P3 결함은 아래에 남깁니다.

## root의 직접 실행과 재분석

전체 `make test`는 GNU 환경에서 Python **330개**와 C/C++ 검사를 skip 없이
통과했습니다. stock rootfs와 기존 검증 제품 bundle 경로를 명시하여 패키징
검사도 실행했습니다. 작성 worker 23개는 raw **12,146개**를 보존했고 각각
일반 분석은 inconclusive·violation 0입니다. reset/rejection을 정상 통과로
승격하지 않습니다. 마지막 fixture 초기화·양성 assert 변경 뒤 관련 host
journal **94개**를 다시 통과했습니다.

고정 GCC 4.9.1과 QEMU ARM으로 journal fixture를 다시 빌드·실행하고 공개
12개 메서드를 통과했습니다. producer 18개 출력은 이 실행의 host/ARM에서
파일 해시까지 같았습니다. ARM 전체 runner에도 새 검사 호출을 연결했습니다.
이번에는 ARM 전체 suite 또는 새 원본 VM을 실행하지 않았습니다. 제품 소스
57개 입력과 기존 제품 artifact 5개가 이전 검증 기록과 일치함은 별도로 확인했습니다.

이전 host/ARM worker 46개와 원본 VM journal 5개를 root가 직접 재분석했습니다.
기준선/수정본의 **전체 보고서가 51개 모두 동일**했습니다. 모두 기존
inconclusive 판정을 유지하고 새로운 violation은 없습니다. 이는 기존 로그의
재분석이며 새 펌웨어 실행 또는 유효 GPS 재현이 아닙니다.

실패한 시도도 보존했습니다. 첫 전체 host 실행은 root가 결과 저장 디렉터리를
만들지 않아 fixture의 output-open assertion에서 종료됐습니다. 경로를 만든 뒤
전체를 다시 실행했습니다. 첫 ARM compile은 새 fixture의 `{}` 초기화가 고정
GCC의 missing-field 경고에 걸렸으며 `Type()`으로 고쳐 재빌드했습니다.
초기 raw 집계 보조 명령도 batch의 필드를 잘못 지정하여 KeyError로 실패했고,
실제 `events` 배열을 읽는 보존된 보조 스크립트로 12,146개를 대조했습니다.
이 준비 오류들을 제품 결함이나 추가 성공 수로 계산하지 않습니다.

## 외부 작업과 남은 범위

외부 Claude 브랜치의 `b4ea8f2` 이후 `1ccedbfd6b6eaa68fb5d953d4c468ada9d12c379`
병합을 확인했습니다. 우리의 cb65fd5 수정이 포함됐으며 별도 새 GPS 성공 증거는
추가되지 않았습니다. 앞선 [외부 LDS 시작 조사](https://github.com/junoh-moon/mazda-aa-dr/blob/c3e7ecc964aecf1b1b5b562700bc36cfdd375d31/validation/LDS_INPUT_STARTUP_2026-09-30.md)의
SYSTEM 상태 변화·USB 목록 요청은 유효 NMEA 소비를 입증하지 않습니다.
외부 기록과 직접 실행을 구분하며 이번 단위에서 Claude CLI는 실행하지 않았습니다.

**미구현 TODO:** 공개 Pipeline API에 UINT64_MAX-1의 generation을 주어 정상
예측을 만든 뒤 generation 소진 상태에서 입력 fault를 발생시키면, Pipeline은
비활성화되지만 core query가 기존 model_valid를 다시 true로 만들 수 있습니다.
독립 native/GNU assertion으로 재현했습니다. 일반 시작값에서의 도달성은 극단적이며
흔한 운행 장애로 주장하지 않습니다. 생산자 수명 정리의 별도 수정 대상입니다.
분석기에서 `pipeline!=OK`를 일괄 금지하여 이 문제를 숨기지 않았습니다.

이 검사는 기록 필드 사이의 필요조건입니다. 센서 provenance, 물리 정확도,
유효 원본 GPS, 정상 전체 기동·복구, 폰/앱 수용과 ASSIST 자격은 여전히 남습니다.
공개 ZIP은 `v0.3.1-shadow.1`이며 이번 결과로 v1.0 준비 완료를 선언하지 않습니다.

## 고정 파일 해시

전체 실행·실패 로그와 리뷰 사본은 비공개 `evidence/model-result-review-20260930/`에
보존했습니다. OEM 바이너리·원본 실행 로그는 게시하지 않습니다.

| 파일 | SHA-256 |
| --- | --- |
| `tools/analyze_logs.py` | `a104cb9b167cca5812c5a414935c1caa971647e4e22b85e70115a8e8efa17087` |
| `tests/runtime/test_journal.cpp` | `83d3d1d70528df46a446111b9665945a55787e2b7c6612fac48812b9791d84d2` |
| `tests/journal/test_shadow_results.py` | `4f24305f4049c07c8bbb5ff4776490212237e2ed0237b738d787e7d7ae1074c3` |
| host/ARM producer 18개 출력 | `d964f1018b58dc066f3d5174f873f9b7a48d75ccfdc4888424a7356ff17b7253` |
| 최종 전체 host 로그 | `54cb4ec7ddd9619d9157ae0e72d4cd43dfbeb3e7c935853703a339239de3f7b0` |
| ARM journal 빌드·실행 로그 | `8a6a03df721adb242bcce3e3f45f8995d4e77afe0ebf8910f016d7601df0b58d` |
| 기존 51개 로그 비교 | `ce299ea540e89cedc5c3bacf7db86dd01ea849f0615b36097e8fa0975e537b6e` |
