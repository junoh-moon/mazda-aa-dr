# 요청 경로 복사·저장의 독립 통합 검증 — 2026-09-30

NA 74.00.324A 전용입니다. master `e79ca52`의 세션/MODEL 수정을 유지하면서
독립 구현 `91f2c93`의 요청 경로 복사와 `11c115e`의 통합 회귀를 검토·반영했습니다.
외부 보고서의 실행을 이번 직접 실행으로 세지 않았습니다. 공개 ZIP은 기존
v0.3.1-shadow.1이며 live ASSIST는 계속 비활성입니다.

## 변경과 재현한 문제

원본 비동기 submit 전에 destination·path·interface·member를 복사합니다.
요청·응답·worker·POSITION/SEND에 소유한 값으로 전달하므로 원본 method가
변경되거나 해제된 뒤 그 문자열을 다시 읽지 않습니다. NULL은 unknown,
63바이트보다 긴 문자열은 incomplete prefix로 보존합니다. route 이름과
관측 세션은 실제 provider·bus 수명·receiver 소유권의 자격이 아닙니다.

네 getter는 기존 파일 해시·symbol·주소·실행 segment·entry bytes 검증에
포함하며 모두 준비된 경우에만 제품 hook을 공개합니다. 주 에이전트는 정확한
원본 라이브러리의 네 getter가 NULL을 처리하고 borrowed pointer를 직접 반환하는
구현임을 정적으로 확인했습니다. 원본 코드나 전체 disassembly는 게시하지 않습니다.

여섯 문자열의 escape로 커지는 JSON을 위해 request/observation 버퍼를 각각
4096/5120바이트로 늘렸습니다. 일반 worker와 종료 배출 양쪽에 적용했습니다.
분석기는 새 route 형식을 검사하면서 route가 없던 과거 기록도 받아들입니다.

| 재현한 결함 또는 검사 공백 | 수정과 후속 확인 |
| --- | --- |
| 파서가 실제 byte encoder에서 나올 수 없는 NUL·U+0100·surrogate·emoji를 정상 기록으로 판정 | 여섯 text 필드의 30개 반례가 수정 전 실패했습니다. 독립 전체 로그에서도 30개가 잘못된 `local_checks_pass`였습니다. 공통 byte 범위를 검사하여 모두 `request_record_malformed`/inconclusive로 바꿨습니다. 과거 encoder와의 호환성도 확인했습니다. |
| destination/interface가 같은 fixture 때문에 getter 혼선 mutation을 놓침 | 공개 observer·session_request·ARM fixture에서 두 값을 구분했습니다. 동일 mutation은 공개 observer와 실제 submit→send 회귀에서 assertion 실패합니다. |
| 일반 worker의 버퍼만 2200바이트로 되돌려도 기존 공개 검사가 통과 | 종료 요청 전에 실제 worker가 긴 행 전체를 저장했는지 검사합니다. 정상 GNU/ARM은 통과하고 같은 mutation은 새 공개 회귀에서 실패합니다. 종료 tail도 행 전체를 대조합니다. |
| 필수 getter 준비 검사가 일부 필드에 한정 | 네 getter 각각의 누락·미공개와 후속 정상 준비를 공개 ARM fixture에 추가하고 실제 제품 DSO에서 실행했습니다. |

NUL 없는 소유 Text를 직접 formatter에 넣으면 64바이트 incomplete가 나올 수
있으므로 이 방어적 출력은 계속 허용합니다. 일반 copy/ledger 경로는 최대
63바이트입니다. incomplete를 완전한 식별자로 승격하지 않습니다.

## 독립 리뷰

작성 코드만 받은 세 리뷰어가 수명/forwarding, JSON/분석기, getter/queue 통합을
각각 검토했습니다. 해결 후 검토 범위에서 추가 확정 P1/P2는 없었습니다.
이들의 결과는 OEM·ARM·실차 실행 증거로 세지 않습니다. 이전 작업에서 확인한
도구의 agent thread 제한이 있어 이번 기록도 네 리뷰라고 주장하지 않습니다.

