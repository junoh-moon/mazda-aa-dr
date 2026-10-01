# LDS 관측 전달·원시 기록·정확 연결 코어 — 2026-10-02

고정 소스는 `597d8681d485fdd51785e65af21933609cf2cd46`입니다. LDS의
원본 호출·캐시 복사 경계를 관측하는 hook, 고정 크기 프로세스 간 전달,
AA worker의 독립 기록과 PC 분석기의 정확한 요청 연결을 구현했습니다.
새 소스의 전체 host·ARM 검사를 수행했으며 최초 host 시간 초과와 후속
동일 소스 통과를 아래에 구분합니다.

**이 커밋에는 실제 LDS cold installer·자동 preload 기동 연결이 없습니다.**
작성한 원본 함수 대역을 사용하는 hook 시험과 실제 제품 DSO 검사를
원본 LDS 전체 기동으로 표시하지 않습니다. 새 설치 ZIP 발행·차량 실행은
없으며 물리 입력 자격과 live ASSIST는 계속 미완료입니다.

## 구현한 계약

- 실제 등록 ID와 함수가 검증된 경우에만 고정 callback wrapper를 사용합니다.
  준비와 활성화를 구분하며 비활성·부분 공개 상태에서도 원본을 전달합니다.
  등록의 반환값·부작용·이전 callback 수명을 원본이 결정합니다.
- getter의 실제 목적 버퍼와 호출 위치, 원본 mutex 안의 복사를 결합해 읽기
  사본을 보존합니다. 해당 사본의 첫 검증된 update만 출처를 계승합니다.
  중간 쓰기·같은 값 재할당·미지원 쓰기를 구분하며 원본 쓰기를 막지 않습니다.
  service의 아홉 출력 필드와 출처도 같은 원본 mutex 안에서 함께 복사합니다.
- Path→method→reply→raw message 연결은 동기 호출 범위 안에서만 보존합니다.
  빌린 포인터를 기록하거나 비동기 전송에 남기지 않습니다. Path가 정상 반환한
  뒤, 캐시 mutex 밖에서만 관측 사본을 전달합니다. 취소·예외는 정상 완료로
  발행하지 않으며 원본 인자·반환값·errno를 유지합니다.
- 전달 형식은 버전 1의 명시적인 **640바이트 little-endian**입니다. 포인터,
  C++ padding, Ledger 내부 owner는 직렬화하지 않습니다. 문자열 네 개는
  각각 최대 63바이트와 NUL을 소유하며 잘린 문자열로 정확한 키를 만들지 않습니다.
- nonblocking UNIX datagram과 실제 SCM credentials를 사용합니다. 생산자
  경로에 heap 할당·I/O 재시도·새 기록 파일을 추가하지 않습니다. 손실 번호는
  포화되며 포화·관측 경쟁으로 정확한 손실 수를 모르면 그 상태를 남깁니다.
  UID 0은 해당 펌웨어 설정의 로컬 계정 경계일 뿐 실행 파일 인증이 아닙니다.
- AA worker는 기존 원시 POSITION/SEND를 먼저 기록하고, 한 차례에 최대
  **16개** sideband를 별도 `lds_sideband` 행으로 기록합니다. POSITION을
  기다리게 하거나 버리거나 수정하지 않습니다. 수신 불가·불량 형식·계정
  불일치는 별도 상태로 남기며 기존 raw 경로를 중단하지 않습니다.
- 같은 Journal의 회전·공간·종료 정책을 사용합니다. 종료 시에도 제한된
  배출 뒤 socket을 닫으므로 모든 대기 datagram의 수집을 보장하지 않습니다.
  `drain_limit`은 완전 수집 증거가 아니며 마지막 송신 손실은 후속 성공
  기록이 없으면 수신 측에 보고되지 않을 수 있습니다.

PC 분석기는 같은 기록 묶음·세션에서 **서버 transport GUID, 요청 client
unique name, request serial, 응답 server unique name, response serial,
reply_serial** 여섯 항목으로 먼저 연결합니다. 그 뒤 아홉 원시 필드를
대조합니다. 어느 행이 먼저 도착해도 처리하며 시각·좌표 유사성·실험 label로
누락된 identity를 대신하지 않습니다. GUID는 전체 bus의 GetId와 구분합니다.

