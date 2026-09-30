# 세션 후보 철회·MODEL 초기화의 독립 통합 검증 — 2026-09-30

NA 74.00.324A 전용입니다. master `20bf583`의 동시성 수정을 유지하면서
독립 브랜치의 `903e2d7`·`a15e114`를 검토·통합했습니다. 외부 실행 수치를
이번 결과로 옮기지 않았으며 기존 검증 기록도 수정하지 않았습니다.
현재 공개 ZIP은 v0.3.1-shadow.1 그대로입니다.

## 확인한 문제와 변경

이전 세션의 항법 기준점과 보정값이 새 세션에 남을 수 있었습니다. 독립
리뷰어가 `20bf583`의 실제 worker를 합성 socket 입력으로 실행하여 재생성 뒤
오래된 valid MODEL 13건을 재현했습니다. 시험용 socket 이름을 주입하는
두 줄 외에는 이전 runtime에 새 gate/reset을 넣지 않았습니다.

- create/destroy/status 진입·복귀에서 송신 후보를 철회합니다. 실패·예외·취소도
  포함하며, 완료 revision은 실패한 생성이나 늦은 callback도 구분합니다.
- 원본 storage 재읽기 제거와 겹친 lifecycle의 관측 fault 처리를 유지합니다.
  callback 전환과 create/destroy 중첩을 별도로 세어 정상 초기 callback을
  lifecycle 경쟁으로 오인하지 않습니다. 원본 호출·본문·반환·errno는 보존합니다.
- worker가 세션 경계를 관측하면 주 계산과 holdout의 기준점, 학습·적용 보정값,
  대기 입력을 초기화합니다. 이전 요청 revision·미관측 요청·잘못된 관측 시각
  순서는 MODEL 입력에서 제외합니다. 일반 GPS→GAP 전환은 이 초기화가 아닙니다.
- 새 경계보다 앞선 receipt 또는 기존 MODEL transport 시각의 센서 입력은
  새 계산에 넣지 않습니다. 원시 자료와 `shadow_motion_excluded` 사유를 함께
  남깁니다. 음수·overflow·미래 시각 및 경계 이후 clock 변경의 기존 fault는
  유지합니다. transport 해석을 물리 생산 시각의 증명으로 승격하지 않습니다.
- 분석기는 revision 변경에 가려지던 상태 이력 모순, 불가능한 revision/event
  조합, 일부만 revision이 있는 기록, 경계 이전의 valid frontier와 미래
  frontier를 구별합니다. 경계 reset·입력 제외는 inconclusive로 남습니다.

관측한 세션은 요청 소유권·receiver 자격·폰 연결의 증명이 아닙니다.
live `provenance()`는 false이며 `allow_assist=false`도 유지합니다.

## 독립 리뷰와 회귀 보강

작성 코드만 받은 세 리뷰어가 각각 세션 동시성/후보 철회, MODEL reset/입력,
분석기를 검토했습니다. OEM 실행은 주 에이전트가 직접 수행했습니다.
네 번째 리뷰어 생성 및 기존 다른 리뷰어 재사용은 도구의
`agent thread limit reached`로 실행되지 않았습니다. 네 리뷰로 세지 않습니다.

| 검토 범위 | 재현과 후속 검증 |
| --- | --- |
| lifecycle 통합 | 이전 독립 소스의 storage race를 TSan으로, 겹친 destroy/create의 잘못된 live 문맥을 별도 fixture로 재현했습니다. 현재 통합본은 해당 회귀와 초기 callback을 통과했습니다. |
| 진입 후보 철회 | 기존 검사는 진입 invalidate를 제거해도 통과했습니다. `prediction_cached_inflight`는 이미 복사한 OBSERVED 문맥과 오래된 후보를 가진 송신을 실제 callback 진행 중에 재개하여 이 mutation을 검출합니다. |
| 학습값 초기화 | 기존 worker 여섯 검사는 nav/holdout 각각의 보정값 보존 mutation을 놓쳤습니다. 실제로 yaw 2067·wheel scale 1.03을 학습·적용한 뒤 초기화와 새 holdout BEGIN의 명목값 2047·1, 버전 0을 검사하여 두 mutation을 검출합니다. |
| 센서 시각·원본 보존 | 10개 입력 사례에서 old receipt/transport 제외와 현재 clock fault를 구분합니다. 모든 원본 13개 row의 10필드·순서·epoch·미확인 producer 시각을 독립 기대값과 비교합니다. 제외 branch의 원본 기록 삭제 mutation도 검출합니다. |
| 분석기 | 이력 조합 1,586개와 legacy 14개, 추가 384개 및 제외 기록 조합 407개를 검토했습니다. 정상 worker 로그 456개 row의 오탐은 없었습니다. 검사를 제거·완화한 parser 및 C++ mutation도 별도로 검출했습니다. |