| 검토 | 직접 실행한 독립 근거 |
| --- | --- |
| 수명/forwarding | 공개 trace·observer·session_request를 native·ASan/UBSan·TSan에서 실행했습니다. 별도 11개 fixture도 각 환경에서, deferred cancellation을 포함한 13개는 GNU에서 통과했습니다. 보호 page 경계, 원본 submit 뒤 문자열 접근 차단, 역순 응답과 해제 후 소유 복사를 검사했습니다. |
| JSON/분석기 | C++ 출력 397개/문자열 2,382개, route schema 377개를 독립 대조했습니다. 최대 request/observation 필요 용량은 NUL 포함 3,165/3,746바이트였습니다. 실제 일반 worker와 tail의 3,745바이트 행이 일치했습니다. C++ 6개·parser 6개 mutation과 기존 MODEL/revision/시각 회귀도 검사했습니다. |
| getter/queue 통합 | 독립 17개 시나리오와 공개 회귀를 실행했습니다. 1024개 Observation의 소유 복사·256-slot 재사용은 정상 및 ASan/UBSan에서 통과했습니다. 준비 조건 제거 mutation도 검출했습니다. |

합성 getter가 throw/cancel되면 원본 submit 전이므로 submit 0회/ledger empty이며
이후 정상 요청이 복구됩니다. 원본 submit이 throw/cancel되는 경우와 구별하며,
이를 getter 오류에서도 원본 submit이 실행된다는 보장으로 표현하지 않습니다.

| 최종 리뷰 기록 | SHA-256 |
| --- | --- |
| 수명/forwarding | `68b012110651ead0b956fa2f95307fb4412215c971de8f0b83531af0f33ace0f` |
| JSON/분석기 | `2a7ca12a9292560aae549223e30f09676e0c2b29563a502dc49e6724173cad1d` |
| getter/queue 통합 | `66e9c071b5f21d00a775107706743a883dd0fc3389c89c61f20834732489a072` |

## 주 에이전트의 host·ARM 검사

| 실행 | 결과와 범위 |
| --- | --- |
| GNU `make test` | Python 301개와 C/C++ 전체 검사 통과, skip 0. 원본 BusyBox와 이번 ARM bundle을 명시했습니다. |
| 고정 ARM 전체 | GCC 4.9.1·ARM32 softfp의 최종 제품으로 전체 통과, skip 0. 실제 DSO position 8·request 14·session 29개를 포함합니다. |
| 후속 parser/getter 보강 | journal Python 66개·tools 30개, 구별되는 getter의 observer/session 회귀와 ARM adapter 회귀를 재실행했습니다. 마지막 네 getter 누락 loop를 포함한 실제 DSO request 14개도 다시 통과했습니다. |
| 마지막 일반 worker 보강 | 변경한 공개 journal C++ 회귀를 GNU와 고정 ARM에서 다시 빌드·실행하여 통과했습니다. 독립 리뷰어는 같은 공개 검사로 worker 2200바이트 mutation의 실패도 확인했습니다. |
| 제품 입력 | `build/arm-request-route-r1/arm-build.json`의 53개 입력과 최종 소스를 대조했습니다. 후속 변경은 PC 분석기·회귀 보강이며 제품 runtime은 전체 ARM 검사/VM 이후 바뀌지 않았습니다. |

전체 host 실행은 마지막 parser/test 보강 전의 301개입니다. 후속 검사를 합쳐
새로운 전체 실행 수치로 기록하지 않습니다. 이번 단위에서 새 USB ZIP의
설치·재다운로드나 차량·폰 검사는 실행하지 않았습니다. 새 도구도 설치하지 않았습니다.

## 원본 펌웨어 직접 실행

원본 커널·공유 runtime·LDS·AA를 NIC·호스트 장치·공유 디렉터리가 없는 VM에서
실행했습니다. 진단 init/caller와 커널 진입 machine ID 인자 조정을 사용했으며
원본 kernel/OEM 바이너리 바이트는 바꾸지 않았습니다. userspace debugger나
LDS 내부 객체 추적을 사용하지 않았습니다. 순정 전체 SM 기동 시험은 아닙니다.

최종 제품을 SHADOW로 자동 설치하여 원본 manager의 LDS 요청부터 worker/send까지
실행했습니다. 원본 AA용 util 요청 네 건의 dispatch를 지연하고 세션을 실제
파괴·재생성한 뒤 응답을 전달했습니다. 별도 두 프로세스의 차이는 manager를
정지 상태로 두는지, 재시작하는지입니다. 두 조건 모두 새 세션이 있습니다.

| 원본 실행 | manager 정지 유지 | manager 재시작 |
| --- | ---: | ---: |
| 원본 LDS 요청 / 이전 세션 지연 요청 | 14 / 4 | 16 / 4 |
| 지연 요청의 SEND / LOCATION | 0 / 0 | 8 / 4 |
| 지연 LOCATION 반환 / send lifetime | 해당 없음 | 0 / 2 |
| 새 세션 요청 | 0 | 2 |
| 합성 raw / 지연 응답 구간 raw | 350 / 28 | 441 / 26 |
| 이전 요청의 MODEL 제외 | 4 | 4 |
| journal drop / request loss / session fault | 0 / 0 / 0 | 0 / 0 / 0 |

