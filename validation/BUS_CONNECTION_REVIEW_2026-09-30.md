# 버스 연결 관측 검토·수정·직접 실행 — 2026-09-30

NA 74.00.324A 전용입니다. 외부 버스 구현을 현재 master에 통합하고,
분석기의 모순 누락과 증분 빌드 의존성, 설치·후보 철회 검사의 공백을
재현·수정했습니다. 최종 제품의 원본 펌웨어 VM에서 실제 LDS 요청 30건의
발행/응답 연결과 실제 daemon 종료의 단절 관측을 직접 검사했습니다.
이 결과는 bus/provider/receiver 자격이나 v1.0 완성의 증거가 아닙니다.
ASSIST는 계속 비활성이며 공개 ZIP은 기존 v0.3.1-shadow.1입니다.

## 출처와 통합 범위

부모 master는 `429b037748916732a5b6b90568ddf7436bca6000`입니다.
외부 `4037b5be0e9856d240016764bcfd8bdf050dc69a`의 버스 변경만 적용하여
`e79ca52`의 MODEL 시각·초기화와 `d0c74d3`의 요청 경로·분석기 보강을
보존했습니다. Makefile, adapter 설명, 요청 journal 검사의 충돌을 직접
정리했고 기존 MODEL reset/input ARM 링크에도 새 버스 의존성을 연결했습니다.