독립 리뷰를 통해 보강한 회귀는 공개 `tests/`에 포함했습니다. 초기 실패와
mutation 로그·상세 리뷰는 ignored `evidence/model-session-review-20260930/`에
보존합니다. 이 리뷰어들은 OEM·차량·폰을 실행하지 않았습니다.

| 최종 리뷰 기록 | SHA-256 |
| --- | --- |
| 세션 후보 철회 | `7d58e50d7e1a33fde133d1e62d9dce559a9bafb59a999153291cf8001957360c` |
| MODEL reset/입력 | `9595b2e54f7e8e269ff8b5865b977fbc1af3e56f55f7bafe1e480f0937101ad7` |
| 분석기 | `5849e37191e3d7280453d9d7e90f5ca414aa80fdd69f645585bdd2f3df229d04` |

## 직접 실행한 작성 코드 검사

| 범위 | 결과 |
| --- | --- |
| GNU `make test` | Python 300개와 C/C++ 전체 검사 통과, skip 0. 원본 BusyBox와 현재 ARM bundle 경로를 명시했습니다. |
| 고정 ARM 전체 | GCC 4.9.1·ARM32 softfp로 최종 제품을 고정하여 전체 통과, skip 0. 실제 DSO position 8·request 14·session 29개를 포함합니다. |
| 실제 worker/socket | 파괴·재생성·상태·생성 실패·중복·진행 중 callback 여섯 사례가 host/ARM 모두 통과했습니다. 각각 원시 위치 9건, stale valid 0, 새 GPS 기준점 뒤 회복을 확인했습니다. 합성 입력입니다. |
| 마지막 test-only 보강 | 전체 검사 뒤 원본 13개 row 대조를 강화했습니다. 변경한 입력 회귀를 GNU와 고정 ARM에서 각각 다시 빌드·실행하여 통과했습니다. 생산 소스는 바뀌지 않았습니다. |
| 빌드 입력 | 제품 manifest의 53개 실제 입력과 최종 소스를 대조했습니다. 제품 라이브러리 SHA는 아래에 고정합니다. |

새 parser 회귀의 수정 전 실패 로그도 남겼습니다. 잘못된 MODEL 시간 6개,
revision 이력 5개와 snapshot 형식 3개 assertion 실패를 확인한 뒤 수정했습니다.
후속 성공을 이전 실패 로그에 덮어쓰지 않았습니다.

## 원본 펌웨어 실행

원본 커널·공유 runtime·LDS·AA를 NIC·호스트 장치·공유 디렉터리가 없는 VM에서
직접 실행했습니다. 전용 진단 init/caller와 커널 진입 machine ID 인자 조정을
사용했습니다. 원본 kernel 바이트와 OEM 바이너리는 바꾸지 않았으며 userspace
debugger나 LDS 내부 객체 추적은 사용하지 않았습니다. 순정 전체 SM 기동·
물리 센서·폰 시험이 아닙니다.

동일 제품 DSO를 SHADOW로 자동 설치한 별도 두 프로세스에서 원본 manager의
LDS 요청을 실행했습니다. 원본 AA용 util 요청 네 건의 클라이언트 dispatch를
지연하고 실제 세션을 파괴·재생성한 뒤 응답을 전달했습니다. 비교한 조건은
manager를 정지 상태로 둔 경우와 재시작한 경우입니다. 양쪽 모두 새 세션은
생성되어 있으며, 첫 조건을 세션 부재 시험으로 해석하지 않습니다.

| 원본 실행 | manager 정지 유지 | manager 재시작 |
| --- | ---: | ---: |
| 원본 LDS 요청 / 이전 세션의 지연 요청 | 14 / 4 | 16 / 4 |
| 지연 요청의 SEND / LOCATION | 0 / 0 | 8 / 4 |
| 지연 LOCATION의 실제 반환 / send lifetime | 해당 없음 | 0 / 2 |
| 합성 원시 센서 / 지연 응답 구간 센서 | 336 / 20 | 587 / 21 |
| 이전 요청의 MODEL 제외 | 4 | 4 |
| 새 세션 요청 | 0 | 2 |
| journal drop / request loss / session fault | 0 / 0 / 0 | 0 / 0 / 0 |