버스에서 관찰한 실제 GetPosition 호출 30건과 요청 시 복사한 네 route 값을
대조했습니다. 지연 요청은 issue lifetime 1/revision 1을 유지했고 재시작 조건의
send는 lifetime 2/revision 3이었습니다. 이전 요청 네 건씩을
`session_changed_since_issue`로 MODEL에서 제외하면서 원본 LOCATION 본문과
반환은 보존했습니다. 합성 GPS seed 각 한 건도 `request_unobserved`로 제외됐습니다.

raw는 작성 sender가 직접 MODEL socket에 넣은 wheel/yaw/reverse 입력입니다.
수락된 수와 모든 필드·순번을 저장 로그와 대조했지만 물리 VBS callback이나
센서 시각 증거가 아닙니다. 원본 LDS의 mode·UTC·좌표는 모두 0이며 모든 MODEL은
invalid입니다. 유효 GPS→터널 계산·정확도 시험으로 세지 않습니다.

별도 원본 INVALID callback 두 주기도 확인했습니다. start 0·stop 264,
generation 2→4→6→8 및 8→10→12→14와 revision 0부터 6까지를 확인했습니다.
작성 session DSO 회귀 29개도 같은 제품과 원본 공유 runtime에서 통과했습니다.
정상 폰 연결 callback을 관찰한 것은 아닙니다.

세 capture의 종료 marker·마지막 health·동일 boot ID의 durable ack와 caller
exit 0을 확인했습니다. VM helper는 220초 제한에서 exit 124, QEMU exit 0,
`timed_out=true`였으며 이를 성공 판정 근거로 사용하지 않았습니다. 별도 판정기는
종료 누락·callback 미완료·재시작 근거 누락·이전 요청 제외 변경·가짜 valid
MODEL·raw 누락·route 변경·issue 문맥 변경의 여덟 mutation을 검출했습니다.

일반 분석기는 세 로그 모두 inconclusive(exit 2), violation 0입니다. 세션 reset·
입력 제외와 합성 source fault가 남아 있습니다. 정지/재시작 조건의 MODEL reset
최대값도 각각 14/14이며 이를 센서 연속성 합격으로 해석하지 않습니다.

| 고정 산출물 | SHA-256 |
| --- | --- |
| 제품 `libmx5dr.so` | `b276b58b02282a359d9c37552f146848199483b3676355899cafb2bacf23da27` |
| 비공개 initrd | `01427b6d6474e444a7c86f8a93711413717182e620fdde2a1a50511dc4517208` |
| 최종 console | `5903a6a5c259e2ad826789f7e39f94840ce462b7f292e3266d9172002cdcb3c9` |
| 전용 판정기 | `55a1734de5ac947d83b9d8021b2bb5f90c54a9b3edb4108186558546ba516539` |

명령·실패·최종 실행·리뷰는 ignored `evidence/request-route-review-20260930/`에
보존합니다. OEM 원본·주소 자료·전체 console·개인 자료는 공개하지 않습니다.

## 외부 조사 추적과 남은 범위

SSH fetch로 확인한 외부 최신 tip은 `11c115e3ad8bede234dcfd743c1e84e3e5f443fa`입니다.
[외부 route 조사](https://github.com/junoh-moon/mazda-aa-dr/blob/91f2c9324448b8ff5b1aa4e0514d205784368d88/validation/REQUEST_ROUTE_2026-09-30.md)와
[외부 통합 조사](https://github.com/junoh-moon/mazda-aa-dr/blob/11c115e3ad8bede234dcfd743c1e84e3e5f443fa/validation/SESSION_MERGE_2026-09-30.md)를
읽고 위 변경을 따로 검토했습니다. 외부 VM 실행은 그 작성자의 결과이며,
이번 단위에서 Claude CLI를 별도로 실행하지 않았습니다.

이전 [세션 반복 VM 정리 실패](SESSION_CONTEXT_REVIEW_2026-09-30.md)의 정확한
원인은 여전히 미분리입니다. 이 성공으로 과거 실패를 닫지 않습니다. 확대된
Observation/ledger의 ARM·CMU 실제 메모리 여유와 callback 지연도 검증하지 않았습니다.
bus 수명·실제 provider/receiver 소유권, 물리 센서 단위·품질·시각과 위치 정확도,
폰/앱 수용, 정상 전체 SM 기동·물리 복구가 남습니다. 이번 기록은 v1.0 또는
실차 승인 기록이 아니며 추가 차량·폰 시험을 요청하지 않았습니다.