늦게 발견한 충돌도 앞선 연결을 철회합니다. 행 중복, 충돌한 재사용, raw
누락, 불완전 identity, 값 불일치를 구분합니다. 세션당 키·기록 ID 4,096개
한도를 넘으면 임의로 오래된 키를 버려 새 연결을 확정하지 않고 연결 판정을
불확정으로 남깁니다. 출력 연결 예시는 256개로 제한하고 생략 수를 기록합니다.
rotation은 이어 읽되 다른 export나 boot의 키를 가져오지 않습니다.

Path 반환 숫자는 원본 진단으로 보존합니다. 실제 fixture의 정상 반환값은
0이며 임의의 100 성공 조건을 두지 않습니다. 정상 반환 표시·실제 send
성공·reply type·비충돌 정확 키와 원시 값 대조가 연결 조건입니다.
이 연결 실패 때문에 기존 raw·MODEL·GPS holdout 집계를 버리지 않습니다.

관측 수명·쓰는 사본 번호·송수신 순번·관측 시각은 물리 생산자 순번·측정
시각·complete-through watermark가 아닙니다. 개별 필드 출처가 unknown인
사본도 그대로 보존하며 `assist_ready=false`, 생산 시각 unknown을 유지합니다.

## 먼저 실패시킨 회귀와 집중 검사

이전 비공개 LDS 실행은 [필드별 출처 검증](LDS_OWNED_LINEAGE_2026-10-01.md)에
남아 있습니다. 그 원본 실행을 이번 커밋에서 다시 수행한 것으로 세지 않습니다.
이번 hook 회귀는 실제 제품 hook·Ledger·formatter와 작성한 원본 함수·호출
스케줄을 연결합니다. 별도로 원본 공유 runtime 아래 실행한 경우도 그 대역을
원본 LDS service로 바꾸지는 않았습니다.

| 새 회귀에서 확인한 실패 | 수정·대조 범위 |
| --- | --- |
| 전달만 하는 초기 hook이 관측 record를 만들지 않음 | 등록·수명·읽기 사본·service·reply 연결 24개 사례 |
| 실제 raw 연결 모순에서 원시 헤더까지 사라짐 | 헤더·원본 결과를 보존하면서 conflict만 표시 |
| 손실 counter가 최댓값 뒤 0으로 돌아감 | 포화·손실 상태 보존, 실제 datagram queue 압력 |
| AA worker에 sideband opened/기록이 없음 | 실제 worker·Journal·socket을 연결한 6개 시나리오 |
| 분석기 연결 부재·큰 JSON 정수의 OverflowError | 불량 metadata를 진단하면서 raw 분석 계속 |
| Path 반환값 100 가정으로 실제 0 응답을 배제 | 반환 숫자 gate 제거·원본 진단 보존 |
| boolean raw 값이 숫자 1과 같다고 연결됨 | identity 뒤 원시 타입·값 대조 |
| 늦은 충돌·다른 세션의 identity를 잘못 유지함 | 연결 철회·묶음/세션 경계 회귀 |

집중 host에서는 hook **24개**, worker **6개**, sideband 분석 **24개**를
통과했습니다. 실제 C++ formatter 및 hook의 `--emit` 출력을 Python이 읽어
양쪽 도착 순서를 검사했습니다. 원본 함수·입력과 sink의 송수신 metadata는
작성된 시험 조건입니다. endpoint 11개, calibration 51개, 기존 analyzer
34개도 별도로 통과했으며 전체 검사 수와 더해 새로운 사례 수로 부풀리지 않습니다.

저수준 전달·formatter의 집중 host 실행은 **351 checks**, 이후 컨테이너의
새 통합 실행과 전체 ARM 실행은 각각 **195 checks**였습니다. queue 압력
시험이 실제 수용된 datagram마다 assertion을 수행하므로 이 수는 환경의
queue 한도에 따라 달라집니다. 모두 종료 0이며 서로의 실행 수로 바꾸지
않았습니다. 최댓값·escaped 문자열 formatter 사례는 **3,049/4,096바이트**이고
정확한 용량·한 byte 부족·canary를 대조했습니다.

