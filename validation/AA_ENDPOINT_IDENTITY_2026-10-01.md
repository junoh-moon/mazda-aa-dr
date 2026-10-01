# AA 요청의 연결 식별자 — 2026-10-01

구현 커밋은 `f2ec7c2bed55080e9700b1509e016b6600f98cf6`입니다.
원본 연결 등록에서 서버 주소 GUID와 client 고유 이름을 복사하고 실제
송신 연결과 요청 수명의 일치를 기록합니다. 같은 요청 serial을 사용하는
여러 client와 새 서버 연결을 구분할 수 있는 진단 정보입니다.
LDS 제품 producer/sideband와 물리 입력 자격은 미구현이며 live ASSIST는
비활성입니다. 이 문서의 원본 실행은 정상 전체 SM 기동이나 차량 시험이 아닙니다.

## 구현한 계약

- 등록이 성공하고 같은 연결 수명의 바깥 connect도 성공했을 때만
  `request.endpoint.server_guid`와 `unique_name`을 소유 사본으로 공개합니다.
  GUID 할당은 원본 해제 함수로 돌려주고 빌린 문자열을 나중에 참조하지 않습니다.
- 이름 조회는 연결 설정 중에만 합니다. 요청·송신 중에 새 조회·할당이나
  동기식 D-Bus 질의를 추가하지 않습니다. 연결 객체의 내부 raw 포인터는
  검증한 원본 등록 호출의 잠금·수명 안에서만 읽습니다.
- 요청 수명과 실제 raw 송신 인자가 일치해야 `endpoint_matched=true`입니다.
  송신 뒤 수명이 바뀌면 이 표시만 철회합니다. 그 변화만으로 잘못된 송신을
  단정하지 않습니다. 실제 raw 불일치나 중복 송신은 conflict로 표시하면서
  첫 관측 serial을 보존합니다.
- 정보 누락·잘린 문자열·조회 실패에도 원시 POSITION 기록은 남습니다.
  재접속은 이미 소유한 과거 요청 사본을 바꾸지 않습니다. 공유 문자열의 각
  byte도 atomic으로 복사하여 version 검사만으로 데이터 경합을 숨기지 않습니다.
- 기존 cold 설치 트랜잭션에 등록 슬롯 하나를 추가합니다. 21개 슬롯의
  원본 chain·펌웨어·모듈·진입점·호출 위치를 먼저 검사하고 wrapper 준비 뒤
  공개합니다. 펌웨어 guard를 완화하지 않았습니다.
- JSON 한 행 버퍼 용량은 5,120에서 6,144바이트가 됩니다. ARM의 고정 256개 관측 큐는
  258,080에서 292,896바이트로 증가합니다. 큐 항목 수와 로그 회전·공간 정책은
  그대로이며 저장량이 계속 증가하는 별도 기록 파일을 추가하지 않습니다.

