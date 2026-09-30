# 발행 당시 LDS 요청 경로 보존 — 2026-09-30

NA 74.00.324A 전용이며 `feat/session-observation`의 `a15e114` 이후 변경입니다.
같은 작업 브랜치를 유지합니다. 기존 실행 기록의 횟수와 결과는 변경하지 않습니다.

## 문제와 변경

기존 제품은 요청→응답→worker→POSITION/SEND를 연결했지만 요청이 향한
목적지·객체 경로·인터페이스·메서드명을 보존하지 않았습니다. callback 시점의
최신 서비스 조회나 응답 송신자만으로 발행 당시 요청의 경로를 대신할 수 없습니다.

- 원본 `JCIDBUS_method_get_destination/get_path/get_interface/get_name`의 ARM
  함수 계약을 확인하고 기존 파일 SHA-256·매핑·주소·시작 바이트 검사에 추가했습니다.
  네 getter가 모두 준비되기 전에는 제품 후크를 게시하지 않습니다.
- 비동기 전송 **전** 살아 있는 method에서 네 문자열을 각각 복사합니다.
  원본 전송 후에는 callback이 이미 method를 해제했을 수 있으므로 읽지 않습니다.
  기존 요청 ledger와 worker 전달은 빌린 포인터 없이 복사한 값만 보유합니다.
- 각 문자열은 63바이트까지 보존합니다. NULL은 unknown, 초과 길이는
  `complete=false`이며 잘린 접두어는 식별 근거가 아닙니다. 알려지지 않은 값의
  남은 바이트와 NUL 없는 작성 입력도 정규화합니다.
- `request.route`를 POSITION/SEND에 기록합니다. 관측 실패 때는 이전 route를
  기록하지 않습니다. 분석기는 잘못된 형식과 실패 결과에 남은 route를 거부하고,
  이 필드가 없는 과거 로그도 읽습니다.
- request JSON 버퍼는 4096, 일반·종료 배출의 observation 버퍼는 5120바이트입니다.
  긴 여섯 문자열을 escape한 기록도 종료 배출에서 저장하도록 수정했습니다.

well-known 목적지 이름은 실제 제공자 identity가 아닙니다. bus lifetime, 실제 LDS
제공자 인증, 요청/세션 소유권과 해당 위치 snapshot의 receiver 자격 연결은
**미구현**입니다. live `provenance()`의 false와 `allow_assist=false`를 유지합니다.

## 작성 코드 검사

| 범위 | 결과와 한계 |
| --- | --- |
| `make test` | C/C++ 및 파이썬 295개 검사 실행. 설치 payload 두 건은 fixture 경로 오류로 skip했으며, 현재 ARM 빌드 경로를 지정한 별도 두 검사에서 통과했습니다. 미해결 skip은 없습니다. |
| 요청 관측 | 두 요청의 응답을 역순으로 처리하고, 발행 뒤 원본 문자열·포인터를 변경해도 이전 route가 유지됩니다. NULL·긴 문자열·errno와 qualified 필드의 unknown을 검사했습니다. |
| JSON·종료 배출 | 잘못된 route 다섯 종류가 수정 전 분석기를 통과하는 실패를 확인했습니다. 종료 배출의 2200바이트 버퍼도 긴 기록에서 실패를 재현한 뒤 수정했습니다. 최대 정수·escape, 정확한 버퍼 크기와 한 바이트 부족 검사를 통과했습니다. |
| 고정 ARM 전체 검사 | GCC 4.9.1·ARMv7 softfp로 현재 다섯 바이너리 빌드 및 전체 검사 통과. 제품 DSO의 position 8·request 14·session 22개 포함입니다. |
| 원본 공유 runtime | 같은 제품 DSO의 작성 요청 후크 14개 검사 통과. 원본 LDS 서비스 실행과는 별도이며 차량 검사가 아닙니다. |

처음 호스트 parser 검사에는 기존 오래된 `build/test_journal`도 사용되어
`session_context`가 없는 오류 한 건이 함께 발생했습니다. 이후 격리 checkout에서
현재 fixture를 다시 빌드했고 전체 검사를 통과했습니다. 종료 배출의 red 실패와
별개로 보존한 이 초기 오류를 제품 회귀로 계산하지 않습니다.

