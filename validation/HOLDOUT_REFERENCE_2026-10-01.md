# GPS holdout의 원본 관측 식별자 보존 — 2026-10-01

구현 커밋은 `40051f8afffa38443e0db13bef6c592c8d80a854`입니다.
holdout 결과에서 사라지던 원본 위치 관측의 call/generation을 보존하고,
분석기가 같은 기록 묶음·세션의 원본 position 행에 직접 연결하도록 했습니다.
이는 **진단 정보 유실의 수정**입니다. 기존 계산이 잘못된 GPS 참조를 사용했다는
증거나 위치 정확도 향상의 측정 결과는 아닙니다.

MODEL 계산, 참조 GPS의 예측 입력 제외, 보정 적용 시점, 원본 기록 순서,
저장 정책과 live ASSIST 비활성은 유지합니다. 연결 실패 때문에 제품 계산을
중단하거나 새 대기·자격 조건을 추가하지 않았습니다. 원본 관측 ID는 생산자의
측정 시각·필드별 출처·물리 품질을 검증한 식별자가 아닙니다.

## 정보가 사라지던 지점과 수정

`GpsHoldout`의 대기 큐에는 `adapter::Observation` 전체가 값으로 남아 있었습니다.
늦은 센서 입력을 기다린 뒤에도 실제 참조의 position, receipt time, call 및
adapter generation을 사용할 수 있었지만, `emit()`이 결과에 position과
receipt time만 복사하면서 call/generation이 소실됐습니다.

`HoldoutResult`에 실제 참조 존재 여부와 두 uint32 식별자를 추가했습니다.
`emit(o)`에 원본 관측이 있을 때만 그 값들을 복사합니다. 내부 MODEL context의
generation이나 처리 시점의 최신 ID로 바꾸지 않습니다. formatter는 항상
`reference_call`과 `reference_generation` 두 키를 출력합니다.

| 실제 결과 생성 경로 | 새 JSON 식별자 |
| --- | --- |
| BEGIN / COMPARED / SKIPPED의 실제 참조 | 원본 uint32 정수 두 개 |
| END / 일반 ABORT / reset의 참조 부재 | `null`, `null` |
| 결과 큐 초과로 바뀐 ABORT / output_overflow | 바뀌기 전 실제 참조의 존재 여부 유지 |

0과 UINT32_MAX도 유효한 관측 값입니다. 0을 참조 부재의 표시로 사용하지 않습니다.
관련 구현은 `src/navigation/holdout.h`, `holdout.cpp`,
`src/runtime/shadow_log.h`입니다.

## 분석기의 연결 범위

`tools/analyze_logs.py`는 앞서 읽은 원본 position의 정확한
`(call, generation)`으로만 찾습니다. 식별자로 선택한 뒤 원본 `mono_ns`와
`reference_ns`, COMPARED의 원본 좌표와 `ref_lat/ref_lon`을 대조합니다.
시각 근접성·좌표 유사성·최신 위치로 대신 연결하지 않습니다.

`shadow_holdout.reference_links`는 다음 상태를 별도 집계합니다.

| 상태 | 의미 |
| --- | --- |
| `matched` | 같은 기록 범위의 단일 원본 행과 식별자·일관성 검사가 일치 |
| `legacy_without_identity` | 과거 형식으로 두 식별자 키 모두 없음 |
| `no_reference` | 참조 없는 결과의 명시적인 null 쌍 |
| `raw_missing` | 해당 ID의 앞선 원본 행이 회수된 기록에 없음 |
| `ambiguous` | 같은 ID의 원본 행이 중복되어 하나를 고를 수 없음 |
| `mismatch` | ID로 고른 원본과 기록 시각 또는 비교 참조 좌표가 모순 |
| `malformed` | 일부 키 누락, 잘못된 타입·범위 또는 해당 사건에서 불가능한 ID |

뒤늦게 중복 원본 행이 나타나도 이전 `matched` 집계를 `ambiguous`로 바꿉니다.
중복을 마지막 값으로 덮어써서 성공으로 세지 않습니다. 원본 소실·불량 ID 등은
진단으로 남기며, 그 때문에 별도로 유효한 COMPARED 횟수와 차이 통계를 지우지
않습니다. 뒤에 나온 raw 행으로 이전 `raw_missing`을 추정 복구하지 않습니다.

