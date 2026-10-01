# 실제 제품 계산기와 원본 AA 송신 — 2026-10-02

고정 소스 `8d5669c3cd06180dfcca52758c976c02d7b51b23`의 실제 제품이 계산한
위치를 원본 BLM의 LOCATION 송신에 적용하고, GPS 복귀 뒤 원본 위치로 돌아오는
실행을 완료했습니다. 작성한 센서·자격 입력을 사용한 펌웨어 실행입니다.
물리 정확도, 휴대폰·네이버 지도 반영이나 배포 ASSIST 활성화를 뜻하지 않습니다.

이 기록은 [원본 세션 기동 검사](AA_PRODUCT_STARTUP_2026-10-02.md)의 후속입니다.
기동만 실행한 과거 결과에 위치 계산·송신 실적을 소급해 더하지 않습니다.

## 실행한 경로

작성한 PTY 문장을 원본 LDS reader·parser·등록 callback·cache에 넣고,
원본 LDS 응답 → 원본 BLM callback·WorkerQueue → 실제 제품 POSITION 관측 →
실제 제품 AssistWorker 계산·발행 → 후속 원본 BLM 위치 callback → 실제 제품
위치 선택 → 원본 `aap_send_vehicle_data`와 SysV IPC를 연결했습니다.

제품 코어를 시험 실행 파일에 소스 링크하거나 계산 결과를 직접 발행하지 않았습니다.
원본 parser·위치 callback·WorkerScope를 수동 호출하거나 원본 sender의 성공을
대체하지 않았습니다. AA cold 설치의 시작 순서·lease와 부분 초기화, 정규화한
센서 입력, source readiness와 inline 자격은 비공개 시험이 작성했습니다.
배포 bootstrap의 `allow_assist=false`와 미구현 물리 자격 공급부는 그대로입니다.

첫 원본 GPS callback의 좌표·방향·UTC를 작성한 기준점에 결합했습니다.
callback 수신 시각을 기준점의 모델 시각으로 사용했고, 속도 10 m/s·yaw 0.1 rad/s·
전진 및 100 ms 센서 창을 작성했습니다. 이는 실제 생산자 측정 시각·품질·보정의
증거가 아닙니다. 실제 callback의 값·요청 token·호출 순번·generation은 바꾸지
않았으며, 모델 GPS 기준점과 같은 callback인지 별도로 검사했습니다.

## 완료한 실행과 범위

| 비공개 실행 | 실제 결과 | 판정 범위 |
| --- | --- | --- |
| `original-qualified-gdb-r4` | POSITION/LOCATION 7쌍, GPS 3건·mode 0 4건, DR 대체 3건, 제품 후보 발행 76회, GPS 복귀 후 원본 전달 | 작성한 자격에서 실제 제품 계산·선택·원본 송신 연결 |
| `original-denied-r1` | 7쌍, readiness 159회 모두 거부, 입력 소비·후보·대체 0회 | source readiness 거부와 원본 전달 유지 |
| `original-model-r1` | 7쌍, 입력 227개 소비, 그중 MODEL 센서 216개 pop, `input_fault`, 후보·대체 0회 | MODEL을 실제 worker에 공급한 거부 대조 |

세 실행은 각각 따로 수행했습니다. 각 7쌍은 같은 callback의 정확한 요청·generation과
한 번의 LOCATION 송신을 대조한 개수입니다. 비대체 LOCATION은 48바이트가
그대로이고 원본 로컬 반환은 0입니다. 비위치 메시지 전체의 바이트 대조로
일반화하지 않습니다.

qualified의 발행 76회에는 같은 frontier를 다시 발행한 경우가 있습니다.
서로 다른 계산 frontier는 41개이며, 독립 WGS84 적분·UTC·속도·방향·입력 기한을
발행 76회에 대조했습니다. 실제 DR 송신 3건의 48바이트는 독립 인코딩과
대조했습니다. 최대 좌표 차이는 약 `3.24e-8 m`입니다.
작성한 입력 모델끼리의 수치 일치이며 실차 위치 정확도가 아닙니다.
`sampled_ns`는 나중에 제품 상태를 읽은 시각으로, 발행 시각으로 바꾸지 않았습니다.

denied는 후보가 준비된 뒤 inline provenance를 거부한 시험이 아닙니다.
실제 worker의 readiness가 입력 pop 전에 거부됐으며, mode 0 송신의 사유는
`EPOCH_MISMATCH`입니다. MODEL에서는 source pop과 worker 입력 수를 함께
확인했습니다. MODEL 행이 대기열에만 있었던 것을 거부 실행으로 세지 않았습니다.

원본 client와 LDS reader/server는 각각 종료 0, 원본 세션 해제·요청 배출·큐 join,
제품 `capture.done`을 확인했습니다. 대기 중인 AA 서비스와 monitor는 실행 관리자가
SIGTERM으로 종료했으므로 전체 OEM 서비스의 자연 종료로 표현하지 않습니다.
각 실행의 프로세스 그룹·socket·alias와 소유한 IPC 큐를 정리했고 IPC 전후는 비었습니다.