## 원본 실행 및 증거

원본 커널·LDS·AA를 NIC·호스트 공유 장치 없는 격리 VM에서 실행했습니다.
전용 진단 init과 작성 caller를 사용하므로 순정 전체 SM 기동 검사는 아닙니다.
폰·물리 센서는 사용하지 않았습니다. 실제 LDS 응답의 mode·UTC·좌표는 0입니다.
제품 시나리오의 motion과 직접 GPS seed는 명시적인 합성 입력입니다.

원본 manager의 요청 네 건에서 목적지·경로·인터페이스·메서드명이 발행과 callback
양쪽 모두 일치했습니다. 원본 공개 `method_get_serial`은 양쪽 모두 0이었습니다.
이 표본에서 wire serial을 얻은 것으로 세지 않으며 제품 getter에 추가하지 않았습니다.

제품을 preload한 실행에서는 세션 부재 시나리오의 원본 요청 5건과 재생성
시나리오의 9건에서 모두 정확한 route가 기록됐습니다. 각각 이전 요청 한 건을
세션 파괴 이후까지 지연했고, POSITION과 연결된 SEND의 전체 요청 기록이
일치했습니다. 송신 본문은 원본과 바이트 단위로 같았습니다.

| 제품 실행 | 세션 부재 | 세션 재생성 |
| --- | ---: | ---: |
| 원본 요청 / 지연 요청 | 5 / 1 | 9 / 1 |
| 지연 요청에서 발생한 SEND / LOCATION | 0 / 0 | 2 / 1 |
| LOCATION 반환 / 송신 세션 lifetime | 해당 없음 | 0 / 2 |
| 합성 raw 수집 / 지연 구간 수집 | 543 / 195 | 522 / 168 |
| journal loss / request loss / session fault | 0 / 0 / 0 | 0 / 0 / 0 |

이전 요청의 issue lifetime은 1로 유지됐습니다. MODEL은 각각
`session_unavailable`, `session_changed_since_issue`로 늦은 위치를 제외했습니다.
직접 넣은 GPS seed도 실제 요청 관측이 없어 제외됐으며 두 실행의 모든 MODEL
출력은 invalid입니다. 이를 유효한 터널 위치 계산·정확도나 폰 수용으로 세지 않습니다.
일반 로그 분석기는 두 실행 모두 inconclusive(exit 2)를 반환했습니다.

별도 실제 상태 callback 두 주기에서 INVALID·stop 264와 generation
4→6→8, 10→12→14를 확인했습니다. QEMU는 완료 marker와 capture 종료 후
175초 제한으로 정리됐습니다(helper exit 124, `timed_out=true`). 성공 판정은
위의 전용 assertion과 원본 호출·저장 로그에 근거하며 QEMU 종료 코드가 아닙니다.

| 고정 대상 | SHA-256 |
| --- | --- |
| 제품 `libmx5dr.so` | `ece566fbe8aad7d40ff03cc8bbec663199a72336e8d227560c137c77dd9ab67b` |
| 비공개 진단 initrd | `466c8888abd95d8d21df19ef0f5e467902a776b7e6453d2c2b633d3045e0adfd` |
| 최종 console | `a11e8e9cc757a26b66db81f77ff95104e8aa856ed522378621e1be5939a13f60` |

원본 바이너리·주소 자료·전체 console과 실행 스크립트는 ignored
`evidence/provider-request-20260930-a15e114/`에만 보관합니다. ARM 빌드의 53개
입력 파일 해시를 현재 소스와 대조했습니다. 공개 릴리즈·master는 변경하지 않습니다.

## 도구 정리

추가 도구는 전용 `mazda-provider-request` 컨테이너 안에만 설치했습니다.
QEMU user/system, 빌드·DBus 개발 도구 등 추가 패키지 120개와 기존 패키지
업데이트 6개를 목록으로 보관했습니다. 고정 도구체인은 manifest 2124개 항목입니다.
검사 후 컨테이너와 도구체인, `/tmp/mazda-provider-lifetime.I6mmA6`를 모두
제거했습니다. 호스트 패키지 목록은 설치 전후 같으며 기존 Docker 이미지는
보존했습니다. 비공개 증거에는 실행 결과와 작성 산출물을 남겼습니다.