작업 중 외부 tip `c595407e17a1308b509b25c016c15098aa88a52d`도 확인했습니다.
그 브랜치는 위 master 변경과 자원 측정을 통합하고 별도 실행을
[OBSERVATION_SYNC 기록](https://github.com/junoh-moon/mazda-aa-dr/blob/c595407e17a1308b509b25c016c15098aa88a52d/validation/OBSERVATION_SYNC_2026-09-30.md)에
남겼습니다. 외부의 3/9 요청·561/477 raw·generation 30→32 수치와
아래 직접 실행 결과를 합치지 않습니다. 이번 작업에서 Claude CLI를 새로
실행했다고 주장하지 않습니다.

## 재현과 수정

- 버스 연결 수명 ID 하나가 서로 다른 두 객체에 속하거나, 관측 객체가
  생성 수·고정 용량보다 크거나, 누적 생성 수가 줄어드는 로그 네 종류를
  기존 분석기가 `local_checks_pass`로 판정했습니다. 같은 요청의 발행보다
  응답 수명이 작아지는 경우도 단순 inconclusive에 그쳤습니다.
  전역 수명 소유자, 인과관계가 있는 발행→응답, 고정 용량·누적 생성 수를
  검사하여 명시적인 모순은 violation으로 판정합니다.
- snapshot을 읽은 뒤 receipt clock을 취하므로 다른 요청끼리의 기록·시각
  순서는 수명 순서가 아닙니다. 늦은 정상 행, 동률/미상 시각, 이전 health
  뒤에 생성된 객체는 모순으로 만들지 않습니다. 생성 수별 마지막 health
  시각을 보존하여 늦은 행과 더 오래된 health 사이의 모순도 검사합니다.
  boot마다 이력을 초기화하고 malformed 행은 관계 상태에 넣지 않습니다.
- 작성 API에서 signal predicate를 멈춘 사이 free/recreate로 주소를
  재사용하면 새 객체를 잘못 disconnected로 관측했습니다. predicate 전에
  재사용하지 않는 관측 슬롯을 잡고, ENDED를 제외한 CAS로 상태를 바꿉니다.
  수정 전 공개 회귀의 assertion 실패와 수정 후 성공을 보존했습니다.
  원본은 실제 객체와 mutex를 사용하므로 이것을 **원본의 동시 free/dispatch
  재현이나 그 호출 조합의 지원 증거로 세지 않습니다.** 같은 객체 reconnect가
  분류 중 겹치는 경우의 원본 계약도 미검증입니다.
- 버스의 새 host target 두 개에서 `adapter.h`와 요청/세션 헤더 의존성이
  빠졌습니다. 변경해도 `make -q`가 재빌드 불필요를 반환하는 실패와,
  수정 후 세 헤더 × 두 target이 모두 재빌드되는 결과를 확인했습니다.
- 16슬롯 설치 검사는 첫 슬롯의 늦은 변조만 보았습니다. 모든 슬롯을
  하나씩 변조하는 검사로 바꿨습니다. 재검사를 첫 7개로 제한한 변형은
  이전 공개 검사를 통과했으나 보강 후 실패합니다.
- 버스 경계의 진입 또는 복귀 철회를 없애도 기존 공개 검사가 통과했습니다.
  두 경계 각각 create/connect/disconnect/free/close/signal 여섯 사례에서
  실제 adapter 송신 선택을 검사합니다. 원본 진행 중 후보를 별도 worker가
  발행하는 복귀 사례도 포함합니다. 작성 자격을 주입한 fixture에만 ASSIST를
  허용하며, 오래된 후보 제외·원본 1회/48바이트/반환/errno·새 후보 복구를
  확인합니다. 실제 차량 자격을 주입하거나 제품 gate를 완화하지 않았습니다.

## 독립 리뷰와 작성 코드 검사

세 독립 에이전트가 각각 수명/동시성/전달, 요청 복사/JSON/분석기,
설치/빌드/통합을 검토했습니다. 담당 범위의 재현 가능한 P1/P2는 수정 후
남지 않았다고 보고했습니다. 리뷰어는 OEM·실차 검증을 수행하지 않았습니다.

- 수명 리뷰: 공개 bus 29개 GNU 통과. native·ASan/UBSan·TSan 각각 28개
  통과와 Darwin forced-unwind 1개 skip입니다. GNU의 별도 작성 취소 7개와
  실제 송신 후보 경계 12개도 검사했습니다. 진입/복귀 철회 삭제, signal의
  늦은 lookup, ENDED 보호 삭제 변형을 각각 검출했습니다.
- 요청/분석기 리뷰: 관계 112개, schema 2,024개와 기존 MODEL·revision·route
  계약을 검사했습니다. C++ 복사/formatter 변형 6개와 parser 변형 9개를
  검출했습니다. 공개 검사에서 살아남던 이전 health 누락·행 시각 상한 누락·
  이전 capacity 누락 세 변형도 공개 회귀를 보강한 뒤 모두 검출했습니다.
  최대 observation 3,933바이트 행의 일반 worker·종료 배출 일치를 확인했습니다.
- 설치 리뷰: 1~16슬롯, 0/17 거부, 각 슬롯 변조, 권한 실패·이중 복원 실패와
  prepare/할당 실패 등 독립 90개 경계를 검사했습니다. 요청 issue/reply 복사
  삭제 및 reply 객체/수명 고정 변형도 검출했습니다. 마지막 native 33개
  프로세스는 bus 29개, cold, 초기 생성자, 요청 연결 유지/재생성입니다.

최초 host 실행은 stock fixture와 제품 경로 환경 변수를 빠뜨려 패키징 21개가
skip됐습니다. 이를 통과 수에 넣지 않습니다. 기존 원본 rootfs와 검증된 제품
산출물 경로를 지정한 최종 `make test`에서 Python **306개**와 C/C++ 전체를
skip 없이 통과했습니다. 공개 parser 보강 후 journal Python 70개도 별도
통과했습니다. 고정 GCC
4.9.1 ARM 전체 검사도 통과했으며, 실제 제품 DSO의 위치 8개·요청 14개·세션
29개·버스 29개를 포함합니다. 이 두 전체 실행의 skip은 0입니다. 독립 리뷰의
Darwin skip을 이 결과로 바꾸지 않습니다. 관계 112개도 직접 다시 확인했습니다.

원본 파일에서는 hash·ELF·export 위치·17개 JCIDBUS API 진입 바이트,
libdbus predicate와 새 GOT 9개 relocation/권한을 직접 대조했습니다.
실제 설치의 live GOT 값은 아래 제품 cold bootstrap으로 별도 검사했습니다.
원본 코드 페이지를 추가로 패치하거나 firmware guard를 완화하지 않았습니다.

## 최종 제품의 원본 VM 직접 실행

원본 커널·공유 runtime·LDS·AA를 격리 VM의 진단 init으로 실행했습니다.
NIC·호스트 공유 디렉터리·물리 장치 연결은 없습니다. 커널 진입 machine ID
인자만 기존 진단 방식으로 조정했고 원본 kernel/OEM 파일 바이트는 보존했습니다.
순정 전체 SM 기동, 물리 센서 또는 폰 연결을 실행한 시험은 아닙니다.

| 직접 실행 항목 | manager 정지 | manager 재시작 |
| --- | ---: | ---: |
| 원본 LDS 요청 / 세션 재생성을 넘긴 지연 요청 | 14 / 4 | 16 / 4 |
| 지연 요청의 SEND / LOCATION | 0 / 0 | 8 / 4 |
| 새 세션 발행 요청 | 0 | 2 |
| 보존한 합성 raw / 지연 응답 구간 raw | 423 / 24 | 500 / 21 |
| 오래된 요청 MODEL 제외 | 4 | 4 |
| 시간 경계 제외 raw 대조 | 1 | 3 |
| journal / 요청 loss / 세션 fault / 버스 fault | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |

두 조건 모두 AA 세션을 재생성합니다. 차이는 그 뒤 manager 재시작 여부입니다.
실제 BLM의 GOT 호출이 서로 다른 HMI/service 연결을 생성·연결했으며,
발행/응답의 service 객체·수명과 네 route 필드를 보존했습니다. 별도 원본
bus-monitor의 GetPosition 30건과 대조했습니다. 늦은 요청의 이전 세션 문맥과
새 send storage도 구분했습니다. 정리 뒤 두 연결 관측은 모두 unobserved입니다.
raw는 직접 MODEL channel에 넣은 합성 입력이며 물리 VBS 입력이 아닙니다.
계산에서 제외한 네 raw 행도 epoch/순번으로 원본 sensor·receipt·transport를
대조했습니다. 모든 원본 LDS 응답의 mode·UTC·좌표는 0이고 MODEL은 invalid입니다.

별도 프로세스에서는 제품 bootstrap 후 원본 두 모듈의 lifecycle GOT 8곳과
signal GOT를 확인하고 그 경유로 원본 공개 API를 호출했습니다. 실제 daemon
종료와 원본 dispatch 뒤 같은 객체의 관측은 connected→disconnected,
generation은 **18→20**, close callback은 **0회**, bus fault는 **0**이었습니다.
free 뒤에는 unobserved로 전환됐고 원본 파일 hash가 유지됐습니다. 같은 객체·
이름의 reconnect는 원본 반환 0으로 실패했습니다. 원인 해결로 세지 않습니다.

별도 원본 INVALID callback 두 주기, 원본 공유 runtime에서 작성 세션 DSO
29개·버스 DSO 29개도 통과했습니다. 작성 API 대역과 위 원본 JCIDBUS/LDS
실행은 서로 다른 근거입니다. 네 journal 모두 일반 분석은 inconclusive이며
violation은 0입니다. 상세 inconclusive 수는 callback 16, manager 정지 38,
재시작 50, 독립 bus 3입니다. 세션 경계·MODEL source fault·없는 위치 입력을
성공으로 바꾸지 않았습니다.

VM helper는 240초 한계에서 exit 124, QEMU exit 0·timed_out=true입니다.
이 값은 성공 판정이 아닙니다. helper 종료 후 확정된 console에서 caller
완료/exit, 실제 요청·단절, 모든 작성 사례와 최종 health·capture.done을
별도 판정했습니다. 완료·단절·철회·callback·요청 문맥·raw 등을 훼손한
14개 판정기 변형을 모두 거부했습니다. 예비 판정기의 이전 fixture 경로
잔류로 생긴 FileNotFoundError도 보존했고 경로를 정리한 뒤 최종 판정했습니다.
그 오류를 제품 또는 VM 실행 실패로 세지 않습니다.

## 고정 산출물과 남은 범위

| 산출물 | SHA-256 |
| --- | --- |
| 제품 libmx5dr.so | `45295df6809f0a7112cfcd6a05f57419624d7943bb01795ab6f2d47dd05860ab` |
| 비공개 initrd | `ba64c55b1b661f3fcf85b0a5fb38b833bc404e240d911645dcc05a1c8080171d` |
| 최종 console | `7e38996551274b5b872e480cb2124ecc29b4463934017b795ab80f249337cf2a` |
| 최종 판정기 | `78e2a43c000c20ad81e93cbfc7a30771c89fa894a84f590b014863334bb1a672` |

검증한 ARM 입력 56개와 산출물 5개의 해시를 최종 작업 트리와 다시 대조했습니다.
원본 파일·역어셈블·전체 console은 게시하지 않습니다. 명령, 수정 전 실패,
리뷰 보고서와 직접 실행 자료는 ignored
`evidence/bus-connection-review-20260930/`에 보관합니다. 고정 도구체인과
기존 컨테이너를 사용했으며 새 패키지 설치는 하지 않았습니다.

MODEL/holdout의 **버스 경계 reset은 TODO**입니다. daemon GUID/provider
소유자와 요청/session/receiver 자격도 아직 구현·입증하지 못했습니다.
같은 객체의 경쟁 reconnect, 정상 전체 기동·복구, 실제 센서 단위/품질/시각,
유효한 원본 GPS→터널 정확도와 Galaxy S25/무선 AA/네이버 지도 수용은
남아 있습니다. 앞선 반복 VM 정리 실패의 원인도 이번 성공으로 닫지 않습니다.
마지막 fetch에서 외부 `2548312ce1b4b20dc5aff6d69a56aca08c4f0ad1`의 MODEL
버스 reset 변경도 발견했습니다. 다음 검토 대상이며 이 체크포인트에는
아직 통합하거나 해당 외부 실행을 직접 검증한 것으로 세지 않습니다.