qualified와 denied 원문에서 사후 요청·LDS 응답은 각각 7쌍으로 연결됩니다.
같은 실행의 live LDS resolver는 각각 matches 6·rejected 2·retirements 2입니다.
사후 7쌍과 live 성공 7회를 혼동하지 않습니다. 이 시험의 자격 입력은 작성한
공급부이며 resolver의 결과를 물리 자격으로 사용하지 않았습니다.

원본 AA 서비스에는 세션 상태 거부가 남습니다. 원본 로컬 송신 함수가 0을
반환해도 AA 서비스의 정상 처리나 실제 휴대폰 수용을 증명하지 않습니다.
정상 전체 ServiceInit·하드웨어·휴대폰 세션은 실행 범위 밖입니다.

## 보존한 실패와 GDB의 범위

- 첫 qualified 실행은 중간 DR 선택이 있었지만, 긴 idle 뒤 원본 reader의
  부분 문장 상태를 잘못 가정한 작성 feeder 때문에 GPS 복귀를 완료하지 못했습니다.
  이때 별도로 발견한 `capture.stop` 파일/디렉터리 오류도 수정했습니다.
  그 표식 오류는 해당 실행에서 도달한 실패 원인이 아닙니다.
- 다음 qualified 실행은 원본 위치 요청 세 건과 대체 한 건 뒤 요청 진행이
  멈췄습니다. 후속 실행에서 재현되지 않았으며 원인을 해결했다고 주장하지 않습니다.
- GDB 첫 시도의 private Python 이름 충돌, 두 번째 시도의 idle 문장 경계,
  세 번째 시도의 debugger 기본 SIG32 정지를 각각 보존했습니다.
  원본 취소 신호를 그대로 전달하도록 GDB를 설정한 네 번째 실행이 완료됐습니다.
- 성공한 GDB 실행은 최초 QEMU 진입점에서 연결하고 계속 실행했습니다.
  중간 정지·스택 수집 trigger는 없었으며 원본 메모리·함수 반환을 바꾸지 않았습니다.
  debugger 연결의 타이밍 영향까지 없었다고 주장하지 않습니다.

## 독립 판정과 고정 입력

비공개 Python 판정기는 제품 적분기·serializer를 가져오지 않고 WGS84 RK4와
고정 LOCATION 형식으로 계산합니다. 독립 검토가 발견한 기한·요청·소비 경계
누락을 각각 실패 대조로 보존한 뒤 고쳤습니다. 대기열 전체가 아니라 실제 소비한
prefix에서 기준점·GPS POSITION·mode 0 POSITION·센서 창을 찾아 대조합니다.
무관한 미소비 tail을 허용하며 MODEL 거부와 입력 미소비를 구분합니다.
이 Python 검사 자체는 제품·원본 실행 개수에 합산하지 않습니다.

| 고정 항목 | SHA-256 |
| --- | --- |
| 실제 AA 제품 | `c1c0bf9175a57ebf0bf677aaa24d2d03ca958f5510337c6b5b9efe86f4ec7180` |
| 실제 LDS 제품 | `e6f560bc2185c68af5e9901f3ba05a2cecafbde30b0bfb1aaa5b2055e4b9c342` |
| qualified caller 기록 | `ebe592f70b444879cb356bb10e80410543c3613e9eaa87fe582d546f58522076` |
| qualified journal | `2f82d731e2f164ccb1c496ed73aad3b2bace25144caef8d2c1075e33c1cee254` |
| MODEL caller 기록 | `5a42b32d4106232bbc4f76ad8ab107789e3f5f0ee281881c7bf005d17e1b7710` |
| MODEL journal | `cb81345ed6e72acf85e9be63c77588cb6be6ff3f62bd8e87de31ffdac79ddbfd` |
| qualified/MODEL 독립 판정기 | `569055eac385da20ec13332e07dd76d9617c42eb4e6c7a9818943662c75bba3e` |

denied는 이전 고정 판정기 `262fad550a7e6e4b782858b8c4b5162e36e0c562dd33ff443d215f7648843b20`로
실행했고 원문 독립 재검토를 완료했습니다. 새 판정기의 실행으로 소급하지 않습니다.
각 실행의 원본·제품·고정 작성 입력과 비공개 설정은 전후 해시가 같았습니다.
이번 작업은 제품 소스 변경이 없어 전체 host/ARM suite를 다시 돌리지 않았습니다.
같은 제품의 [전체 8d 검사](PROVENANCE_CONTEXT_2026-10-02.md)는 과거 결과로 보존합니다.

GDB 13.1-3과 새 의존 패키지 19개는 전용 컨테이너에만 추가했습니다.
호스트 설치·기존 패키지 upgrade는 없고 다운로드 helper는 제거했습니다.
도구·패키지·다운로드 파일 목록은 보존했으며, 실행 환경과 GDB 제거는 후속
복구 시험이 진행 중이라 아직 완료하지 않았습니다.

## 남은 작업

자격 상실 → 실제 철회 → readiness만 회복한 상태의 비발행 → 새 원본 GPS와
BEGIN → DR 재개, 그리고 유효 후보가 있는 실제 callback의 inline 거부는 아직
완료하지 않았습니다. 원본 요청의 간헐적 정지, 응답 도착 전 출처 공급부,
물리 센서의 시각·품질·보정, 차량 기동·복구, Galaxy S25·동글·지도 반영도 남습니다.
새 설치 ZIP이나 v1.0을 발행한 기록이 아닙니다.