일반 파일은 같은 실제 디렉터리의 `trace.N.jsonl` 회전 묶음만 이어 읽습니다.
tar는 archive 자체와 member 디렉터리도 구분합니다. 서로 다른 export나
임의 파일의 boot 누락이 앞선 기록의 ID·health·window를 이어받지 못하게 했습니다.
collector와 storage 진단 파일을 읽는 일은 AA trace 회전의 연속성을 끊지 않습니다.
AA boot가 나타나면 ID 공간을 다시 시작하며, 불완전한 boot도 이전 ID를 가져오지
않습니다. 입력 그룹 전체를 보관하는 별도 cache는 추가하지 않았습니다.

연결 범위는 `reference_link_scope=same_trace_group_and_recorded_session`입니다.
boot가 없는 같은 묶음 안의 행끼리는 일치할 수 있지만 `missing_boot`와 불완전
판정을 유지합니다. 실제 프로세스가 같았다는 증명으로 확대하지 않습니다.
`reference_exclusion=not_provable_from_journal`, `gps_is_ground_truth=false`,
`time_basis=receipt_model`과 기존 비교 통계의 범위도 유지합니다.

## 기존 overflow 판정의 보완

결과 큐가 가득 차면 이미 만든 결과를 ABORT/output_overflow로 바꾸는 동작은
이번 변경 전에도 있었습니다. 따라서 window가 시작되기 전 SKIPPED의 참조가
남은 채 `window_id=anchor_ns=frontier_ns=0`, `reference_ns>0`인 ABORT가
실제로 나올 수 있습니다. 기존 분석기는 이 행을 `invalid_holdout_time`으로
분류했습니다.

실제 formatter 출력으로 그 실패를 확인한 뒤, 해당 overflow·참조·빈 MODEL
예측 형태를 `holdout_aborted`로 처리하도록 보완했습니다. 두 새 키만 제거한
과거 형식에서도 같은 실패를 재현하여 `legacy_without_identity`와 중단 진단을
함께 남기도록 했습니다. 일부 키만 존재하거나 불량/null ID인데 양수 참조 시각을
가진 모순은 예외로 허용하지 않습니다. 미래 참조 시각, 잘못된 anchor/frontier,
MODEL 예측 또는 차이 값도 기존 검사에서 거부합니다.

두 형식 모두 **ABORT/inconclusive**이며 완료 창이나 사용 가능한 비교 결과로
세지 않습니다. 이는 기존에 가능한 출력 형태의 분석 보완이고, 새 제품 계산
결함을 수정했다거나 과거 중단을 성공으로 바꾼 것이 아닙니다.

## 수정 전 실패와 대상 검사

대상 검사는 host에서 실행했습니다. 전체 소스를 고정해 새로 빌드한 검증 및
ARM 검증은 아래 별도 상태를 따릅니다.

| 검사 | 수정 전 실제 실패 | 수정 후 대상 결과 |
| --- | --- | --- |
| enqueue → 센서 대기 → 실제 결과 → formatter | BEGIN의 `reference_call:101` 누락, exit 1 | 원본 객체를 덮어써도 BEGIN 101/7, COMPARED 202/8·303/9 보존 |
| formatter의 최대 ID·null·버퍼 | UINT32_MAX 식별자 키 누락 assertion, exit 134 | 정수 경계·null·버퍼 검사 통과 |
| 원본 연결 분석 | 새 18개 검사에서 예상 assertion 실패 42건, 오류 0 | 최종 calibration/holdout Python 51개 통과 |
| terminal 사건의 ID | END·일반 ABORT의 불가능한 숫자 ID를 잘못 분류한 두 실패 | `malformed`로 구분, 유효 비교 통계 보존 |
| 실제 overflow 및 과거 형식 | 각각 `invalid_holdout_time` 분류 실패 | 중단/inconclusive로 분류, 모순된 형태 거부 |
| 기존 파일·archive·SEND·collector 분석 | 기존 회귀 사용 | Python 34개 통과 |

최종 host holdout은 **7,258개 synthetic check**를 통과했습니다. 지연 큐의
원본 ID, 0/UINT32_MAX 양방향 경계, 참조 없는 terminal, 실제 큐 초과의 참조
유무를 확인했습니다. held-out 좌표·방향·속도와 참조 ID를 달리하는 기존 예측
일치 검사도 유지했습니다.

formatter의 최대값 시험 입력은 **NUL 포함 773바이트**, 실제 worker 출력
버퍼는 **2,200바이트**입니다. 정확히 필요한 길이의 성공, N-1 실패, NUL 종료,
앞뒤 canary를 검사했습니다. 773은 해당 작성 경계값 시험의 결과이며 모든
입력에 대한 수학적 최대 길이를 새로 증명했다는 뜻은 아닙니다. 이 버퍼는 원본
position formatter의 별도 버퍼와 구분합니다. 위 대상 검사에서 생략은 없습니다.