각 이전 요청은 issue lifetime 1/revision 1을 보존했고, 재시작 조건의 송신은
lifetime 2/revision 3으로 구분됐습니다. 두 조건 모두 이전 요청 네 건을
`session_changed_since_issue`로 MODEL에서 제외했습니다. 별도 합성 GPS seed
각 한 건도 `request_unobserved`로 제외됐습니다. 원본 LDS 응답의 mode·UTC·
좌표는 모두 0이며 모든 MODEL 출력은 invalid입니다. 원본 송신 본문과 반환을
보존했고, 합성 센서 입력은 수락된 수·모든 필드·순번을 저장 로그와 대조했습니다.
물리 VBS callback이나 유효 GPS→터널 계산·회복을 입증한 실행이 아닙니다.

별도 원본 INVALID 상태 callback 두 주기에서는 생성·callback·파괴의 generation이
각각 2→4→6→8, 8→10→12→14, revision이 0→1→2→3, 3→4→5→6이었습니다.
start 0·stop 264와 원본 정리 반환을 확인했으며 정상 폰 연결로 해석하지 않습니다.
작성 session DSO 회귀 29개도 같은 제품과 원본 공유 runtime에서 통과했습니다.

세 capture 모두 종료 marker·마지막 health·동일 boot ID의 durable ack를 확인했습니다.
원본 caller exit는 모두 0입니다. VM helper는 220초 제한에서 exit 124,
QEMU exit 0·`timed_out=true`였으며 이 종료 값은 성공 판정 근거가 아닙니다.
console/image/product/caller 해시를 검사한 판정기는 누락 종료·가짜 완료·
재시작 근거 누락·이전 요청 제외 변경·가짜 valid MODEL·raw batch 누락·
issue 문맥 변경의 일곱 mutation을 모두 검출했습니다.

일반 분석기는 세 로그 모두 inconclusive(exit 2), violation 0입니다. MODEL
세션 reset·입력 제외와 합성 sensor source fault가 남아 있으며, 정지/재시작
조건의 기록된 MODEL reset 최대값은 각각 14/7입니다. 이를 깨끗한 센서 연속성이나
항법 정확도 합격으로 바꾸지 않습니다. 요청/journal loss 0과도 별개의 지표입니다.

| 고정 산출물 | SHA-256 |
| --- | --- |
| 제품 `libmx5dr.so` | `04e180226c9aa59bf4506e9ca0552917588d83fc07e48ef15a5b2f5b569887fb` |
| 비공개 initrd | `9341b84ef69ec3e418dca0a76867dd8fe31ecbd9ce83fb0fc76e5d802eaab483` |
| 최종 console | `ba28ae5a51dd99e93acaed43c84adc0e4c1b24edb5c73f8501915f1bfafedfcd` |
| 전용 판정기 | `4ff43ff999df0c705f657e94c5a5e16fd3891e81feecad4203a720edbbbcfbcb` |

원본 바이너리·주소 자료·전체 console은 공개하지 않습니다. 실행 산출물과
명령·실패 근거는 위 ignored evidence에 보존했습니다. 새 패키지·도구를
설치하지 않았으며 기존 컨테이너와 고정 toolchain을 사용했습니다.

## 새 외부 조사 추적과 남은 범위

작업 도중 SSH fetch로 `91f2c93`과 `11c115e`를 추가 확인했습니다.
[요청 경로 보존 기록](https://github.com/junoh-moon/mazda-aa-dr/blob/91f2c9324448b8ff5b1aa4e0514d205784368d88/validation/REQUEST_ROUTE_2026-09-30.md)은
발행 당시 destination/path/interface/name 복사와 큰 JSON의 종료 배출을 다룹니다.
[후속 병합 기록](https://github.com/junoh-moon/mazda-aa-dr/blob/11c115e3ad8bede234dcfd743c1e84e3e5f443fa/validation/SESSION_MERGE_2026-09-30.md)은
master의 동시성 수정 및 해당 route 기능의 통합·별도 VM 결과를 남겼습니다.
그 실행은 이번 주 에이전트의 실행이 아니며, route 변경도 이 체크포인트에는
포함하지 않았습니다. 별도 검토 대상으로 유지합니다.

이전 [세션 반복 VM 정리 실패](SESSION_CONTEXT_REVIEW_2026-09-30.md)의 정확한
원인은 여전히 미분리입니다. 이번 성공으로 그 실패를 해결 처리하지 않습니다.
bus 수명·실제 provider·receiver/session 소유권의 qualified 연결, 물리 센서
단위·품질·시각, 유효한 원본 GPS와 실제 위치 정확도, 폰/앱 수용, 정상 전체
SM 기동과 물리 복구 검증은 남습니다. 이 기록은 v1.0 또는 실차 승인 기록이
아니며, 추가 차량 방문·폰 시험을 요청하지 않았습니다.