`server_guid`는 전체 bus의 `GetId`와 다른 transport별 서버 주소 식별자입니다.
실제 원본 실행에서도 두 값은 달랐습니다. [공식 D-Bus API](https://dbus.freedesktop.org/doc/api/html/group__DBusConnection.html)의
`dbus_connection_get_server_id` 계약을 따릅니다. 제품에 GetId 요청은 추가하지
않습니다. fixture의 기존 GetId 호출은 serial 순서를 만드는 작성한 시험 조건입니다.

분석기는 두 문자열이 비어 있지 않고 완전하며, 일치한 실제 송신의 nonzero
serial과 비충돌 상태가 있어야 `exact_request_key_records`로 집계합니다.
이는 식별 정보의 가용성 집계이며 LDS 응답 출처를 연결한 결과가 아닙니다.
`lds_sideband_matching=not_implemented`, `qualification=not_established`를
명시합니다. 새 필드는 선택 사항으로 받아 구형 로그를 지원합니다. 모순된
새 필드를 진단해도 원시 POSITION과 기존 MODEL/holdout 결과를 지우지 않습니다.

## 집중 검사와 수정 전 실패

호스트와 고정 GCC 4.9.1 ARM/QEMU에서 새 endpoint 25개 대조가 통과했습니다.
NULL·빈 값·잘린 문자열, 등록/연결 실패, 잘못된 raw/호출 위치, 중첩·재진입,
취소·예외, 송신 전후 수명 변경, 주소 재사용, 동시 reader를 포함합니다.
이 단계의 ARM 25개는 제품 소스를 연결한 실행입니다. 실제 제품 DSO 검사와
혼동하지 않습니다.

호스트의 기존 bus 31개, 초기 생성자, session/request 두 경우, raw wire,
request-trace 17개 그룹, observer와 journal도 통과했습니다. cold 검사는
21개 슬롯 재검사와 30개 보호 실패 지점·복원 실패를 실행했습니다.
최악의 escaped 기록은 NUL을 포함하여 request 5,309, SEND 5,889,
POSITION 5,684바이트입니다. 정확한 용량·한 byte 부족·canary와 실제 worker의
일반 처리·종료 시 남은 기록 처리에서도 5,120바이트를 넘는 행 보존을 확인했습니다.
formatter 연동 Python 18개, 새 endpoint 분석기 11개, 기존 분석기 34개가
통과했습니다. 이 숫자는 전체 `make test`의 결과가 아닙니다.

수정 전에는 새 endpoint 부재, 첫 conflict와 함께 원시 serial이 사라짐,
송신 중 재접속 뒤 잘못된 일치 표시, 21번째 슬롯 용량 부족을 재현했습니다.
초기 구현은 기존 초기 생성자 검사에서도 실패했습니다. C++11 atomic 배열의
동적 초기화가 먼저 기록된 연결 상태를 지웠으며, 명시적 constexpr byte
초기화로 고친 뒤 같은 검사가 통과했습니다. 초기 fixture의 unwind·namespace·
링크 오류도 제품 결함 재현이나 실제 DSO 성공으로 세지 않습니다.
독립 리뷰의 disconnected snapshot/endpoint 모순도 분석기 회귀로 수정했습니다.

## 실제 원본 연결과 제품 formatter

| 구분 | 소스 | 실제 제품 DSO SHA-256 |
| --- | --- | --- |
| 수정 전 | `9e082f76a3f96da27a9dd3936f93203139482d93` | `fda002045a8bdf474012502a973729e4eb15200d1611ff828f369b4f3519f7ed` |
| 수정 후 | `f2ec7c2bed55080e9700b1509e016b6600f98cf6` | `bb01d42ff6dddc67f18294ee23a14209d6b3c29f6e9b4b108f9b3e4f018f6470` |

새 detached 소스 342개와 빌드 입력 63개를 고정 커밋에 대조하고 다섯 ARM
산출물을 새로 만들었습니다. 제품 formatter의 실제 진입점과 runtime worker의
두 호출 지점을 다시 읽어 두 인자 ABI와 6,144바이트 용량을 확인했습니다.
fixture에는 formatter·observer·adapter 구현을 연결하지 않았습니다.
파일 식별용 SHA helper만 연결하고 실제 제품 함수들을 호출했습니다.

수정 전 원본 client 두 개의 네 요청은 정상 처리됐지만, 제품 POSITION에
endpoint가 없어 별도 checker가 실패했습니다. 수정 후에는 다음을 실제 실행했습니다.

1. 순정 공유 runtime 아래 원본 LDS parser·callback·cache·service 응답과
   원본 JCIDBUS 연결/data-client를 사용했습니다. 서버 관측기는 껐습니다.
   NMEA 입력, 초기화와 worker 소비는 작성한 fixture입니다.
2. 실제 제품 cold 설치의 21개 슬롯과 네 code entry를 확인했습니다.
   원본 callback 값으로 POSITION 아홉 필드를 채우고 실제 제품 token 소비,
   POSITION 캡처와 formatter를 호출했습니다.
3. 같은 daemon의 두 client가 같은 요청 serial을 사용하는 네 요청을
   대조했습니다. 새 daemon에서도 같은 client 이름·serial을 재사용하여 네
   요청을 더 실행했습니다. 두 실행의 제품·fixture·서버·소스는 같았습니다.
4. 별도 checker가 해시로 고정한 실제 stdout, 독립 원본 연결 getter와
   monitor의 request/reply 헤더를 대조했습니다. 각 실행의 네 키는 구별됐고,
   새 daemon의 GUID로 두 실행도 구별됐습니다. 아홉 원시 필드가 보존됐으며
   두 실행과 두 checker 모두 종료 0입니다.

총 여덟 요청은 실제 원본 라이브러리와 새 제품 DSO의 실행입니다. 실제 OEM
WorkerQueue 대신 작성한 즉시 WorkerScope를 사용했고 runtime journal worker
스레드, 전체 ServiceInit/SM, 물리 센서와 차량·폰은 이 대조에서 실행하지 않았습니다.
GUID·요청 식별자·관측 시각은 물리 측정 시각이나 ASSIST 자격을 뜻하지 않습니다.

## 통합 검사 상태

단일 USB 포트에서 AA와 쉘을 번갈아 사용하는 후속 절차를 통합했습니다.
최종 `3346854`의 host Python 421개·C/C++와 고정 ARM 전체, 실제 DSO 8개
suite·145개 사례를 생략 없이 통과했습니다. 첫 최종 후보의 GCC 4.9 fixture
초기화 실패와 수정 후 재검사, 최종 ZIP의 순정 BusyBox 설치·단일 USB 회수,
공개 파일 재다운로드와 도구 제거는 [v0.3.9 발행 검증](RELEASE_V039_2026-10-01.md)에
별도로 기록했습니다. 최종 빌드 입력 63개·다섯 산출물은 위 선행 `f2ec7c2`와
동일하지만 원본 여덟 요청을 최종 pin에서 다시 실행한 것으로 세지 않습니다.
