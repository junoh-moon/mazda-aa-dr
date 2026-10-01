# 원본 reader부터 LDS 제품·AA worker까지 — 2026-10-02

고정 소스 `0f9ffddf5ffc60d5b825de79bc12b3bbc458093d`의 실제 제품을
사용하여, PTY에 넣은 문장이 원본 reader·parser·등록 callback·cache와
서비스 응답을 거쳐 AA worker의 요청 연결까지 도달하는 것을 확인했습니다.
이전 [제품 연결 시험](LDS_REQUEST_SOURCE_2026-10-02.md)에서 작성 코드가
직접 호출하던 parser와 callback을 이번에는 원본 reader가 실행합니다.
물리 센서 입력이나 계산 위치의 AA 송신 시험은 아닙니다.

## 실제 실행 경로

새 ARM 빌드의 제품 preload와 원본 loader·공유 runtime을 QEMU user에서
실행했습니다. LDS 자동 설치의 25개 슬롯과 실제 등록된 callback 열 개를
대조했습니다. 작성 서버·client는 제품 runtime·설치기·요청 연결 구현을
다시 링크하지 않으며, 원본 LDS parser와 callback을 직접 호출하지 않습니다.

원본 `Open`이 만든 reader가 PTY를 읽고 GSA→GGA→RMC, 분할한 다음 GGA,
다음 RMC, 무효 GGA/RMC 순서의 일곱 문장을 처리했습니다. 분할 문장의
앞부분만 넣었을 때와 나머지 바이트를 넣은 뒤의 cache 효과를 구분합니다.
부분 갱신에서 다른 필드의 기존 할당 출처가 계승되고, 새 쓰기가 없는
재조회는 같은 할당 번호를 유지했습니다. 원본 `Close`가 반환하고 입력 fd가
닫힌 뒤 서버와 client 모두 종료 0을 확인했습니다.

입력 문장과 기동 순서, 선택한 원본 초기화, AA cold lease 및 즉시 실행한
WorkerScope는 작성한 조건입니다. 정상 전체 ServiceInit·SM 기동이나 실제
BLM 작업 스케줄을 재현한 것으로 세지 않습니다. 작성한 AA client callback은
원본 응답의 scalar를 72바이트 위치 구조에 복사하고 실제 제품의 WorkerScope와
position-enter/leave를 호출합니다. 원본 BLM 위치 callback·작업 큐·송신은
이 fixture에서 실행하지 않았습니다. `/sys`는 읽기 전용이고
차량 GPS/UART/I2C 장치는 없었습니다. 보존한 syscall trace에 GPIO·차량
장치 접근은 없지만, 이것만으로 모든 내부 helper의 미실행을 증명하지는 않습니다.

## 결과와 독립 대조

- 원본 질의 9건, 제품 POSITION 9건, LDS sideband 9건을 연결했습니다.
- 서버 주소 GUID·client 이름·요청 serial·server 이름·응답 serial·reply
  serial의 여섯 식별 필드를 먼저 대조한 뒤 각 응답의 숫자 아홉 개,
  총 81개를 별도 D-Bus monitor 출력과 비교했습니다. 모두 일치했습니다.
  출력 숫자의 비교이며 원본 wire 전체의 바이트 동일성 검사는 아닙니다.
- 실제 AA worker 연결기는 match 9, conflict 0, reject 0을 기록했습니다.
  종료 후 보관 항목은 0입니다. 이번 실행은 해당 counter를 검사했으며
  `JoinedReply`를 직접 읽는 별도 fixture를 다시 실행하지는 않았습니다.
- 기존 독립 검증기를 수정 없이 적용했습니다. 식별자 네 종류, 중복과
  payload 변조 여섯 조건 및 연결기 상태 변조 일곱 조건을 모두 거부했습니다.
- 실행 전후 원본·제품·작성 입력과 clean checkout이 일치했습니다.
  소유한 프로세스, 임시 alias와 bus socket을 정리했습니다. monitor의
  SIGTERM 종료는 runner의 명시적 정리이며 서버·client 실패가 아닙니다.

전체 분석기의 결과는 종료 2·`inconclusive`입니다. 작성한 기동·WorkerScope와
LOCATION 송신 부재를 완성된 제품 세션으로 취급하지 않았습니다. 원시
provenance와 물리 readiness도 계속 미확정입니다.

| 실행 제품 | SHA-256 |
| --- | --- |
| AA | `fcc01b5a6357d6e09802478411ca178fedb3c59015cd9353b8c16f9abf85c6fd` |
| LDS | `29a2cbeb7a9f29eb83a9e6ac05a91ec3d0b73fad1dea579e016db5710b79b73f` |

비공개 실행 기록은 `evidence/lds-assist-source-20261002/driver-product/`의
`original-0f9-r1`, 독립 대조와 cleanup 기록에 보존했습니다. 앞선 단독
reader의 초기 실패들은 [별도 기록](LDS_DRIVER_READ_2026-10-02.md)에 남깁니다.
이 결합 실행에서는 추가 도구를 설치하지 않았으며 공용 검증 컨테이너는
후속 검사에 계속 사용합니다. 공개 릴리즈 ZIP은 변경하지 않았습니다.

## 실제 ASSIST까지 남은 연결

현재 제품은 cache 할당 출처를 보존하지만 각 할당에 기여한 실제 read의
바이트 범위·parser 호출 소유권은 아직 공급하지 않습니다. 물리 측정
시각·수신기·단위·품질·보정도 이 관측 번호로 대체할 수 없습니다.

또한 sideband는 원본 응답 송신과 Path 반환 뒤 발행합니다. AA callback이
그보다 먼저 실행될 수 있으므로 이미 끝난 요청의 출처를 사후 승인하거나
직전 요청의 연결 결과를 다음 요청에 사용할 수 없습니다. 현재 요청 문맥을
inline으로 전달하는 [후속 수정](PROVENANCE_CONTEXT_2026-10-02.md)과 이
송신 전 증거 공급은 별개입니다. 실제 입력 공급부·계산 위치 선택·GPS 복귀와
폰 반영까지 이어지는 v1.0 경로를 계속 구현해야 합니다.