## 실제 host worker의 raw 기록 연결

제품 변경 전 소스 `028197108b6be39c71baffaa21c0d5cc31371a70`에 새 worker
회귀만 적용해 별도로 빌드했습니다. `bus_reuse` 실행은 실제 기록의
`reference_call` 키 누락을 읽는 assertion으로 exit 134였습니다. 수정 후 같은
실제 worker 시나리오는 exit 0으로 통과했습니다.

이 검사는 실제 pthread worker, motion socket, 기록기와 제품 계산기를 실행합니다.
센서 입력과 bus lifecycle은 작성한 fixture입니다. OEM 전체 서비스·물리 센서·
차량을 실행한 것으로 세지 않습니다. 원본 position을 먼저 기록한 뒤 그 ID와
receipt time에 holdout 결과가 연결되는지도 검사합니다.

회수된 로그의 별도 분석 결과는 다음과 같습니다.

| 항목 | 관측 결과 |
| --- | --- |
| 원본 position / holdout 행 | 9 / 6 |
| holdout 사건 | BEGIN 1, ABORT 5 |
| 참조 연결 | `matched=1`, `no_reference=5` |
| 비교 결과 / 완료 창 | 0 / 0 |
| capture 종료 기록 | 1 |
| health의 최대 drop / audit fault | 0 / 0 |
| 전체 분석 판정 / 종료값 | `inconclusive` / 2 |

전체 분석의 불완전 판정은 유지합니다. 이 fixture는 설치를 실행하지 않아
`install=not_attempted`이고, bus 전환·입력 제외·작성 단절·reset·capture 중단을
포함합니다. 검사할 LOCATION 송신 payload 쌍도 없습니다. 실제 worker 회귀의
성공과 로그 전체의 `inconclusive`는 서로 다른 검사 계약입니다. BEGIN 한 건의
연결을 GPS 비교 성공, 완주, 송신 수용 또는 위치 정확도로 해석하지 않습니다.

## 오래된 workspace 실행 파일을 사용한 실패

추가 journal Python 전체 시도는 123개 검사 중 실패 3건·오류 21건으로 종료했습니다.
이 실행은 기존 workspace의 오래된 `build/test_journal`을 사용했습니다.
직접 출력도 `--emit-positions`에 JSON 대신 일반 시험 요약을 내보내고,
`--emit-requests`에 현재 계약의 6행 대신 5행과 `wire` 없는 객체를 반환했습니다.
MODEL fixture 이름도 현재 기대값과 달랐습니다. 분석기를 거치기 전 출력 계약
불일치 근거와 실패 로그를 보존했습니다.

이를 전체 통과로 세거나 실패를 숨기기 위해 검사·제품 조건을 낮추지 않았습니다.
고정 소스에서 새로 빌드하는 후속 전체 검증과 구분합니다. 해당 과거 host
실행 파일의 SHA-256은
`2cb9d0ffae4766e9054ac7fce369e3c782a7642f3e7ce689af93e1c217baf40a`입니다.

## 고정 소스 전체 검증과 릴리즈 상태

고정 커밋 `40051f8afffa38443e0db13bef6c592c8d80a854`의 새 빌드로
전체 host와 고정 ARM/QEMU 검사를 각각 종료 0으로 완료했습니다.

| 검사 | 확인 결과 |
| --- | --- |
| 전체 host `make test` | Python 401개(41+124+28+1+10+163+34)와 C/C++ 통과, 생략 0 |
| 고정 ARM/QEMU 전체 실행 | 종료 0, 생략 0; 시작·종료 `ARM_TEST_INPUTS` 두 기록 일치, `release_verified=true` |
| 고정 소스로 별도 빌드한 ARM holdout | 7,258개 synthetic check 통과 |
| 같은 ARM formatter 실행 파일을 소비한 Python 계약 | 51개 통과; NUL 포함 773바이트 / 2,200바이트 버퍼 검사 통과 |
| 실제 제품 ARM DSO의 7개 suite | position 8 / request 14 / request-wire 1 / session 29 / bus 31 / assist 28 / runtime-assist 9개 통과 |

ARM holdout·formatter 검사는 제품 소스를 별도 실행 파일에 링크하여 고정
sysroot와 QEMU로 실행했습니다. 실제 제품 DSO를 적재한 7개 suite와 구분합니다.
이 후속 실행에서 제품 DSO 내부의 holdout formatter를 직접 호출하거나 원본
LDS 서비스를 다시 실행한 것은 아닙니다. 고정 toolchain commit은
`61ec0343de84f6fc7c46840056df1d600d44be8a`이며 GCC 4.9.1입니다.
검사 시작·종료와 ZIP에 포함된 `libmx5dr.so` SHA-256은 다음과 같습니다.