추가 실패도 보존했습니다. worker fixture가 `capture.stop`을 디렉터리 대신
일반 파일로 만들어 exit 142가 발생한 시도는 성공으로 세지 않았습니다.
fixture 형태만 바로잡고 제품 종료 정책과 alarm은 유지했습니다. 과거 workspace의
stale formatter로 실패한 calibration 시도는 새로 빌드한 소스의 51개 통과와
구분했습니다. GCC 4.9의 `msghdr` 초기화 경고는 완전 값 초기화로 고쳤으며
컴파일 경고 조건을 낮추지 않았습니다.

## 고정 소스 전체 검증

live 작업 폴더와 독립된 두 detached checkout에서 시작했습니다. 각각
367개 tracked 파일을 같은 Git archive와 대조하고 host BUILD와 ARM BUILD를
새로 만들었습니다. GCC 4.9.1 도구체인 커밋은
`61ec0343de84f6fc7c46840056df1d600d44be8a`입니다. 빌드 입력 68개를 고정
소스와 대조하고 다섯 ARM 산출물로 새 SHADOW bundle을 만들었습니다.
host 패키징 시험에는 이 bundle과 제공된 stock root를 명시했습니다.

첫 `make test`는 packaging 232개 중 한 사례의 오류로 종료 **2**였습니다.
`test_actual_testmode_and_relevant_process_maps_are_collected`에서 export
subprocess가 기존 **20초** 안에 끝나지 않아 TimeoutExpired가 발생했습니다.
내부 어느 단계가 지연됐는지는 첫 로그에 없으며 환경 부하 원인으로 단정하지
않습니다. 같은 소스·fixture·20초 제한의 단일 대조는 **0.570초**, 종료 0이었습니다.
제품·fixture·기한을 바꾸지 않은 별도 전체 host 재실행은 다음과 같습니다.

| 검사 | 실제 결과 |
| --- | --- |
| 전체 host `make test`, 별도 r2 | 종료 0, Python **506개**와 C/C++, 생략 0 |
| Python suite별 수 | 42 + 159 + 28 + 1 + 10 + 232 + 34 |
| 전체 고정 ARM runner, 최초 한 번 | 종료 0, 생략 0 |
| ARM 소스 fixture 출력 연동 Python | sideband 24 + SHADOW 12 + request journal 18 + calibration 51 |
| 실제 ARM 제품 DSO 8개 시험군 | **145개** 사례, 모두 종료 0 |

DSO 사례는 position 8, request 14, request-wire 1, endpoint 25, session 29,
bus 31, assist 28, runtime-assist 9입니다. 유지한 세 인자 `run_worker` ABI를
포함해 실제 제품 DSO를 호출했습니다. 이 시험들의 원본 콜백은 작성한
fixture이며 원본 LDS cold 설치나 차량 호출의 증거가 아닙니다. LDS hook·
worker 새 사례의 소스 링크 실행과 이 DSO 실행을 구분합니다.

실제 검사한 AA 제품 SHA-256은
`9b9f8b91be90e9b87f57251a17e5c93a7079e829f994da87071b6d36509ebced`입니다.
ARM runner 시작·종료 `ARM_TEST_INPUTS` 두 항목이 같고 `release_verified=true`
였습니다. 이는 고정한 빌드 산출물 대조 표시이며 공개 발행을 뜻하지 않습니다.
각 DSO 결과의 제품·소스 해시도 고정 소스와 일치했습니다.

첫 실패와 단일 대조, 후속 전체 host 로그는 별도로 보존했습니다. 두 checkout,
다섯 제품과 bundle, 도구체인 2,124개 파일, stock 의존성 29개, 실행 도구와
컨테이너 패키지는 검사 전후 같았습니다. 이 단위에서 추가 설치한 도구는
없으며 기존 공유 환경은 후속 원본 통합을 위해 유지했습니다.

## 남은 범위

이번 체크포인트는 실제 원본 LDS cold 설치·자동 callback 등록·제품 간
전달을 끝까지 실행한 결과가 아닙니다. 후속 installer·loader·설치 manifest
변경은 이 소스 핀에 포함되지 않으며 별도 실패 회귀와 원본 실행이 필요합니다.
새 ZIP의 순정 BusyBox 실행·공개 발행도 이번 검증 범위에 없습니다.
기존 원본 위치·계산 자료와 빈 실차 회수 기록을 새 성공 결과로 바꾸지 않습니다.
물리 생산자/측정 시각/수신기 자격, 실제 차량 기동·복구, 폰 수용과 v1.0 완성은
여전히 별도 미완료 조건입니다.