`4e6d51e07e06b68c12262dd678c561285176fd3976f9d2453ec1d7df545f6e5c`

공개 설치 ZIP `mazda-aa-dr-v0.3.8-shadow.1.zip`은 **1,440,626바이트, 36개 항목**입니다.
CRC, 전체 manifest의 35개 파일, 고정 커밋의 소스 파일 87개와 제품 산출물 5개를
대조했습니다. `source_modified=false`, 기본 SHADOW이며 SHA-256은 다음과 같습니다.

`19edb8b72faba813e169543ff6e1697a945954f13f76d5ee1346803b81168686`

동일 최종 ZIP으로 순정 ARM BusyBox/libc를 사용한 설치·기동 보호·회수·제거,
계정/소유권 회귀, 숫자 메뉴의 종료 실패 경로 검사 세 가지를 모두 종료 0으로
완료했습니다. 정상 숫자 메뉴와 함께 종료 확인 실패 뒤 원본 기록·실패 결과의
회수, 분리된 USB에서의 거부도 확인했습니다. 입력 로그와 CMU 환경은 작성한
시험 조건이고 mount 동작은 모의했습니다. OEM 서비스·실차 실행은 포함하지
않습니다. ZIP 검사 기록의 `test_exit=0`, `cleanup_exit=0`은 해당 검사와 그
정리 범위의 완료값입니다.

전체 실행 로그 `release/results/host-all.log`, `arm-all.log` 및 종료값,
`release/candidate-verification.json`, 최종 ZIP 검사의 `checks.json`과
`result.txt`를 보존했습니다. 오래된 workspace 실행 파일을 사용했던 위 실패와
새로 빌드한 전체 통과 결과를 별개로 유지합니다.

[v0.3.8-shadow.1](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.8-shadow.1)을
2026-10-01 11:19:40 UTC에 prerelease로 게시했습니다. 원격 annotated tag는
위 소스를 가리킵니다. 공개 ZIP·체크섬을 다시 내려받아 시험한 후보와 바이트
단위 일치, SHA·CRC·전체 manifest·87개 소스 입력·다섯 제품 해시를 확인했습니다.
[발행 검증](RELEASE_V038_2026-10-01.md)에 게시 기록과 실행 범위를 함께 남깁니다.

주 검증 컨테이너와 그 안의 추가·갱신 패키지 79행, 고정 도구체인 2,124파일,
임시 원본 추출본과 clean checkout을 제거했습니다. ZIP 검사 전용 임시 도구도
제거됐으며, 호스트 패키지·이미지·컨테이너·binfmt 목록과 상세 설정은 작업
시작 시점과 같습니다. 기존 이미지와 사용자 펌웨어 archive는 보존했습니다.
호스트 패키지 설치와 `sudo` 실행은 없었습니다.

호스트 baseline 사본과 실패한 worker-session 증거의 삭제는 자동 승인 검토에서
거부됐습니다. 보존 가치가 있는 실행·검증 자료를 영구 삭제할 수 있고, 설치
도구 정리 승인이 그 삭제까지 명시적으로 포함하지 않는다는 사유였습니다.
삭제를 우회하거나 재시도하지 않고 해당 자료를 보존했습니다. 이 자료는 추가
설치 도구가 아니며 도구 제거 완료와 구분합니다.

live qualified 입력 공급부, 물리 센서·원본 측정 시각·차량 복구·폰과 앱 수용
검증은 여전히 남아 있으며, 이번 결과를 live ASSIST 활성화나 v1.0 완료로
해석하지 않습니다.

## 보존 근거

수정 전·후 소스/실행 파일 해시와 로그는 비공개
`evidence/holdout-reference-20261001/`에 보존했습니다. `controller-record.md`,
`analyzer-verification.md`, worker red/green 로그와 `worker-green-analysis.json`을
구분합니다. 공개 문서에는 OEM 바이너리·역어셈블리·메모리 주소나 실차 로그를
포함하지 않았습니다.

| 기록 | SHA-256 |
| --- | --- |
| 최종 대상 Python 51개 검사 로그 | `e5a80a3b86ba52f709e3bedd5fbee8315bb4975ab6c6936ab6b3ae6b2ee9ce1e` |
| worker 수정 전 실패 로그 | `0d5886c366e0f73d4a53538d82d934454393456b9f0969092b39969859f0cb4e` |
| worker 수정 후 분석 JSON | `987368b22497542e5c6c35d7aabe5828cc0eb93629fd49836f952c7fd08a8cd1` |
