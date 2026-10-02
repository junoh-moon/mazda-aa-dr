# 현재 상태와 인계 — 2026-10-02

이 문서는 새 리뷰어·LLM의 첫 진입점이다. 과거 상세 설계와 설치 가능 판단보다 우선한다. 현재 공개 설치 후보는 없다. 로컬 SHADOW 소스는 OFF·폴링 분리·일회성 기동 보호를 구현했다. 호스트·ARM·부분 OEM 실행과 실제 차량 검증을 구분한다. 소스 커밋과 공개 릴리즈는 별개다.

**2026-10-02 긴급 정정:** 공개 `v0.3.10-shadow.1`과 `.2` ZIP은 설치하지 마십시오.
원본 펌웨어의 부분 SM 에뮬레이션에서 `.2` ZIP의 LDS preload가 원본 LDS를
반복 종료시켰습니다. `.1`은 동일한 LDS DSO를 포함합니다. 두 릴리즈
설명에도 사용 중단 경고를 게시했습니다.
LDS DSO의 큰 정적 TLS가 작은 스레드 스택을 침범했을 가능성이 높지만,
스택 여유와 실제 차량 인과관계는 미확정입니다. 수정 후보의 LDS 생존은
전체 OEM 기동·AA·차량·폰 승인으로 세지 않습니다.
[LDS TLS 조사](../validation/LDS_TLS_CRASH_2026-10-02.md)를 먼저 읽으십시오.

**AA DSO의 후속 수정도 아직 차량용 릴리즈가 아닙니다.** 소스
`943c054a826b074985421668ebe607c8edf4cdbf`는 POSITION 문맥을
64개 전역 슬롯으로 옮겨 AA 정적 TLS를 7,284바이트에서 196바이트로
줄였습니다. 부족·중첩 초과·중단 때 원본 송신을 유지하고 관측 실패를
기록합니다. [AA 문맥 풀 검증](../validation/AA_CONTEXT_POOL_2026-10-02.md)의
전체 host/ARM, 실제 제품 DSO, 원본 BusyBox 설치 모의는 통과했지만
새 AA 제품의 원본 AAPA 전체 기동·실제 스택 여유·차량·폰 검증은 없습니다.
[원본 로더의 작은 스레드 대조](../validation/AA_STOCK_LOADER_STACK_2026-10-02.md)는
옛 AA 제품과 TLS 전용 7,284바이트 DSO가 작성한 16 KiB 스레드의 보호
페이지에서 실패하고 새 제품은 통과함을 확인했습니다. 실제 AAPA 스레드
크기나 전체 OEM 실행 근거로 세지 않습니다.
과거 [원본 AA 계산·송신](../validation/AA_PRODUCT_ASSIST_2026-10-02.md)과
[철회·재개](../validation/AA_ASSIST_RECOVERY_2026-10-02.md)는 변경 전
`8d5669c` 제품의 작성 입력 결과이며 새 제품의 재실행으로 세지 않습니다.
새 설치 ZIP이나 v1.0은 발행하지 않았고 live ASSIST는 비활성입니다.

후속 `3625449`의 [새 제품 원본 LDS→AA 실행](../validation/LDS_INLINE_PRODUCT_2026-10-02.md)은
POSITION/LOCATION 10쌍·90개 원시 필드와 현재 callback의 inline 연결 7건을
확인했습니다. AA TLS는 196바이트, 새 LDS는 52바이트이며 실제 DSO 집중
14사례와 전체 host Python 592개·C/C++는 통과했습니다. 첫 전체 ARM은
작성 센서 입력의 251.3ms 지연 거부로 실패했고 같은 binary의 단독 대조는
통과했습니다. 전체 ARM 통과로 합산하지 않습니다. 원본 실행의 후처리 시간
초과도 실패로 보존하고 같은 저장 캡처의 독립 대조 성공과 구분합니다.
[같은 제품의 ASSIST 복구 재실행](../validation/AA_POOL_APPLICATION_2026-10-02.md)은
작성한 자격 입력에서 18쌍·DR 대체 6건과 철회·재개·GPS 복귀를 확인했습니다.
이 fixture의 association_reader는 null이며 물리 공급부 완료로 합치지 않습니다.
부분 SM 기동, 물리 자격과 폰 수용은 계속 남습니다.

후속 `01cb939`의 [새 전체 검사](../validation/LDS_INLINE_FULL_2026-10-02.md)는
같은 제품 입력에서 여섯 바이너리를 다시 빌드하고 host Python 592개·C/C++,
전체 ARM Python 118개·C/C++, 실제 AA DSO 13개 suite·190사례와 원본
LDS 설치기 84사례를 생략 없이 통과했습니다. 이전 실패는 그대로 보존합니다.
첫 부분 SM VM은 240초 안에 LDS 상태를 내보내지 못해 미판정이며,
수집 도구를 보완하고 있습니다. 새 공개 설치 ZIP이나 v1.0 발행은 아닙니다.

후속 `9c8ef75`의 [원본 로더 추가 검사](../validation/AA_POOL_STOCK_UNWIND_2026-10-02.md)는
같은 실제 AA 제품의 POSITION/unwind 18개와 별도 로더 추적 1개,
source-linked ARM 18개를 통과했습니다. `452291c`의 숫자 메뉴 29개도
통과했으며 최종 ZIP 검사와 구분합니다. `jq` 누락의 최초 준비 실패와
컨테이너 전용 도구 추가도 보존했습니다. 원본 AAPA 전체 기동이나 실차
센서·폰 수용의 성공으로 세지 않습니다.

**2026-10-02 첫 실차 회수는 빈 기록입니다.** v0.3.9-shadow.1 설치 뒤
차량 시동 OFF/ON과 무선 AA 동글 사용이 있었고, v0.3.9-shadow.2로 다시
회수한 archive에는 trace·collector JSONL이 전혀 없습니다. 메뉴3의
`export_exit=0`은 빈 archive 회수 성공일 뿐입니다. CMU Linux 재부팅과
guard 소비는 판정되지 않았고 센서·SHADOW·폰 수용도 미판정입니다.
[실제 회수 분석](../validation/FIELD_V039_EMPTY_CAPTURE_2026-10-02.md)을
확인하십시오. 현재 셸은 닫혔으므로 새 차량 방문을 요청하지 않습니다.
공개 v0.3.9-shadow.3은 guard 표식 진단과 [정확한 CMU 재부팅 절차·확대 회수](../validation/STARTUP_RECOVERY_2026-10-02.md)를
포함합니다. 최종 ZIP의 순정 셸 실행과 공개 재다운로드를
[발행 기록](../validation/RELEASE_V039_SHADOW3_2026-10-02.md)에 보존합니다. 차량 점화 OFF/ON은
실제 CMU 새 부팅을 보장하지 않습니다. 별도로 정한 다음 시험이 있다면
출발 전 주차 중 `one_boot=consumed_this_boot`, 현재 collector 기록과 최근
poll을 확인하십시오. ASSIST는 계속 비활성입니다.

**2026-10-01 허용된 한 번의 실차 설치·시험 기회는 위 첫 시도에 사용됐습니다.**
이전 펌웨어 파일만 사용하는 제한은 그 통합 시험에 한해 갱신됐습니다.
추가 방문을 요청하지 않고 가능한 원본 런타임·오프라인 검증을 먼저 진행합니다.
별도 기회가 정해진다면 주차 중 기동·복구 확인과 자동 원본 관측·원시 센서·
SHADOW/GPS 제외 비교·회수를 한 번에 준비해야 합니다. 설치 성공만으로
측정 준비를 판정하지 않으며 저장 공간과 실제 계산 시도·중단 사유를 확인합니다.
실차 센서·폰 수용·물리 복구는 아직 미검증이고 제품 ASSIST는 미완료입니다.
[통합 시험 준비](FIELD_TRIAL_KO.md)와 [v1.0 완료 조건](V1_READINESS_KO.md)을
따르십시오. 공개판도 차량 승인을 받은 완성본이 아닌 SHADOW 시험판입니다.

마지막 공개 시험판 [v0.3.10-shadow.2](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.10-shadow.2)는 위 결함으로 사용 중단 상태입니다.
고정 소스 `be96c6de6619d8e9053a327fd16e9df718384ca4`의 설치 ZIP·
체크섬을 게시한 뒤 다시 내려받아 바이트·SHA·CRC·내부 source pin을
대조했습니다. [새 발행 검증](../validation/RELEASE_V0310_SHADOW2_2026-10-02.md)은
host Python 576개·C/C++, 고정 ARM Python 105개·실제 제품 DSO 145개·
원본 LDS 설치기 71개와 최종 ZIP 순정 BusyBox 검사를 구분합니다.
같은 부팅의 가드 선택 거부, 새 부팅의 소비 표식, 소비한 설정 해시와 현재
설정의 일치를 진단합니다. 표식은 SM 경로 수신이나 AA 기동의 증명이
아닙니다. 물리 CMU·센서·폰 시험은 새로 수행하지 않았고 ASSIST는
계속 비활성입니다.

후속 [LDS 할당 출처 진단](../validation/LDS_ASSIGNMENT_DIAGNOSTIC_2026-10-02.md)은
정확히 연결된 원본 응답에서 아홉 필드에 남은 캐시 할당 번호의 다양성과
미확인 상태를 PC 분석기에 표시합니다. 공개 `.2` ZIP의 제품 코드는
바꾸지 않았고 새 실차 기록도 얻지 않았습니다. 동일 번호나 동일 관측
시각은 물리 측정 시각·신선도·수신기 품질을 증명하지 않습니다. 같은 worker의
관측 연결과 별도로, 물리 자격을 갖춘 live 공급부와 ASSIST는 아직 미완료입니다.

이전 공개판은 [v0.3.10-shadow.1](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.10-shadow.1)입니다.
고정 소스 `50ba484133b0e52739e5af35f7dcc993d5a3f850`를 master에 반영하고
설치 ZIP·체크섬을 발행한 뒤 공개 파일을 다시 받아 대조했습니다.
[이전 발행 검증](../validation/RELEASE_V0310_2026-10-02.md)에 당시 host Python
533개·C/C++, 전체 ARM과 실제 AA DSO 145개·원본 설치기 71개,
최종 ZIP의 순정 BusyBox 네 검사를 생략 없이 기록합니다.
LDS 전용 제품을 자동 설치하여 callback·캐시 할당 출처와 응답 식별자를
기존 용량 제한 AA 기록에 전달합니다. 이전 v2 설치의 회수·제거도
재설치·재무장 없이 검사했습니다. 실제 관성항법 위치 적용은 미완료입니다.

이전 공개판 [v0.3.9-shadow.3](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.9-shadow.3)은
고정 소스 `0ae08ccfae3504bd54dd286baa6c682c3e75b059`를 master에 반영하고
설치 ZIP·체크섬을 발행했습니다. 당시 명시적인 순정 CMU 재부팅 요청과
새 boot ID 대조, 전체 설치 폴더·실제 autostart·SM·부팅 진단의 USB 회수,
보고서 쓰기 실패 표시를 포함합니다. 전원 상태와 단일 USB 교체 순서는
[설치 안내](ONE_COMMAND_INSTALL_KO.md)를 따르십시오.

후속 소스 `597d868`은 LDS callback·캐시·응답 관측 hook과 AA worker의
별도 수신 기록, 분석기의 정확한 요청 연결을 구현했습니다.
[코어 검증](../validation/LDS_RUNTIME_CORE_2026-10-02.md)에 새 host Python
506개·C/C++와 고정 ARM 전체·제품 DSO 145개 사례를 기록합니다. 최초
host 회수 시험의 20초 초과는 원인 미확정으로 보존하며 동일 소스의 단일
대조와 전체 재실행 통과로 구분합니다. 이 고정 커밋에는 LDS 자동 기동
설치기가 없으며 공개 `.3` ZIP에도 새 코어가 포함되지 않았습니다.
이 코어 pin 당시의 LDS 제품 설치·원본 실행 통합은 아래 후속 작업과 구분합니다.

후속 `50ba484`는 LDS 전용 제품 DSO와 정확한 cold 설치기, normal/WCP 자동
기동 템플릿을 연결했습니다. 원본 LDS 실행 코드를 유지하고 callback·
캐시·응답 연결을 관측하며, 기존 AA 용량 제한 기록으로 전달합니다.
[원본 설치기 검증](../validation/LDS_COLD_INSTALL_2026-10-02.md)의 ARM 71개와
호스트 1개는 직접 설치기 실행 결과입니다. 별도
[실제 제품 연결](../validation/LDS_PRODUCT_RUNTIME_2026-10-02.md)은 원본
응답 9건을 실제 AA worker에 기록하고 필드 81개와 정확한 요청 연결을
대조했습니다. 작성한 AA 시작·WorkerScope이며 정상 전체 기동·폰 송신은
포함하지 않습니다. 전체 분석은 `inconclusive`를 유지했습니다.
새 코드는 공개 v0.3.10-shadow.1에 포함됐으며 과거 `.3` ZIP은 변경하지
않았습니다. 공개 `v0.3.10-shadow.2` 시험판은 재부팅 뒤 가드 표식·설정·collector의
일곱 진단을 추가합니다. 공개 `v0.3.10-shadow.1`에는 `startup_state`,
`config_mode`, `runtime_disable_next_start` 출력이 없습니다. 이 진단만으로
LDS 실제 수신이나 물리 자격을 증명할 수 없습니다. live ASSIST는 비활성입니다.
v1.0에는 여전히 물리 입력의 단위·시각·품질, 요청별 자격, 실제 위치
대체와 GPS 복귀 및 폰/지도 반영의 근거가 필요합니다.

후속 소스는 [LDS 요청 연결 공급부](../validation/LDS_REQUEST_SOURCE_2026-10-02.md)를
기존 AA worker에 연결합니다. 원본 POSITION과 LDS 응답을 제한된 메모리에서
결합하고 실제 `AssistSource` callback이 소유된 결과를 조회하는 경로입니다.
`805362f` 전체 host·ARM 검사는 통과했지만 원본 첫 실행에서 9개 중 8개만
연결됐습니다. 첫 bus 발견을 수명 종료로 오인한 경계를 두 실패로 재현하고
`58b7749`에서 수정했습니다. 새 원본 실행은 9개 모두 연결됐고 거부·충돌은
0이며 종료 후 보관 항목도 0입니다. 같은 pin의 전체 host Python 533개와
C/C++, 전체 ARM·실제 AA DSO 145사례·원본 설치기 71사례가 생략 없이
통과했습니다. 새 연결·경계 코어 344개·151개와 worker 아홉 사례도 포함합니다.
이 커밋에 대응하는 새 설치 ZIP은 발행하지 않았습니다.
이 연결은 아직 qualified 기준점·센서 입력이 아니고, 이미 끝난 callback의
provenance도 바꾸지 않습니다. live ASSIST와 v1.0은 계속 미완료입니다.

[후속 원본 reader 실행](../validation/LDS_DRIVER_READ_2026-10-02.md)은
작성한 PTY 문장 다섯 개를 원본 reader·parser·callback·cache에 직접 넣고
원본 Close와 프로세스 정상 종료를 확인했습니다. 초기 세 실패와 작성한
부분 초기화 조건을 보존합니다. 제품 preload·AA worker 결합과 물리 입력
자격은 이 실행에 포함하지 않았습니다.

[제품과 결합한 원본 reader 실행](../validation/LDS_DRIVER_PRODUCT_2026-10-02.md)은
`0f9ffdd`의 실제 제품에서 LDS parser·callback 직접 호출 없이 원본 질의 9건을
AA worker까지 연결하고 숫자 81개를 대조했습니다. 작성한 부분 기동과
WorkerScope이며 실제 AA 송신·물리 입력 자격은 남습니다. 같은 pin의
[기동 진단 병합 검사](../validation/GUARDED_SOURCE_MERGE_2026-10-02.md)는
새 host Python 577개·C/C++, 변경 guard의 ARM 36개 및 로컬 ZIP의 순정
BusyBox 설치·회수 경로를 구분하여 기록합니다. 새 공개 릴리즈는 아닙니다.

[현재 callback의 요청 문맥 전달](../validation/PROVENANCE_CONTEXT_2026-10-02.md)은
one-shot으로 확보한 Trace·호출·세대를 inline 출처 검증 함수에 직접 넘깁니다.
`8d5669c`의 전체 host Python 577개·C/C++와 고정 ARM 전체, 실제 AA DSO
아홉 suite·152개, 원본 LDS 설치기 71개가 생략 없이 통과했습니다.
후속 `8f95298`의 PC 분석기 통합은 제품 입력 75개 동일성과 새 journal
Python 163개를 별도로 확인했습니다. 물리 자격 공급부는 계속 미구현이고
live ASSIST·폰 반영의 완료는 아닙니다.

[실제 제품과 원본 AA 세션 기동](../validation/AA_PRODUCT_STARTUP_2026-10-02.md)은
같은 `8d5669c` 제품의 cold 설치, 원본 IPC 세션 생성·해제와 BLM 작업 큐의
정상 join을 확인했습니다. 첫 큐 설정 실패를 보존하고 별도 IPC namespace에서
원본이 요구한 크기·권한을 맞춰 재실행했습니다. 작성한 시작 순서와 부분 기동이며
이 실행에는 위치 callback·DR 계산·LOCATION 송신이 없습니다. 실제 제품의
ASSIST 계산과 원본 AA 송신을 결합한 후속 실행은 아래 별도 기록을 따릅니다.

[실제 제품 계산·원본 AA 송신](../validation/AA_PRODUCT_ASSIST_2026-10-02.md)은
같은 제품에 작성한 자격·센서 입력을 공급하여 원본 LOCATION 7건 중
mode 0 4건·DR 대체 3건, 후보 발행 76회와 GPS 복귀 후 원본 전달을
확인했습니다. 별도 readiness 거부와 MODEL 실제 소비 대조에서는 대체가
없었습니다. 후속 [실제 제품 복구 실행](../validation/AA_ASSIST_RECOVERY_2026-10-02.md)은
GDB 없이 18쌍·DR 대체 6건, 자격 상실 뒤 새 GPS/BEGIN으로 재개,
inline 출처 거부와 최종 GPS 복귀를 완료했습니다. 간헐적 원본 요청 대기는
미해결로 보존합니다. 물리 자격·폰 수용과 배포 ASSIST는 미완료이며
공개 ZIP은 바꾸지 않았습니다.

[응답 전 LDS 출처의 callback 전달](../validation/LDS_INLINE_ASSOCIATION_2026-10-02.md)은
제품의 잠금 관측·읽기 전용 map·현재 요청 조회·POSITION/SEND 사본을
연결했습니다. 구성 요소의 host·고정 ARM 집중 검사를 통과했으며 새 제품의
전체 검사의 첫 실패와 후속 원본 경로 실행은
[새 제품 기록](../validation/LDS_INLINE_PRODUCT_2026-10-02.md)을 따릅니다. 이 관측 연결을 물리 센서
자격으로 승격하지 않고 live ASSIST는 계속 비활성으로 둡니다.

[AA 문맥 풀과 출처 연결의 병합](../validation/AA_POOL_ASSOCIATION_2026-10-02.md)은
외부 `master`의 작은 TLS 구조에 현재 callback의 소유된 LDS 출처를 보존합니다.
추가 fork 회귀를 보강했고 실제 새 DSO의 문맥 풀 12개·작은 스택 2개를
확인했습니다. 전체 ARM의 첫 실패는 별도로 남습니다. 자식 슬롯 회수나 작성한 작은 스택 대조를 물리
센서 자격·폰 수용으로 세지 않습니다.

아래는 `.2` 회수와 검증의 기록이며 새판의 실행 횟수로 바꾸지 않습니다.
실차에서 v0.3.9-shadow.1의 메뉴3이 정상 `/proc/mounts` 링크를 거부했습니다.
이후 핫픽스로 archive 회수는 성공했지만 trace·collector 기록 파일이 모두
없었습니다. 사용자는 설치·차량 점화 OFF/ON·무선 AA 동글과 S25 연결·주행을
확인했으며 CMU Linux 새 부팅 여부는 확인하지 못했습니다.
공개판 분석기 결과는 종료 2·`inconclusive`입니다.
[빈 기록 조사](../validation/EMPTY_CAPTURE_2026-10-02.md)에 원본 영구 저장장치
마운트 경로와 빠진 기동 진단을 구분했습니다. 실제 자동 기동·저장 실패의
원인은 아직 확정하지 못했으며 센서·계산 결과는 미판정입니다.
[회수 핫픽스](../validation/TRIAL_EXPORT_HOTFIX_2026-10-02.md)는
그 결함과 잘못된 시험 fixture를 수정했습니다. USB 파일을 교체하고 기존 한 줄과
`3`만 사용하며 제거 후에도 재설치·재주행 없이 보존 파일을 회수했습니다.
실제 Linux procfs와 순정 BusyBox의 회수 경로를 구분해 검사했습니다. 새 host
Python 426개·C/C++, 고정 ARM 전체와 최종 ZIP의 BusyBox 세 검사를 생략 없이
통과했고 공개 재다운로드 대조 및 임시 도구 제거를 마쳤습니다. 기존판의
BusyBox 통과를 실차 회수 성공의 근거로 취급하지 않습니다.

아래는 이전 공개판 [v0.3.9-shadow.1](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.9-shadow.1)의 기록입니다.
`3346854`의 새 ARM 빌드·설치 ZIP은 실제 AA 등록에서 서버 주소 GUID와
client 고유 이름을 보존하고 raw 송신 연결·요청 수명을 대조합니다. 정보가
누락돼도 원시 위치는 남깁니다. [연결 식별자](../validation/AA_ENDPOINT_IDENTITY_2026-10-01.md)와
[발행 검증](../validation/RELEASE_V039_2026-10-01.md)에 host Python 421개·C/C++,
고정 ARM 전체와 실제 DSO 8개 suite·145개 사례, 최종 순정 BusyBox ZIP의
설치·계정·종료·회수 검사를 생략 없이 기록했습니다. 원본 여덟 요청은 선행
`f2ec7c2`에서 실행했으며 최종 빌드 입력 63개·다섯 산출물이 같음을 대조했습니다.
이를 최종 pin의 새로운 원본 실행으로 세지 않습니다.

이전 절차는 USB 한 포트에서 설치 후 AA로 바꾸면 시험 부팅에 자동
기록된다고 가정하고, 메뉴2를 선택 진단으로만 두었습니다. 빈 실차 기록
이후에는 출발 전 주차 중 메뉴2의 기동 근거 확인을 요구합니다. 실제 순정
BusyBox에서 설치 USB 부재와 작성한 새 boot 뒤 이전 기록 회수를 검사했습니다.
물리 USB 전환이나 정상 전체 OEM 기동 검증은 아닙니다. 공개 파일을 다시 받아
SHA·CRC·manifest·고정 소스를 대조했고 추가 도구를 제거했습니다.
LDS 제품 공급부·물리 센서·차량 복구·폰 수용은 남아 있으며 ASSIST는 비활성입니다.

이전 [v0.3.8 검증](../validation/RELEASE_V038_2026-10-01.md)의
`40051f8` ARM 빌드·설치 ZIP은 GPS holdout 결과에 원본 위치의
호출·generation 식별자를 보존합니다. 분석기는 같은 기록 묶음·세션에서 정확한
원본 행을 연결하고 누락·중복·불일치·과거 형식을 구분합니다. 연결 진단 때문에
기존 MODEL 비교를 버리거나 GPS 차이를 참값으로 표시하지 않습니다.
[기능 검증](../validation/HOLDOUT_REFERENCE_2026-10-01.md)과
[발행 검증](../validation/RELEASE_V038_2026-10-01.md)에 새 checkout의 host Python
401개·C/C++와 고정 ARM 전체(생략 0), 최종 ZIP의 순정 BusyBox 설치·계정·복구·
회수·제거를 기록했습니다. 공개 파일을 다시 내려받아 SHA·CRC·전체 manifest·
고정 소스를 대조했습니다. 추가 설치 도구를 제거하고 최초 호스트 목록과
일치함을 확인했습니다. 실제 qualified 입력 공급부·물리 센서·차량 복구·폰 수용은
미완료이며 live ASSIST는 계속 비활성입니다.

후속 소스 `a0c6f5b`는 [LDS 필드별 쓰기 출처](../validation/LDS_OWNED_LINEAGE_2026-10-01.md)를
구현했습니다. 부분 갱신 사이에 다른 쓰기가 끼어도 각 필드의 출처를 실제로
읽었던 사본에서 계승합니다. 새 코어를 비공개 원본 LDS 관측 서버에 연결하여
이전 값 복원·같은 값 재할당·미확인 쓰기·응답 복사 뒤 갱신을 대조했습니다.
host·고정 ARM·순정 공유 runtime의 코어 각 11,732개와 독립 분석기 16개,
전체 host Python 401개·C/C++와 고정 ARM 전체를 생략 없이 통과했습니다.
v0.3.9에는 이 코어의 소스를 포함했습니다. 제품 LDS 기동·등록 수명과
프로세스 간 공급부 연결은 미구현이며 물리 측정 시각이나 ASSIST 자격을
부여하지 않습니다.

[v0.3.7 검증](../validation/RELEASE_V037_2026-10-01.md)은 `56a7042` 제품의 원본
위치 아홉 필드 보완, host Python 378개·C/C++·고정 ARM, 실제 위치 기록 함수와
원본 LDS parser→service 응답→제품 token 실행을 별도 보존합니다. 이 원본 LDS
실행과 기록 함수 직접 검사를 v0.3.8에서 다시 실행한 것으로 세지 않습니다.
기준점·GPS 콜백 결합 수정, 숫자 메뉴와 저장 공간 정책은 유지합니다.

외부 [v0.3.6 발행 기록](../validation/RELEASE_V036_2026-10-01.md)과
[기준점 결합 검증](../validation/ASSIST_ANCHOR_PAIRING_2026-10-01.md)을 보존합니다.
그 기록의 원본 VM runner 240초 제한 종료·bus lifetime 미확인 네 건은
이번 실행의 결과와 구분합니다. 기존 [v0.3.5](../validation/RELEASE_V035_2026-10-01.md)의
비활성 계산 경로 결함은 후속 판에서 수정했습니다.
아래 이력의 공개 ZIP 미갱신·미구현 설명은 각 검증 당시의 상태입니다.

공개 v0.3.8 이후의 [이전 세대 콜백 경계 보완](../validation/ASSIST_STALE_CALLBACK_2026-10-01.md)은
폐기된 qualified 계산기의 늦은 GPS 콜백과 새 기준점의 순서 역전을
구분합니다. 해당 수정은 이번 v0.3.9에 포함됐습니다.
해당 독립 변경 소스의 전체 host Python 375개·C/C++와 고정 ARM 전체는
생략 없이 통과했습니다. live ASSIST는 계속 비활성이고, 이 수정의 합성
회귀를 실차 입력·폰 수용 증거로 세지 않습니다.
[v0.3.8 소스와 병합한 후속 검증](../validation/ASSIST_STALE_CALLBACK_2026-10-01.md)도
새 ARM 빌드·로컬 SHADOW ZIP에서 host Python 401개·C/C++와 고정 ARM 전체를
생략 없이 통과했습니다. 과거 v0.3.8 ZIP은 그대로 보존하고 후속 수정은
별도 고정한 v0.3.9 ZIP으로 발행했습니다.
[큐 폐기 경계 보완](../validation/ASSIST_QUEUE_CUTOFF_2026-10-01.md)은
같은 tick의 큐에 남은 POSITION을 지우며 경계를 잃던 후속 결함을 수정했습니다.
수정 전 실제 ARM DSO에서 이전 기준점의 DR 선택을 재현했고, 수정 후 두
전달 순서의 거부와 정상 기준점의 회복을 대조했습니다. `9e082f7`의 전체
host Python 401개·C/C++와 고정 ARM 전체는 생략 없이 통과했습니다.
이 수정도 v0.3.9에 포함됐으며 live ASSIST 자격이나 LDS 제품 공급부는 추가하지 않습니다.
[외부 통합판 `d3c6fa1` ZIP의 원본 userspace VM 관측](../validation/INTEGRATED_VM_2026-10-01.md)은
진단용 PID 1에서 `sh install.sh`·일회성 guard·collector·원본
VBS/AA/LDS/navi 기동과 LDS 위치 질의 응답을 확인했습니다. GPS/CAN·live
AA session이 없어 위치 poll 22건은 모두 mode 0, 완전 LOCATION payload는
0건입니다. 분석은 종료 2·`inconclusive`이고 runner는 240초 제한
`observation_only`입니다. 정상 전체 차량 기동·실차·폰 자격을 추가하지 않습니다.

후속 [양성 입력 통합 VM](../validation/INTEGRATED_POSITIVE_VM_2026-10-01.md)은
외부 `d3c6fa1` 제품 DSO를 원본 LDS의 합성 NMEA와 원본 VIM·VBS의 합성 센서에
연결했습니다. r7에서 정차 영점 후보를 이동 GPS 기준점에 적용하고, 원본
mode 0 약 5초 동안 MODEL 유효 snapshot 47건을 얻었습니다. 별도 WGS84
계산은 작성한 10 m/s 휠 입력과 4.844초·48.440 m에서 일치했고 이동
mode 1 복귀 뒤 유효 결과가 철회됐습니다. LOCATION 48바이트 쌍 20건은
제품 기록에서 원본과 동일했습니다. 초기 입력 공백·중단된 holdout을
보존한 전체 PC 판정은 여전히 `inconclusive`입니다. 이는 부분 SM·가상
GPIO·합성 수신 시각 시험이며 차량·폰 검증이나 live ASSIST 자격이 아닙니다.

[외부 `c62b313` DSO 재실행](../validation/INTEGRATED_CURRENT_DSO_VM_2026-10-01.md)은
ASSIST 큐 수정이 포함된 `fda002…` 제품의 별도 ZIP을 같은 원본 LDS·VIM 경계에
연결했습니다. 두 VM에서 no-fix 중 연속 MODEL 유효 55·47건과 작성 휠
10m/s의 거리 일치를 다시 관측했습니다. guest 진단은 끝났으나 두 runner는
guest halt 뒤 제한 종료됐고 전체 분석도 `inconclusive`입니다. 해당 소스의
host/ARM 빌드·설치 fixture를 별도로 통과했다는 외부 기록이며 차량·폰 자격은
추가되지 않았습니다. 두 외부 VM 기록의 원시 자료는 이번 작업에 없어 수치를
독립 재검증하지 않았으며, v0.3.9의 `bb01d42…` 제품 검사와 합산하지 않습니다.

[원본 LDS 필드 출처 실행](../validation/LDS_FIELD_LINEAGE_2026-10-01.md)에서는
실제 NMEA parser→callback→잠금 cache 쓰기→원본 service snapshot→serializer와
공개 v0.3.5 제품 token을 연결했습니다. GGA 좌표·고도가 갱신돼도 이전 RMC의
UTC·방향·속도가 함께 반환되며, snapshot 뒤 새 쓰기가 완료되어도 이미 복사한
응답은 이전 값을 유지했습니다. 작성 입력·초기화·진단 계측의 결과이며 개별 필드의
물리 측정 시각이나 제품 qualified 공급부를 구현한 것은 아닙니다. 같은 바이너리의
서버 계측 on/off 응답도 비교했습니다. 제품 position 기록의 고도·horizontal·vertical
누락은 후속 소스에서 수정하여 formatter 회귀를 통과했고 v0.3.7에 포함했습니다.
실제 ASSIST는 여전히 비활성입니다.

[ASSIST 선택 후 철회 경계](../validation/ASSIST_SELECTION_BOUNDARY_2026-10-01.md)는
변경하지 않은 adapter 소스와 작성한 host 대기 지점에서 실제 pthread 철회를
실행했습니다. 선택 전 철회는 ORIGINAL, 마지막 선택 검사 뒤 철회는 이미 선택한
DR을 원본 대역에 전달했습니다. 현재 명시한 선택 시점 계약과 일치하며 추가
guard를 넣지 않았습니다. 실제 ARM DSO의 해당 구간과 OEM teardown 동시성은
이 검사에서 실행하지 않았습니다.

[평탄화 전 요청·응답 연결](../validation/WIRE_REQUEST_2026-10-01.md)을
`3812fe6`·`fe052be`에서 구현했습니다. 원본 builder와 pending의 실제 수명 안에서
raw serial·reply_serial·sender·오류를 제품 token에 연결하며 기존 공개 getter도
별도로 보존합니다. 원본 라이브러리의 29요청·역순 응답·주소 재사용·timeout·취소,
실제 제품 cold 설치와 원본 data-client 네 요청을 검사했습니다. 서버와 worker
소비는 작성한 fixture이며 전체 SM/LDS·차량 검증은 아닙니다. 해당 제품의 전체
host Python 374개와 C/C++·고정 ARM 검사를 생략 없이 통과했습니다.
후속 통합 제품은 위 v0.3.5로 별도 검사·발행했습니다. producer/snapshot 자격과
폰 수용은 여전히 남아 있고 ASSIST는 비활성입니다.

[실제 runtime worker의 ASSIST 연결](../validation/ASSIST_RUNTIME_2026-10-01.md)은
`2313373`에서 검증된 외부 입력의 소비·계산·발행·철회를 연결했습니다.
정상 동시 입력과 새 source/session은 원본 시각을 보존하며, 자격 상실 뒤에는
같은 epoch의 새 기준점을 요구합니다. 제어기는 host·고정 ARM에서 각각
12,643개 검사를 통과했습니다. 실제 worker 9개는 host·고정 ARM·순정 공유
runtime에서 통과했습니다. 전체 host는
생략 없이 통과했고 ARM은 새 시험의 링크를 보완한 뒤 남은 구간까지 통과했습니다.
master 반영과 임시 도구 제거를 완료했습니다. 물리 센서와 요청별 provenance
공급부는 여전히 미구현이며 정상 기동의 ASSIST 비활성과 공개 ZIP을 유지합니다.
아래 이전 검증의 worker 미구현 설명은 해당 커밋 당시의 범위입니다.

[후속 ASSIST 송신 기한 구현](../validation/ASSIST_PUBLICATION_2026-10-01.md)은
`f3556b4`에서 코어의 원본 입력 유효 기간·나이·시간·오차 예산으로 비동기 송신
가능 기한을 직접 구하도록 했습니다. 대기 중인 GPS/기준점 전환도 반영하며
좌표·측정 시각을 갱신하지 않습니다. 현재 시각만 매핑한 경로의 실패를 확인한 뒤
host Python 371개·C/C++와 고정 ARM 전체, 실제 제품 DSO의 호출 계약 검사를
생략 없이 통과했습니다. master에도 반영했고 임시 도구를 제거했습니다.
live 자격 입력과 qualified worker 발행 연결은 미구현이며 ASSIST는 비활성입니다.
공개 v0.3.4 ZIP을 바꾸거나 이 후속 소스를 새 릴리즈로 게시한 것은 아닙니다.

[ASSIST 계산·송신 상태 전환 수정](../validation/ASSIST_GENERATION_2026-10-01.md)은
`3d05a2c`에서 GPS 품질 변경과 복귀 때 계산기와 adapter의 generation이
어긋나던 두 결함을 해결했습니다. 이전 실제 ARM DSO에서 실패를 재현하고,
수정 제품의 계산→발행→송신 10개 사례를 고정 sysroot와 순정 공유 runtime에서
통과했습니다. 입력·자격과 worker 스케줄은 작성한 시험 조건이며, 실제 runtime의
qualified 입력 연결은 여전히 미구현입니다. 전체 host 검사와 패키징 누락 보완,
고정 ARM 전체 검사를 완료하고 임시 도구를 제거했습니다. 공개 설치 ZIP은 변경하지 않았습니다.

[같은 계산기의 ASSIST 재획득](../validation/ASSIST_REACQUISITION_2026-10-01.md)은
`c56219b`에서 새 기준점과 같은 GPS 복귀 관측을 중복 처리하던 결함을 수정했습니다.
재초기화 없이 두 단절을 처리하며 입력 순서·native/GPS 품질 복귀를 포함한
제품 DSO 15개 사례를 고정 ARM과 순정 공유 runtime에서 통과했습니다.
원본 측정 시각이 수신보다 이른 기준점도 별도로 검사했습니다. 실제 runtime의
qualified 입력 연결·물리 센서·폰 수용은 미완료이며 공개 설치 ZIP은 유지합니다.
전체 host·고정 ARM 검사도 생략 없이 통과했고 임시 도구를 제거했습니다.

앞선 외부 master에서 [v0.3.3-shadow.1](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.3-shadow.1)을
커밋 `260528c751739c0afb59153f7a5cfa3585eb6048`에서 빌드하여 게시한 것을 확인했습니다.
해당 host/ARM·순정 BusyBox 설치 검사는 외부 발행 기록을 따릅니다. 이번 작업은
공개 ZIP을 직접 내려받아 SHA-256·CRC·전체 manifest와 고정 커밋을 대조했습니다.
[이번 릴리즈 검증](../validation/RELEASE_V033_2026-10-01.md)에 파일 해시와 실행 범위를
고정했습니다. 기본 모드는 SHADOW이며 ASSIST는 비활성입니다. 실차 센서 callback,
위치 정확도, 정상 전체 차량 기동·복구와 폰 수용은 미검증입니다. 조사 문단의
`공개 ZIP 미갱신`은 해당 시점의 기록이며 포함 여부는 고정 커밋으로 대조하십시오.

외부 master `1385b5c`의 [원본 LDS 타이머 후속 계측](../validation/LDS_TIMER_CAUSALITY_2026-10-01.md)은
정지 중 QEMU 가상 시계가 계속 흐르고, 두 CPU의 개인 타이머가 guest
제어 레지스터 쓰기로 비활성화되며, GPT broadcast의 가까운 만료도
없는 표본을 기록했습니다. 별도 계측에서는 이미 지난 GPT 비교값이
긴 롤오버 대기로 이어졌지만, 계측 부하가 부팅 시점을 바꾸므로
원본 서비스 정지의 단일 원인이나 실차 동작으로 단정하지 않습니다.
파일 trace를 끈 재실행에서도 마지막 OCR1은 같은 CPU 스레드의 직전
카운터 읽기보다 254 tick 앞이었지만, QEMU 계산 시에는 이미 295 tick
지난 값이었습니다. 독립 재실행에서는 계산 시 325 tick이 지났고,
마지막 쓰기 뒤 같은 스레드의 TCN 재읽기는 제한 종료까지 미관측입니다.
TCG 스케줄링 지연의 영향과 guest 재시도 경로는 아직 확인하지 못했습니다.
기본 QEMU 완주와 ASSIST 자격·폰 수용은 계속 미검증입니다.
[후속 타이머·IRQ 대조](../validation/LDS_TIMER_IRQ_FOLLOWUP_2026-10-01.md)에서는
실시간 단일 TCG 스레드에서도 가까운 GPT 비교값이 계산 전에 지나간
사례를 기록했고, 그 실행의 마지막 GPT 출력선은 low였습니다. 별도
단일 스레드 실행의 정상 예약·callback은 제한 종료 직전에 발생해
후속 guest 동작을 판정할 수 없습니다. GIC IRQ 87 전달·CPU 수락은
같은 정지 경계에서 미측정입니다.
[같은 실행의 GPT→GIC→CPU 후속 계측](../validation/LDS_TIMER_WAKE_BOUNDARY_2026-10-01.md)은
LDS 질의 6회 뒤 비교값이 계산 시 287 tick 지난 사건을 포착했습니다.
QEMU 가상 시계는 계속 증가했지만 두 vCPU는 1·5·10초 표본 모두
halted였고, 새 GPT 출력·GIC IRQ 87 요청은 없었습니다. 이는 QEMU
모델 안의 정지 경계이며 OEM 재시도 코드와 실차 동작의 판정은 아닙니다.
같은 QEMU 바이너리의 300초 무개입 대조에서는 160초 후속 표본까지
두 CPU가 유휴 상태였으나 그 뒤 질의 4~18회가 재개됐습니다. 180초
무진행은 영구 정지가 아니며, 복귀를 일으킨 정확한 타이머 사건과
guest uptime·QEMU 가상 시각의 차이는 추가 조사 중입니다.
후속 호스트 QEMU 계측에서는 늦은 비교 뒤 롤오버 callback, 다시
예약된 OCR1 비교의 GPT IRQ 출력과 GIC IRQ 87 입력, 그 뒤의 LDS
질의 재개를 같은 실행에서 포착했습니다. [T16·T17 근거](../validation/LDS_TIMER_WAKE_BOUNDARY_2026-10-01.md)를
따릅니다. 같은 실행에 CPU 개인 타이머 IRQ도 있어 IRQ 87이 유일한
복귀 원인인지는 미확정이며, OEM 재시도·실차 동작은 여전히 미검증입니다.

[외부 master의 MODEL 초기화 원인 기록](../validation/MODEL_RESET_REVIEW_2026-10-01.md)은
이 작업 브랜치를 검토한 `260528c`의 독립 검증입니다. raw 원본이 원인
진단보다 먼저 저장되도록 보완한 내용을 작업 브랜치에도 통합했습니다. 주 계산기 reset 사유와 실제
`drain()` 호출 횟수를 분리해 표시하며, 이 수치를 센서 처리나 ASSIST
자격으로 승격하지 않습니다. 전체 host·고정 ARM과 원본 ARM BusyBox의
상태 표시·설치 에뮬레이션 통과는 외부 작성자의 실행 기록입니다. 이 문단은 해당 소스 변경의
기록이며 v0.3.3 ZIP에도 포함됩니다. v0.3.2 검증은
[이전 발행 기록](../validation/RELEASE_2026-10-01.md)에 남겨 두었습니다.

아래 조사 이력의 미구현·미검증 범위는 각 기록 시점의 상태입니다. 요청 관측은
후속 제품 설치·journal까지 연결했습니다. [제품 연결 검증](../validation/REQUEST_PRODUCT_2026-09-30.md)과
[후속 반복 취소 비교](../validation/MANAGER_CANCELLATION_2026-09-30.md)를 함께 따릅니다.

[이전 시험 후보](../validation/TRIAL_PREPARATION_2026-10-01.md)는 별도 커밋
`a29f1b89987dc647f2146df945b9d2ba16526047`의
`mazda-aa-dr-a29f1b8-shadow-trial.zip`입니다. 설치 전 예상 공간, 두 기록기의
8MiB 여유 정책, 중단·MODEL 초기화 사유와 실제 계산 호출 수를 추가했습니다.
최종 host Python 358개·C/C++, 고정 ARM 전체, 원본 BusyBox awk 33개와
최종 ZIP 설치·회수·제거 검사를 생략 없이 통과했습니다. 이 추가 보완은
공개 v0.3.2 ZIP에 없습니다. 동시 부하 시험의 지연 원인은 미확정이고 실차
센서·복구·폰 수용과 live ASSIST는 미완료입니다. 임시 도구는 모두 제거했습니다.

Shift 키가 동작하지 않는 사용자 제약에 맞춰 [후속 후보 b185b99](../validation/KEYBOARD_TRIAL_2026-10-01.md)에
숫자 USB 메뉴를 추가했습니다. `sh /tmp/mnt/sda1/trial` 한 줄을 열고
숫자로 설치·상태 확인·종료와 자동 USB 회수·제거를 선택합니다. 종료 확인이
실패해도 원본 회수를 시도하며 단계별 결과를 USB에 남깁니다. master `260528c`의
raw 우선 기록·진단 보완도 통합했습니다. 원본 BusyBox의 메뉴 전체 흐름과
상태 파서 35개, host Python 371개·C/C++와 고정 ARM 전체를 직접 통과했습니다.
실패 시 원본 셸의 회수도 검사했으며 추가 도구를 모두 제거했습니다.
작업 브랜치의 별도 후보이며 master 병합·GitHub 릴리즈 게시와 구분합니다.

[같은 후보의 GDB 관측](../validation/GDB_READINESS_2026-10-01.md)에서는 재빌드 없이
실제 ARM DSO의 요청 제출·worker에서 중단점·레지스터·호출 스택을 읽고
기존 합성 호출 검사를 정상 종료했습니다. 별도 ARM 계산 fixture도 MODEL
기준점 설정 성공·계산 함수 진입과 815개 검사를 확인했습니다. 순정 LDS·AA
모듈의 함수 심볼은 남아 있지만 제품·순정 소스 줄 정보는 없으며, 이번에
전체 서비스나 차량을 디버깅한 것은 아닙니다. 초기 기동 검사와 남은 중단
조건을 기록했고 임시 도구를 모두 제거했습니다. 후보 ZIP은 변경하지 않았습니다.

[MODEL 구간 순서 수정](../validation/MODEL_WINDOW_ORDER_2026-10-01.md)의 후속 후보는
`e2341fb`입니다. 기준점 준비 중 휠을 먼저 소비하여 정상 요레이트 평균 구간을
`LATE`로 거부하던 결함을 수정했습니다. 수정 전 회귀 실패와 수정 후 전체 host
Python 371개·C/C++·고정 ARM, 최종 ZIP의 순정 BusyBox 설치·숫자 메뉴 흐름을
생략 없이 통과했습니다. 원본 시각·250ms 나이 제한과 GPS 제어 경로를 유지합니다.
숫자 메뉴·저장 공간 보완을 포함한 고정 후보이며 공개 GitHub 릴리즈를 갱신한
작업은 아닙니다. 임시 도구를 제거했고 host 목록의 전후 동일성을 확인했습니다.
과거 동시 부하의 최초 지연 원인과 실제 ASSIST 적용·차량 검증은 여전히 남습니다.

[LDS 진단 API 조사](../validation/LDS_DIAGNOSTIC_PROVENANCE_2026-10-01.md)에서
`GetUbloxDiag`도 특정 위치 응답의 출처 증거로 바로 사용할 수 없음을 확인했습니다.
원본은 위치·진단 캐시를 별도 잠금으로 순차 갱신하고, 센서 상태도 다른 메시지에서
갱신합니다. 정적 분석이며 실행 중 경쟁 상황을 재현한 결과는 아닙니다.
생산자→snapshot→응답의 제품 연결은 미구현이고 live ASSIST는 비활성입니다.
이번 제품 코드 변경·새 VM 실행은 없으며 추가한 분석 도구는 제거했습니다.

[VIP 생산자 조사와 누적값 거부](../validation/VIP_ACCUMULATOR_2026-10-01.md)에서
제공 업데이트의 원본 MCU 명령어 일부를 작성한 RAM·입력으로 해석 실행했습니다.
요레이트 합계·개수의 넘침과 제출 실패 반환 뒤의 초기화를 확인했고, 넘친 합계를
정상 평균처럼 받아들이던 MODEL 조건을 수정했습니다. 개수 넘침·개별 표본 품질은
여전히 패킷만으로 판정할 수 없습니다. 실제 CAN·MCU 전체·물리 센서 실행이나
제품 ASSIST 연결 완료로 세지 않으며, 검증·추가 도구 정리는 연결된 기록을 따릅니다.

[세션 관측의 제품 연결](../validation/SESSION_PRODUCT_2026-09-30.md)에서는
생성·파괴·상태 callback을 관측하고 요청 발행 당시 문맥과 실제 send 대상을
별도로 보존했습니다. 외부 journal 수정 `1884228`을 반영한 최종 제품으로
원본 LDS 요청 12건·세션 재생성을 실행했고 관측 loss/fault는 0이었습니다.
원본 상태 callback의 INVALID와 stop 264도 별도 두 주기에서 확인했습니다.
host/ARM 전체 검사와 stock runtime의 세션 14개 검사를 통과했으며, 초기화
순서 오류를 고정 GCC 4.9까지 재현·수정했습니다. 이 관측은 요청의 세션
소유권·정상 폰 연결을 증명하지 않습니다. qualified 필드와 live ASSIST는
계속 비활성이며, 공개 ZIP은 변경하지 않았습니다.

[후속 실제 세션 전환 검사](../validation/SESSION_TRANSITION_2026-09-30.md)에서는
원본 manager 정지·세션 파괴 뒤에도 LDS 요청 1건이 남는 것을 확인했습니다.
새 세션·manager를 시작하고 callback 전달을 재개하자 이전 요청의 LOCATION이
새 세션으로 송신됐습니다. 제품은 요청 문맥 1과 송신 대상 2를 구분했고 분석기는
inconclusive를 반환했습니다. 실제 DSO·stock runtime의 새 회귀도 추가했습니다.
원본 요청의 세션 소유권·정상 폰 수용이나 ASSIST 구현 완료로 세지 않습니다.

[세션 경계의 송신 후보 철회](../validation/SESSION_PREDICTION_LIFETIME_2026-09-30.md)를
추가했습니다. 생성·파괴·상태 callback 전후에 이전 후보와 전환 중 발행된 후보를
무효화하고, 전환 중에는 ASSIST 선택이 원본을 전달합니다. 이전 제품에서 실패한
회귀 8개를 수정 후 통과했으며, 최종 제품 DSO·원본 공유 runtime의 세션 22개와
원본 LDS/AA 실행에서 실제 generation 변경을 확인했습니다. 이 기록 시점에는
MODEL/holdout의 세션 reset과 요청별 qualification이 미구현이었습니다.

[MODEL/holdout 세션 경계 처리](../validation/MODEL_SESSION_2026-09-30.md)를
후속 구현했습니다. 생성·파괴·상태 callback의 관측 revision이 바뀌면 계산·보정과
대기 입력을 초기화합니다. 이전 요청과 관측 불가 입력은 계산에서 제외하고 원본
기록은 보존하며, 새 센서 입력과 GPS 기준점 이후에만 MODEL 계산을 재개합니다.
일반적인 GPS→GAP 전환은 이 세션 reset을 일으키지 않습니다. 요청의 세션 소유권,
물리 센서·폰 수용을 입증한 것은 아니며 live ASSIST는 계속 비활성입니다.

[요청 경로 보존](../validation/REQUEST_ROUTE_2026-09-30.md)을 추가했습니다.
실제 비동기 전송 전에 목적지·객체 경로·인터페이스·메서드명을 복사하여
응답과 POSITION/SEND 기록까지 전달합니다. 문자열 부재·잘림을 구분하며,
응답 시점의 최신 정보로 요청을 덮어쓰지 않습니다. 서비스 이름만으로 실제
제공자나 수신기·세션 소유권을 인증하는 구현은 아직 없으며 ASSIST는 비활성입니다.

[원격 세션 경쟁 수정 통합](../validation/SESSION_MERGE_2026-09-30.md)에서는
외부 master `20bf583`의 storage 재읽기·중첩 lifecycle 수정을 같은 작업 브랜치에
반영하고 기존 revision·예측 철회·MODEL reset·요청 경로 복사를 보존했습니다.
수정 전 ThreadSanitizer의 실제 race와 회귀 6개 실패를 확인했고, 통합 후 host
297개 파이썬 검사와 C/C++·ARM 전체 검사, 원본 공유 runtime의 세션 28개를
통과했습니다. 원본 LDS 요청 15건과 별도 상태 callback에서 기록·전달을 확인했습니다.
ARM 합성 송신 시험 한 번의 실패는 후속 단독·전체 검사에서 재현되지 않았으며
원인은 미분리입니다. bus lifetime과 qualified 제공자·수신기·세션 연결은 미구현입니다.

[실제 버스 연결 수명 관측](../validation/BUS_CONNECTION_2026-09-30.md)을
후속 구현했습니다. 원본 생성·접속·해제와 실제 단절 신호를 관측하고 요청
발행/응답의 연결을 각각 보존합니다. 원본 close callback 앞에서 단절 신호가
소비되는 경로를 두 VM 실패로 확인하고 signal 관측을 보강했습니다. 최종
원본 LDS 요청 17건과 daemon 종료를 실행했습니다. 생성부터 관측한 연결의
로컬 식별자이며 daemon/provider 신원이나 요청의 세션 소유권이 아닙니다.
MODEL의 버스 경계 reset과 qualified 연결은 TODO이고 ASSIST는 비활성입니다.
이후 외부 master `e79ca52`·`d0c74d3`의 MODEL 시각·상태 이력·요청 문자열
검사를 [같은 브랜치의 통합본](../validation/OBSERVATION_SYNC_2026-09-30.md)에
반영했습니다. 이전 transport 입력의 잘못된 수락과 파서 결함을 직접 재현했고,
통합 제품으로 원본 LDS 요청 12건과 세션 전환·실제 버스 단절을 확인했습니다.
미관측 자격을 채우거나 ASSIST를 활성화한 변경은 아닙니다.

[MODEL의 버스 경계 처리](../validation/MODEL_BUS_2026-09-30.md)를 후속 구현했습니다.
실제 LDS submit에 사용된 연결을 관측하고 단절·새 수명·관측 불가·여러 연결의
경계에서 두 계산기의 기준점·보정·대기 입력을 초기화합니다. 이전 요청과 경계
이전 센서 입력은 계산에서 제외하며 원시 기록은 보존합니다. 원본 LDS 요청
17건을 실행했고 AA 세션을 유지한 실제 버스 해제에서 MODEL reset과 단절 중
원시 입력 78건을 확인했습니다. 이는 provider/receiver/session qualification이나
유효한 원본 GPS·폰 수용의 증명이 아닙니다. live ASSIST는 계속 비활성입니다.

[원본 LDS의 직렬 입력 기동 조사](../validation/LDS_INPUT_STARTUP_2026-09-30.md)에서는
작성한 NMEA를 격리 PTY에 공급하며 여섯 VM을 실행했습니다. 원본 GpioChip과
SYSTEM을 포함한 구성에서 LDS의 실제 system state 2 응답·USB 목록 요청까지
진행했지만, 위치는 계속 mode 0·READ_NOT_READY였습니다. 누락된 온도 경로에
따른 SYSTEM의 standby 전환 시도도 남았습니다. 원본 파서의 유효 fix와 이
GPS 입력에서의 제품 MODEL은 아직 미검증이며, 빌드만 한 후속 caller를 실행
증거로 세지 않습니다. 추가 도구의 정리는 아래 후속 통합 기록을 따릅니다.

[후속 수신기 대기 조사](../validation/LDS_RECEIVER_2026-09-30.md)는 원본 USB 목록
응답·callback·재시도 타이머의 실행을 확인했습니다. 초기화 전 추적에서 원본
USB 관리자의 두 전원 GPIO write는 성공했지만 이후 읽기는 0이었습니다.
VM의 GPIO 읽기 경로를 조사해야 하며 유효한 원본 GPS는 아직 얻지 못했습니다.
다섯 추가 실행의 위치 80건은 모두 mode 0·READ_NOT_READY입니다.

[GPIO 되읽기 보완과 원본 GPS 실행](../validation/LDS_GPIO_2026-09-30.md)에서
이 VM 기동 장애를 분리했습니다. 원본 핀 설정을 따르는 실험용 QEMU 모델에서
작성한 NMEA를 원본 LDS가 해석하고 유효→무효→재수신을 수행했습니다. 후속
제품 실행은 원본 위치 26건·raw 1,314개와 MODEL 유효 snapshot 51개를 보존했고,
독립 직진 계산식과 GPS 복귀 시 예측 철회를 확인했습니다. LOCATION 26건은
원본 바이트 그대로 송신됐으며 보정 위치의 ASSIST 선택은 아직 미완료입니다.
두 핀의 부분 모델·합성 센서·폰 부재라는 범위와 초기 실패를 검증 기록에 명시했습니다.
추가 도구는 모두 제거했고 호스트 설치 전후 목록이 동일함을 확인했습니다.

[순번 소진 수정의 통합·원본 재실행](../validation/PIPELINE_MERGE_2026-10-01.md)에서는
외부 `55bdd8f`의 회귀 6개를 이전 코드에서 재현하고 `3eecacd`에 통합했습니다.
ASan/UBSan navigation 2,414개, host Python 330개·C/C++와 고정 ARM 전체
검사를 완료했습니다. 최초 경로 누락으로 생략된 설치 21개는 별도 통과했습니다.
새 제품의 원본 위치 26건·raw 1,329개·MODEL 결과 56개와 GPS 복귀 시 철회를
확인했습니다. 최근 GPS가 항상 수락된 기준점이라는 검증기 가정도 바로잡았습니다.
원본 송신 바이트는 그대로이고 실제 ASSIST는 미완료입니다. 추가 도구는 제거했습니다.

[원본 AA 호출의 계산 위치 선택 시험](../validation/ASSIST_SELECTION_2026-10-01.md)은
별도 작성 실행 파일에 기존 core·adapter를 연결하고 합성 자격을 부여한 검사입니다.
곡선 이동을 계산한 위치가 원본 로컬 송신 API의 인자로 선택됐고 GPS 복귀 시
원본으로 돌아왔습니다. 자격 거부·MODEL 대조에서는 계산 결과가 있어도 원본을
유지했습니다. 같은 최종 호출기의 세 조건을 모두 검사했습니다.
제품 runtime/preload의 ASSIST 연결이나 실제 센서·폰 수용 완료가 아닙니다.
첫 대조 실행의 원본 manager 초기화 실패는 원인 미분리로 보존합니다.
후속 대조·검사 범위와 추가 도구 정리는 연결된 검증 기록을 따릅니다.

[버스 관측 수정과 MODEL 초기화 통합](../validation/BUS_MODEL_MERGE_2026-09-30.md)에서는
외부 `00c6c5e`의 주소 재사용·연결 이력 수정을 기존 MODEL 경계와 합쳤습니다.
수정 전 실패를 확인했고 통합 제품의 host Python 312개·C/C++와 고정 ARM 전체
검사를 통과했습니다. 원본 LDS 요청 14건과 버스 해제 중 raw 69건 보존을
확인했지만 유효 GPS와 ASSIST 자격은 미완료입니다. 추가 도구는 모두 제거했으며
같은 `feat/session-observation` 브랜치를 유지합니다.

[동시 버스 출처·수신 시각 수정 통합](../validation/BUS_CLOCK_MERGE_2026-09-30.md)에서는
외부 `cb65fd5`의 결함 두 개를 이전 소스에서 재현한 뒤 같은 브랜치에 합쳤습니다.
통합 제품 `1ccedbf`의 host Python 318개·C/C++·고정 ARM 전체 검사와 원본 공유
runtime의 세션 29개·bus 31개를 통과했습니다. 원본 VM의 실제 LDS 요청 17건,
세션 재생성과 버스 해제 중 raw 78건 보존도 확인했습니다. 이는 적용 경로의
경계 검증이며, 보정 위치를 실제 지도에 적용하는 live ASSIST는 여전히 미완료입니다.
이번 추가 도구를 모두 제거했고 호스트 설치 전후 목록을 대조했습니다.
마지막 fetch의 외부 `8cbcc29` 분석기로도 보관 로그 세 개의 전체 보고서가
동일함을 확인했습니다. 이후 [같은 브랜치에 분석기·시험 코드를 통합](../validation/MODEL_RESULT_MERGE_2026-09-30.md)하고
host Python 330개·C/C++와 고정 ARM의 결과 의미 검사 12개를 직접 통과했습니다.
제품 runtime 소스는 변경하지 않았습니다. 해당 통합 시점에는 유효 원본 GPS와
ASSIST가 미완료였으며, GPS 파서의 후속 VM 실행은 위 GPIO 검증 기록을 따릅니다.

[새로 제공된 펌웨어의 로컬 재실행](../validation/FIRMWARE_REPLAY_2026-09-30.md)에서는
동일 커널·rootfs 해시와 당시 master(`2b959ff`)의 재현 빌드를 확인하고 전체 host/ARM 검사를
생략 없이 통과했습니다. 원본 커널 VM의 baseline/SHADOW 비교에서 위치 API의
0값 응답, 제품 후크·SHADOW 초기화와 collector UID 1001을 확인했습니다.
센서·AA LOCATION 표본이 없어 분석은 inconclusive이며, 기존 전용 manager
취소·재연결 fixture를 재실행한 결과는 아닙니다. 추가 도구는 전용 컨테이너에만
설치했고 컨테이너·도구체인·임시 작업 폴더를 모두 제거했습니다.

[후속 OEM 위치 서비스 실행](../validation/OEM_LOCATION_2026-09-30.md)에서는
원본 LDS 제공자와 실제 위치 API 응답을 확인했습니다. mode·UTC·좌표는 모두
0이며 READ_NOT_READY 상태입니다. 후속 실제 SM 부분 그래프에서는 LDS의 SM
연결과 NNG 상태 조회가 진행됐으나 VBS의 CAN 준비 timeout으로 순정 SM이
VBS를 종료했고, 의존성을 유지한 jcinavi·jciAAPA는 시작되지 않았습니다.
별도 aap_service는 RUNNING이었습니다. 유효한 GPS
또는 정상 전체 기동으로 판정하지 않습니다.
같은 이미지에서 우리 preload·collector 없는 baseline도 VBS 시작 제한과
같은 네 서비스의 STOPPED 상태를 보였습니다. 정확한 실패 원인은 미분리입니다.
후속으로 VIM·SM의 strace까지 제거한 baseline에서도 같은 실패가 발생했습니다.

[원본 VIM/CAN 콜백 실행](../validation/VIM_CALLBACK_2026-09-30.md)에서는
실제 등록과 합성 입력 세 건의 원본 MQ·CAN callback 진입/복귀·AA raw 수신을
확인했습니다. [위치 객체 ABI 실행](../validation/REQUEST_PROVENANCE_2026-09-30.md)에서는
원본 생성·파괴와 주소 재사용을 확인했습니다. 합성 소프트웨어 경로의 증거이며
물리 센서·실제 요청 출처·폰 수용이나 ASSIST 연결의 완료는 아닙니다.

[요청 관측 identity 자료구조](../validation/REQUEST_TRACE_2026-09-30.md)를
별도 구현하고 합성 회귀를 추가했습니다. 생산 바이너리에는 연결하지 않았으며
실제 요청·응답·작업의 OEM 생존 경계 연결은 명시적인 TODO입니다.

[원본 LDS 비동기 요청 실행](../validation/LDS_ASYNC_2026-09-30.md)에서는 격리 VM의
원본 LDS에 두 GetPosition을 동시에 대기시키고, 요청별 method·context·reply
sender·userdata callback·정상 정리를 hardware breakpoint만으로 관찰했습니다.
JCIDBUS connect는 0이 아닌 값이 성공이라는 계약 정정도 기록했습니다. 값은 모두
mode 0이며 BLM worker 큐·AA 세션·수신기 자격·취소 경로는 여전히 미검증입니다.

[별도 LDS API 장애 시험](../validation/LDS_CLIENT_API_2026-09-30.md)에서는
userspace debugger 없이 64건 동시 대기의 userdata callback, 버스 부재의 연결
실패, 제공자 부재의 실제 ServiceUnknown callback을 확인했습니다. data client를
직접 호출한 결과이며 제품 BLM의 util 계층·정리 hook·ASSIST 연결 검증은 아닙니다.

[후속 연결·지연·AA용 util 실행](../validation/LDS_CLIENT_LIFECYCLE_2026-09-30.md)에서는
원본 AA용 공개 요청 함수까지 실행했습니다. 제공자 종료 후 util callback의 NULL
error·mode 0, 45초 넘게 대기한 요청의 제공자 재개 후 완료, free/recreate의 주소
재사용을 확인했습니다. 동일 객체·이름의 재연결은 실패했고 원인은 미분리입니다.
그 시험에서는 BLM callback·worker·송신 경로를 실행하지 않았습니다.

[후속 원본 BLM 큐 실행](../validation/BLM_QUEUE_2026-09-30.md)에서는 원본 LDS
AA용 API에 원본 BLM callback을 전달하여 실제 큐·위치 worker·RequestSendPosition과
기존 OBSERVE 후크까지 실행했습니다. 정상 버스 요청/reply 대응과 제공자 종료 후에도
0값 위치가 전달되는 현상을 확인했습니다. AA 세션이 없어 실제 send는 0건입니다.
요청 출처의 제품 연결·qualified 자격·폰 수용은 여전히 미구현 또는 미검증입니다.

[후속 AA 로컬 세션 시험](../validation/AA_SESSION_2026-09-30.md)에서는 원본
세션 생성·파괴와 명시적 송신 API를 실행했습니다. 하위 send는 생성 전 256,
생성 후 0, 파괴 후 256이었고 OBSERVE가 입력·반환을 보존했습니다. 폰이 없어도
0을 반환하며 서비스에는 미시작 세션의 요청 거절이 기록됐습니다. 위치 요청과
송신을 연결한 전체 경로는 아니며 원본 정리 오류도 남아 있습니다.

[후속 원본 AA manager 시험](../validation/AA_PIPELINE_2026-09-30.md)에서는
원본 시작 API가 만든 스레드의 자동 LDS 요청부터 큐·위치 worker·실제 하위
send까지 연결했습니다. 원본 LDS의 mode 0 응답과 명시한 합성 위치를 구분하여
캐시 재송신을 재현했고, 기존 OBSERVE/SCRUB의 native 바이트 전달을 검사했습니다.
정지 뒤 추가 스레드 소멸·큐 종료도 확인했습니다. 정상 폰 연결 사건·폰 수용,
오류 없는 세션 정리와 요청 identity의 제품 연결은 여전히 남아 있습니다.

[후크 없는 세션 정리 비교](../validation/AA_CLEANUP_BASELINE_2026-09-30.md)에서는
제품 adapter·preload 없이도 원본 mutex destroy 오류를 재현했습니다. 같은
프로세스의 세션 재생성과 추가 task 소멸은 확인했지만 오류 원인·영향과 정상
전체 애플리케이션 정리는 미해결입니다. 500ms 대기와 StopSession 선행 호출도
이 진단 조건에서 정리 오류를 없애지 못했습니다.

[실제 요청 관측 연결](../validation/REQUEST_LINK_2026-09-30.md)은 새 Observer와
adapter의 관측 전달을 원본 요청→reply→BLM worker→native send에 연결한
비공개 fixture입니다. 정상/제공자 부재 응답과 네 요청 동시 대기를 검사했고,
util에서 사라지는 ServiceUnknown도 보존했습니다. 제품 cold-install·journal
연결, 실제 취소/예외 경계와 receiver/session·센서 자격은 아직 미구현 또는
미검증입니다. 생산 runtime의 request reader와 ASSIST는 활성화하지 않았습니다.

[후속 실제 요청 종료 시험](../validation/REQUEST_LIFECYCLE_2026-09-30.md)에서는
연결 free/disconnect에 따른 12건의 실제 method 정리와 관측 슬롯 회수,
새 연결의 늦은 응답 네 건을 확인했습니다. 같은 이름의 재연결 실패,
timeout 만료·userdata 누수 여부와 제품 설치/journal 연결은 남아 있습니다.

[ARM 예외 정리 수정](../validation/ADAPTER_UNWIND_2026-09-30.md)에서는 기존
위치 후크를 통과하는 합성 예외의 abort를 재현·수정했습니다. 실제 배포 DSO와
원본 libc/C++ runtime VM의 작성 target에서 예외·취소 8개 사례를 통과했습니다.
OEM 함수 전체의 unwind나 비공개 요청 후크의 모든 예외 경계까지 검증한
것은 아닙니다. 전체 host/ARM 회귀와 네 독립 코드 리뷰 결과도 기록했습니다.

[요청 관측의 제품 연결](../validation/REQUEST_PRODUCT_2026-09-30.md)을 구현했습니다.
실제 배포 DSO의 한 번의 cold-install과 journal을 원본 manager→LDS→worker→send에
연결하여 정상 요청 13개와 제공자 부재 요청 11개의 metadata를 검사했습니다.
util에서 사라지는 ServiceUnknown도 로그에 남습니다. host/ARM 전체 회귀와
독립 리뷰 네 개를 완료했습니다. 작성 request 예외/취소 13개는 실제 DSO와
원본 runtime에서도 통과했습니다. 축소된 free/resume 비교는 통과했지만,
전체 manager 실행 뒤 반복 취소·재개에서 발생한 AA용 요청 timeout의 원인은
미분리입니다. 이 실패와 최종 검사의 명시적 제외 범위를 새 기록에 보존했습니다.
관측 ID는 receiver/session·센서 자격을 만들지 않으며 live ASSIST는 꺼져 있습니다.
공개 ZIP은 아직 기존 v0.3.1-shadow.1입니다.

[후속 세션 상태 콜백 실행](../validation/SESSION_EVENTS_2026-09-30.md)에서는
원본 생성·송신·시작의 반환 0 뒤에 실제 INVALID/-1 callback과 stop 264를
두 VM에서 각각 두 번 확인했습니다. 생성별 callback 문맥을 보존하는 재현
도구와 판정기를 저장소에 추가했고, 원격 `ccfb255` 반영 후에도 Python 281개와 C/C++ 검사를
통과했습니다. 합성 0값 시작 인자를 쓴 별도 진단이며 제품 요청별 세션 자격,
정상 폰 연결·수용과 ASSIST 활성화 완료는 아닙니다. 추가 도구는 제거했습니다.

[후속 전체 취소·재개 실행](../validation/MANAGER_CANCELLATION_2026-09-30.md)에서는
동일 제품 ELF로 전체 manager 경로와 취소 12건·지연 응답 4건을 끝까지
검사했습니다. 별도 후크 유무 비교에서도 manager 실행 후 각각 요청 80건,
free/resume 20회를 완료했고 전후 제어·위치 조회도 응답했습니다. 이전 timeout은
재현되지 않았으며 원인 해결로 세지 않습니다. 제품 코드 변경 없이 얻은 추가 근거입니다.

[센서→제품 MODEL 통합 실행](../validation/SHADOW_RUNTIME_2026-09-30.md)에서는
연속 합성 입력의 실제 VIM/VBS→AA 계산을 확인하고, 센서 datagram의 EAGAIN
누락을 줄이도록 수신 대기를 개선했습니다. 최종 실행은 raw 916건·정차 보정·
직진/회전·yaw 단절·재기준점 복구와 독립 수치 대조를 완료했습니다. host/ARM
전체 검사와 독립 리뷰 네 건도 완료했습니다. 다만 같은 최종 ELF의 다른 실행은
요청 관측기의 잠금 경합으로 실패했습니다. 이 원인 분리는 남아 있으며, 전체
통합 안정성·native LOCATION 송신·실차·폰 수용의 완료로 세지 않습니다.
공개 ZIP은 변경하지 않았습니다.

[요청 관측 잠금 분리](../validation/REQUEST_CONTENTION_2026-09-30.md)는 작성 코드에서
재현한 request 정리·worker 실행·상태 조회의 상호 간섭을 제거했습니다. 세대
공개/손실 처리와 테스트의 거짓 통과 두 건까지 보강했고, host/ARM 전체 검사와
독립 리뷰 네 건을 완료했습니다. 원본 VM 두 실행의 요청 관측 손실은 0이지만,
첫 실행에서 별도 journal 큐 drop 1건으로 MODEL이 중단됐습니다. 같은 이미지의
후속 성공으로 이를 닫지 않았으며 journal 누락 원인 분리는 다음 작업입니다.

[동시 상태 조회 추가 회귀](../validation/REQUEST_STATUS_POLL_2026-09-30.md)는
원격 잠금 분리를 반영한 뒤 두 조회 스레드와 2,000개 요청 주기를 검사했습니다.
host·고정 ARM·원본 libc/C++ runtime에서 통과했고, 수정 전 native/ARM은 같은
검사에서 손실로 실패했습니다. 전체 host/ARM와 sanitizer도 통과했습니다.
추가 도구는 제거했으며, 별도 journal 누락·실차·폰 검증은 계속 미완료입니다.

[Journal 큐 후속 수정](../validation/JOURNAL_QUEUE_2026-09-30.md)에서는 빈 큐에서도
consumer/producer 잠금이 겹치면 기록을 버리는 경로를 재현·제거했습니다.
미완성 입력이 남은 종료와 전역 초기화도 보강했습니다. 네 독립 리뷰에서 발견한
고정 ARM compiler의 64비트 load 후행 장벽 문제까지 수정·재검증했습니다.
최종 host/ARM 전체 검사와 실제 제품의 원본 VM 두 실행
(raw 936·855건, journal/요청 loss 0)을 완료했습니다.
과거 실패의 정확한 busy/full 원인은 미확정이며 물리 센서·폰 검증을 대신하지 않습니다.
공개 ZIP은 변경하지 않았습니다.

[독립 세션 진단 검토·직접 재실행](../validation/SESSION_DIAGNOSTIC_REVIEW_2026-09-30.md)에서는
원격 작성 probe·상태 조회 회귀를 검토하여 통합했습니다. NULL 상태·필수 작업
누락·종료 표식 순서의 판정 오류와 callback 검사의 결손을 고쳤습니다.
직접 실행한 원본 VM 두 번에서도 API 반환 0과 INVALID callback의 차이를
확인했습니다. 원본 큐·LDS·제품 request hook을 실행하는 시험은 아니며,
제품 세션 자격 연결과 ASSIST는 여전히 미구현 또는 비활성입니다.

외부 master `20bf583`의 [세션 관측 검토·수정·직접 실행 기록](../validation/SESSION_CONTEXT_REVIEW_2026-09-30.md)에서는
독립 구현의 issue/send 문맥을 통합하고 storage 재읽기 race·중첩 생성/파괴의
잘못된 생존 관측·분석기 상태 이력 모순 누락을 재현·수정했습니다.
네 독립 리뷰와 최종 host/ARM 전체 검사 뒤, 실제 제품으로 원본 callback과
세션 재생성 이후 지연된 LDS 요청 네 건의 전달을 확인했습니다. 이전 요청의
문맥과 새 송신 대상을 구분하는 관측이며 session/receiver 자격과 ASSIST 연결은
미완료입니다. 반복 VM 한 번의 정리 검사 실패는 원인이 미분리이며, 세부 진단을
추가한 다음 VM의 성공으로 해결됐다고 판단하지 않습니다.
공개 ZIP은 기존 v0.3.1-shadow.1 그대로입니다.

[세션 후보 철회·MODEL 초기화 검토](../validation/MODEL_SESSION_REVIEW_2026-09-30.md)에서는
독립 변경을 통합하고 오래된 MODEL 계산의 재생성 후 잔류를 재현·수정했습니다.
세션별 기준점·학습 보정값·대기 입력을 초기화하고 제외한 센서 원본과 사유를
보존합니다. 세 독립 리뷰, host 300개 Python 및 C/C++·고정 ARM 전체 검사를
완료했습니다. 최종 제품의 원본 VM에서 두 조건 각각 지연 LDS 요청 네 건의
계산 제외와 순정 송신 보존, 합성 raw 336·587건 보존을 직접 확인했습니다.
모든 MODEL은 invalid이며 일반 분석 결과는 inconclusive입니다. bus/provider/
receiver 자격과 유효한 원본 GPS·실차·폰 검증은 남습니다. 앞선 반복 VM 정리
실패의 원인도 미분리입니다. 새 외부 route 조사 커밋은 다음 검토 대상으로
기록했으며 현재 공개 ZIP은 바꾸지 않았습니다.

[요청 경로 복사·저장 검토](../validation/REQUEST_ROUTE_REVIEW_2026-09-30.md)에서는
외부 route 변경을 통합하고 비동기 submit 전의 네 문자열을 journal까지 보존합니다.
세 독립 리뷰에서 파서의 불가능한 문자열 허용과 getter 혼선·일반 worker 버퍼의
회귀 공백을 재현·수정했습니다. host 301개 Python 및 C/C++·고정 ARM 전체 검사와
후속 변경 대상 검사를 완료했습니다. 최종 제품의 원본 VM에서 실제 요청 30건의
route, 세션 교체 뒤 지연 응답의 이전 문맥과 MODEL 제외를 직접 확인했습니다.
raw 350·441건은 직접 넣은 합성 입력이며 모든 MODEL은 invalid, 일반 분석은
inconclusive입니다. provider/bus/receiver 자격·실차·폰 검증 및 이전 정리 실패의
원인은 남아 있으며 공개 ZIP은 변경하지 않았습니다.

[고정 ARM 자원 측정](../validation/ARM_ROUTE_RESOURCES_2026-09-30.md)에서는
위 제품의 `.bss` 증가 101,408바이트, 스레드별 TLS image 증가 2,112바이트와
worker local stack frame 359,120바이트를 확인했습니다. 계측 객체 12개는 기존
제품 객체와 일치하며, ARM의 전체 관측값 큐 1,024개 복사·full 경계도 통과했습니다.
실제 CMU의 메모리·스택 여유나 callback 시간 상한을 검증한 결과는 아닙니다.

[버스 연결 관측 검토·직접 실행](../validation/BUS_CONNECTION_REVIEW_2026-09-30.md)에서는
외부 버스 구현을 통합하고 연결 이력 모순의 거짓 통과, 증분 빌드 의존성과
설치·후보 철회 회귀 공백을 수정했습니다. 세 독립 리뷰, host Python 306개와
C/C++·고정 ARM 전체 검사를 통과했습니다. 최종 제품의 원본 VM에서 실제
LDS 요청 30건의 발행/응답 연결을 보존했고, 실제 daemon 종료 뒤 단절 관측과
generation 18→20, 원본 close callback 0회·bus fault 0을 직접 확인했습니다.
raw 423·500건은 합성이며 MODEL은 invalid, 일반 분석은 inconclusive입니다.
이 외부 검토 시점에는 MODEL의 버스 경계 초기화가 미완료였으며,
[후속 버스 계산 경계 검사](../validation/MODEL_BUS_2026-09-30.md)에서 구현했습니다.
provider/receiver 자격과 이전 정리 실패의 원인·실차·폰 검증은 남아 있습니다.
공개 ZIP은 변경하지 않았습니다.

[후속 MODEL 버스 경계 검토](../validation/MODEL_BUS_REVIEW_2026-09-30.md)에서는
연결 변경 때 기준점·학습값·대기 입력을 초기화하는 외부 변경을 통합했습니다.
세 독립 리뷰와 직접 재현으로 동시 출처 표시 누락·분석기 모순 허용을 수정했고,
정상 datagram을 미래 시각으로 거부하던 실제 ARM 수신 순서 결함도 고쳤습니다.
최종 제품의 원본 VM에서 LDS 응답 42건과 AA 세션을 유지한 복수 연결·해제를
검사했습니다. 합성 raw 437·549·480건을 보존했으며 원본 GPS는 모두 mode 0,
MODEL은 invalid, 일반 분석은 inconclusive입니다. 상세 host/ARM 결과와
초기 실패는 새 기록을 따릅니다. 기존 MODEL 결과 의미 검사 공백은 별도
TODO이며 provider/receiver 자격·실차·폰 검증과 공개 ZIP 갱신은 미완료입니다.

[MODEL 결과 의미 검토](../validation/MODEL_RESULT_REVIEW_2026-09-30.md)에서는
위 분석기 공백을 수정했습니다. 실제 계산기 경계 출력과 세 독립 리뷰,
host Python 330개 및 C/C++·고정 ARM journal 검사를 완료했습니다.
기존 host/ARM worker 46개와 원본 VM journal 5개의 전체 분석 보고서는 동일합니다.
새 원본 VM 실행이나 유효 GPS 검증은 아닙니다. 별도 generation 소진 후 fault의
유효성 재노출은 독립 재현된 미구현 TODO로 남겼습니다. 외부 Claude 브랜치의
cb65fd5 병합도 확인했습니다. ASSIST와 공개 ZIP은 변경하지 않았습니다.
같은 기록에서 클로드의 후속 원본 SM·SYSTEM·USB 관리자 기동 조사도
별도 커밋으로 확인했습니다. 외부 실행과 직접 실행을 구분합니다.

[Pipeline 순번 소진 수정](../validation/PIPELINE_EXHAUSTION_2026-09-30.md)에서는
앞서 남긴 MODEL 유효성 재노출과 추가로 발견한 큐 범위 위반을 공개 API와
ASan/UBSan으로 재현·수정했습니다. root의 전체 host 330개 Python 및 C/C++,
고정 ARM 전체 검사는 통과했습니다. 순정 VM·차량·폰 실행이나 공개 ZIP 갱신은
이번 결과에 포함되지 않습니다.

외부 master `9fce0aa`의 [원본 커널 GPIO 진단](../validation/LDS_GPIO_VM_2026-10-01.md)은
별도 격리 VM의 출력 latch 1·방향 1·PSR/sysfs 0을 보고합니다. 원본 LDS가
OTG 전원 `1` 되읽기를 기다리는 정적 조건도 구분했습니다. 이 외부 기록의
VM에는 LDS 서비스가 없으며, 해당 기록을 이 작업 세션의 직접 실행으로 세지
않습니다. 실제 LDS 파서의 후속 실행은 위 GPIO·제품 통합 검증 기록을 따릅니다.

외부 master `7b5ceb6`의 [VIP 누적값 통합 검사](../validation/VIP_ACCUMULATOR_INTEGRATION_2026-10-01.md)는
같은 MODEL 거부 동작을 독립적으로 검사한 기록입니다. 기존 제품 코드와 동작이
같으며, 아래 외부 LDS·진단 API 기록과 함께 이 브랜치에 보존합니다. 외부 기록의
검사 개수·바이너리 해시를 이번 설치 후보의 직접 검증 결과로 합산하지 않습니다.

외부 master `4cc7e95`의 [원본 LDS 합성 NMEA 경로 실행](../validation/LDS_PATH_VM_2026-10-01.md)에서는
같은 원본 커널·진단 initrd의 GPIO 되읽기 전달/보정 비교가 완주했습니다.
보정 조건의 원본 LDS는 합성 유효→무효→재획득 좌표를 mode 1→0→1로
응답했고, 전달 조건은 READ_NOT_READY·mode 0에 머물렀습니다. 사후
검증에서 첫 무효 전환 응답의 UTC만 직전 유효 주기에 남은 경계 혼합을
발견해 별도로 기록했습니다. 첫 fixture의 `strtok` 결함과 START 이전
NMEA 공급을 수정한 뒤 기본 QEMU의 여러 실행은 WFI 정지 또는 SM 조기
종료로 실패했습니다. 진단용 CPU 유휴 방지 루프를 추가한 동일 이미지
비교는 30/37회 질의와 A/B/C의 고유 공급 주기 대조까지 완주했으며,
첫 무효 응답의 같은 유형의 UTC 경계 혼합을 다시 보였습니다. 합성
수신·진단용 GPIO 보정·부분 SM과 바뀐 CPU 부하의 관측이며 실제
수신기/차량·폰이나 제품 ASSIST 검증이 아닙니다.

외부 master `3a1a547`의 [진단용 CPU 루프 제거 재실행](../validation/LDS_WFI_REPLAY_2026-10-01.md)에서는
같은 수정 fixture의 한 이미지로 `pass`/`mirror`를 각각 두 번 더
실행했습니다. 네 실행 모두 질의 3·25·9·6 이후 완료하지 못했고, 한
실행의 두 vCPU가 앞서 본 커널 WFI 경로에 있었습니다. 질의 중단 시점이
달라 원인은 미분리입니다. 앞선 루프 포함 완주를 기본 QEMU의 재현성이나
제품·실차 검증으로 승격하지 않습니다. 후속 QMP도 두 vCPU의 WFI 지점을
반복 관찰했고, 단일 스레드 TCG만으로는 완주하지 못했습니다. 반면 원본
커널에 진단용 `nohlt` 인자를 준 동일 이미지 `pass`/`mirror`는 30/37개
질의와 완료 표식까지 진행했습니다. `mirror`의 mode 1·UTC 0 응답과
두 좌표/UTC 경계 혼합을 별도로 제외했습니다. `nohlt`는 CPU 부하와 idle
경로를 바꾸므로 WFI/인터럽트 원인 확정이나 ASSIST 자격 근거가 아닙니다.
후속 동일 VM trace의 진행 3초에는 GIC set/acknowledge가 1,286/1,172건,
질의 중단 뒤 8초에는 선택한 GIC 이벤트가 0건이었습니다. 마지막 timer
예약 상태를 보지 못해 가상 인터럽트가 멈춘 선행 원인은 미확정입니다.

외부 master `ad252cd`의 [LDS 진단값 출처 연결 검토](../validation/LDS_DIAGNOSTIC_PROVENANCE_REVIEW_2026-10-01.md)는
이 브랜치의 [원본 함수 정적 분석](../validation/LDS_DIAGNOSTIC_PROVENANCE_2026-10-01.md)을
입력 해시와 공개 D-Bus 출력 형식·별도 cache mutex에서 대조한 기록입니다.
`GetUbloxDiag`와 `GetPosition`에는 같은 생산 측정을 연결하는 ID가 없으므로
진단값을 사후 polling하여 위치 응답별 수신기·센서 자격으로 승격할 수
없습니다. 제품 live `provenance()`는 계속 false이고 ASSIST는 비활성입니다.
원본 갱신 순서와 경쟁 가능성은 정적 분석이며 새 경쟁 상황 실행 결과가
아닙니다. 생산자→위치 snapshot→실제 요청·응답 경계의 자격 연결은 미구현입니다.

[주차 중 상태 진단 수정](../validation/TRIAL_STATUS_REVIEW_2026-10-01.md)에서는
원시 센서 수집이 진행되지만 MODEL 계산이 멈춘 경우를 별도로 보이게
했습니다. 순정 위치·계산·MODEL 진단은 정보 항목이고 상태 명령의 종료
코드는 원시 수집 근거만 판정합니다. 누적 queue `events`를 입력 처리 완료로
세거나 거부 입력의 worker 확인 시각을 센서 수신 시각으로 쓰지 않습니다.
거부 입력만으로 세 센서 수집 성공을 선언하지 않으며, 미래 시각으로
거부된 입력이 나중에 상태 검사 기준의 과거가 되어도 성공으로 세지 않습니다.
조용한 기록 중단 뒤 오래된 정상 행을 현재 동작으로 보지 않도록
health 5초·collector poll 8초·MODEL 진단 2초로 판정 창을 줄였습니다.
원본 ARM BusyBox 설치·상태·회수 에뮬레이션은 합성 로그 시험이며 실차
자료나 위치 정확도 검증은 아닙니다.

[기본 MODEL GPS 기준점 회귀 수정](../validation/GPS_REJECTION_2026-09-30.md)에서는
GPS 쌍 검사 실패 뒤 이전 READY 기준점이 다음 단절 때 되살아나는 결함을
재현·수정했습니다. 현재 배포 runtime의 GPS/wheel 검사 활성 경로에는
해당하지 않으며, 새 공개 ZIP 또는 실차 검증 결과가 아닙니다.

## SHADOW 기능 변경

추가 독립 감사에서 헤더 변경 누락, ZIP 빌드 출처 혼합, collector 종료 오판,
정차 GPS의 heading 상실, 보정 수신 시각과 단일 휠 모순 처리를 수정했습니다.
[새 오프라인 감사 기록](../validation/OFFLINE_AUDIT_2026-09-29.md)과
[v1.0 완료 조건](V1_READINESS_KO.md)을 함께 읽으십시오.
실제 SM의 명시적 재시작 및 지연 종료 정책은
[SM 실행 기록](../validation/SM_RETRY_2026-09-29.md)에 별도로 정리했습니다.
정상 전체 차량 기동·물리 재부팅·휴대폰 수용은 아직 검증하지 못했습니다.

순정 커널·OEM 서비스 실행 검증과 계정 수정은
[새 OEM 실행 기록](../validation/OEM_RUNTIME_2026-09-29.md)을 우선한다.
기존 설치 검증의 `cmu` 비특권 계정 가정은 틀렸다. 공식 passwd update는
`cmu=UID 0`, `service=UID 1001`이며 collector와 로그 소유권을 이에 맞췄다.
이전 로컬 USB ZIP도 이 수정이 없어 교체 대상이다.
순정 커널 및 일부 OEM 서비스 실행은 확인했지만 정상 전체 기동·차량 센서·
폰 수용 검증은 완료하지 못했다. ASSIST는 계속 꺼져 있다.

USB 설치 경로를 수정했다. 기존 `v0.3.0-shadow.1`의 MP3 진입 파일 누락,
해시 도구·계정 이름·저장소 링크·마운트·구형 awk 가정을 해결한다.
[현재 USB 사용 안내](../packaging/USB_START_KO.md)와
[새 검증 기록](../validation/USB_INSTALL_2026-09-29.md)을 따른다.
기존 공개 ZIP을 소급 변경하거나 실제 차량 검증을 완료한 것은 아니다.

후속 [GPS 기준점 검사와 제한된 휠 거리 보정](GPS_WHEEL_MODEL_KO.md)을 추가한다. GPS-visible 구간에서만 보정 근거를 모으고, 비교 구간에서는 적용값을 고정한다. 기존 모델 구조 안의 변경이며 설치 기본값·ASSIST 자격·배포 릴리즈를 바꾸지 않는다.

기존 VBS VIMC 콜백의 센서 입력을 복사하여 실제 SHADOW 항법을 계산하는 경로를 추가했다. [설계·소스 계약·남은 조건](LIVE_SHADOW_2026-09-29_KO.md), [검증 기록](../validation/LIVE_SHADOW_2026-09-29.md)이 이 변경의 기준이다. 아래 OBSERVE 시험 절차는 이미 발행된 v0.2.0-observe.2의 범위이며, 새 SHADOW 변경을 차량에서 확인했다는 뜻이 아니다.

정차 자이로 영점의 자동 추정·새 GPS 기준점에서의 적용과, 별도 GPS 제외 구간의 센서 DR 비교를 추가했다. [상세 계약](SHADOW_CALIBRATION_KO.md), [새 검증 기록](../validation/SHADOW_CALIBRATION_2026-09-29.md)을 따른다. 두 기능은 MODEL 계산이며 ASSIST 자격을 만들지 않는다.

## 목표와 범위

2019 MX-5 ND2 6MT, 1세대 Mazda Connect NA 74.00.324A에서 Android Auto 네이버 지도가 터널에서도 차량 속도·회전에 맞춰 움직이도록 하는 것이 최종 목표다. 대상 사용 환경은 기존 OEM AA touch preload, Galaxy S25, 무선 AA 동글이다. 운전 중 CMU 조작을 요구하지 않는다. 로그는 자동 수집하고 주차 후 회수한다.

실차 접근은 한두 번뿐일 수도 있는 제약이다. 새 시험 계획은 [통합 시험 준비](FIELD_TRIAL_KO.md)를 우선한다. 별도 OBSERVE/SHADOW 방문을 기본 전제로 두지 않고, 한 번의 준비된 SHADOW 시험에서 순정 관측·raw 수집·계산·주차 중 종료와 회수를 묶는다. 이 계획/소스 변경은 기존 공개 릴리즈를 바꾸거나 현재 차량 설치를 승인하지 않는다.

현재 코어의 개발 범위는 짧은 GNSS 단절이다. 기본 제한은 60초·1,500m·추정 오차 100m이며 실제 정확도 보증이 아니다. 지도 매칭과 장터널 대응은 구현하지 않았다. **speed-only 직진 DR은 허용하지 않는다.** 유효하고 신선한 yaw를 증명하지 못하면 자체 DR 송신을 허용하지 않는다.

## 설치 판정

**주차 상태의 첫 OBSERVE 시험을 위한 패키지와 절차를 준비했다. 실차 검증은 아직 수행하지 않았다.** 터널 DR 완성판이나 상시 설치 승인으로 해석하지 않는다.

- OFF/무효 설정/disable은 추가 로딩 전에 판단한다. 추가 NOW 실패 시 원 flags 1회 재시도 후 후크를 포기한다. 동시·재진입·NOLOAD 경쟁도 검사한다.
- D-Bus/SMDB 폴링은 별도 collector로 옮겼다. AA 안에는 후크·queue·writer만 남고 D-Bus 링크/자식 생성은 없다.
- `/usr/bin/autostart`의 SM 실행 직전 외부 가드가 시험 권한을 소비·동기화한 후 임시 설정을 반환한다. 영구 SM 설정에는 우리 preload를 남기지 않는다. 성공 여부와 관계없이 자동 재예약은 없다.
- 보장 경계는 **다음 가드 경유 기동**이다. 같은 실행 중인 SM의 내부 재시도와 파일시스템 고장까지 해결했다고 주장하지 않는다.

[첫 시험 절차](FIRST_TRIAL_KO.md), [복구 설계](RECOVERY_2026-09-28.md), [통합 검증](../validation/INTEGRATION_2026-09-28.md)을 함께 읽는다. 첫 실제 부팅·기존 touch 공존·collector 버스 권한·다음 부팅 복귀·폰 수용은 남은 실차 확인이다. 운전 중 조작은 없다.

## 이미 확인한 것과 확인하지 못한 것

| 항목 | 현재 근거와 한계 |
| --- | --- |
| 차량 위치 → AA | 해당 펌웨어 정적 분석과 원본 manager의 자동 LDS→native send VM 실행. 물리 센서·실제 휴대폰 수용은 미검증 |
| 순정 DR | NNG의 mode=3 출력과 AA 통과 경로 존재. SD·프로파일·센서·지역 밖 조건에서 활성화되는지는 별도 문제 |
| mode=0 캐시 | 원본 자동 경로 VM에서 이전 좌표·속도·방향의 재송신을 확인. OBSERVE/SCRUB은 native 구조체의 기존 timestamp를 보존. 이후 protocol wire timestamp·미활성 ASSIST의 DERIVED 시각은 별도 검증 대상 |
| 상위 후크 → send | 해당 바이너리에서 RequestSendPosition → OrderSendVehicleData → 하위 send 동기 호출 근거 확보. LDS callback→worker 큐 경계를 TLS가 넘는다는 뜻 아님 |
| SMDB yaw | 평균값만 보존되고 원래 count/timestamp 전달이 손실됨. poll 시각으로 생산 시각을 대체할 수 없음 |
| 센서 보정 | yaw 부호·bias, 속도 품질, 후진·정지·지연 계약 미검증 |
| 앱 지원 | 다른 차량의 긍정/부정 후기는 존재. 이 차량·폰·앱 버전의 DR 수용을 입증하지 않음 |
| fallback | 차량 LOCATION이 끊겨야만 폰 GPS로 전환한다거나 DROP이면 반드시 전환한다는 정책은 확정하지 못함 |

원본 해시·오프셋·필드 계약은 [구현 설명](IMPLEMENTATION_REVIEW_KO.md)과 [과거 설계](archive/DESIGN_V1_KO.md)에 있다. 원본 파일이 필요한 정적 분석을 공개 소스만으로 다시 수행했다고 주장하지 않는다.

## 구현된 것

- `src/core/`: 외부 I/O 없는 C99 DR 상태 기계. 품질·시간·후진·재획득·오차 제한과 합성 리플레이.
- `src/adapter/`: 상위 ARM veneer/TLS와 하위 GOT 후크. 원본 입력 복사, LOCATION 선택, 정확히 한 번 원본 send 호출.
- `src/runtime/`: 설정 우선 dlopen 경계 설치, bounded queue/journal. 별도 `src/collector/`가 관찰용 폴링을 맡는다.
- `src/sensors/`, `src/navigation/`: 기존 VBS 콜백의 원본 센서 복사, 비차단 datagram, 시간 정렬과 MODEL/qualified 공통 파이프라인. 새 구독자·차량 명령·TCP7035 접속 없음.
- `packaging/`: 펌웨어 해시 검사, 기존 touch 보존, 영구 preload 없는 일회성 기동 가드, 명시적 arm·제거. 상시 설치 승인 아님.
- `tools/analyze_logs.py`: 실제 이벤트·health·drop 등 분석. `audit_fault`를 이미 불완전 실험으로 판정함.

SCRUB은 mode=0 캐시 LOCATION에서 `hasSpeed=false`, `hasBearing=false`로 만들고 값도 0으로 지운다. 유효한 speed=0을 제공하는 것과 다르다. 오래된 좌표는 계속 남는다. SHADOW는 이번 Draft에서 실제 센서 수신과 MODEL-domain DR 계산을 수행한다. 계산 위치와 LOCATION 미리보기만 기록하며 순정 송신을 유지한다. ASSIST는 `allow_assist=false`와 항상 false인 provenance 검사 등으로 차단되어 있다.

## 다음 작업과 종료 경로

OFF/로더, collector 분리, 외부 가드는 코드와 호스트/합성 ARM 검증이 완료됐다. 상세 근거는 [로더](../validation/LOADER_FIX_2026-09-28.md), [collector](COLLECTOR_2026-09-28.md), [통합 검증](../validation/INTEGRATION_2026-09-28.md)에 있다.

| 남은 확인 | 완료 조건 |
| --- | --- |
| 통합 시험의 주차 확인 | AA/touch 시작, 정상 hook/health와 원본 수신, collector 버스 권한과 별도 로그, 다음 부팅 baseline 복귀. SHADOW 방문에 묶고 별도 OBSERVE 방문을 기본 요구하지 않음 |
| 순정 DR/폰 수용 | native provider의 mode·위치와 폰/앱 로그를 같은 부팅·세션에서 비교. send 성공으로 폰 수용을 대체하지 않음 |
| SCRUB/DROP 비교 | 실제 변형 이벤트·audit 상태로 유효 구간 판정. DROP은 caller/반환값/상태 부작용 분석 후 별도 계약 결정 |
| 자체 ASSIST | 생산자 시각·품질·보정·후진·오차 한계 확보. poll receipt로 대체하지 않음 |

NNG의 순정 DR이 실제로 충분하면 자체 ASSIST보다 기존 경로 확인을 우선한다. SD 없는 구성에서 yaw가 없거나 생산 시각·품질 계약을 확보하지 못하면 **현재 SMDB 기반 ASSIST 경로는 폐기**한다. 정상 차량 LOCATION을 폰/앱이 쓰지 않는다면 CMU 좌표 생성만으로 최종 목표를 해결할 수 없다.

## 검증 기록의 해석

0.1의 호스트/ARM 합성 시험 기록은 [VALIDATION.md](VALIDATION.md)에 보존했다. 공개 이관 검사는 [PUBLIC_IMPORT.md](../validation/PUBLIC_IMPORT.md), 후속 수정 검사는 [INTEGRATION_2026-09-28.md](../validation/INTEGRATION_2026-09-28.md)에 따로 적는다. 그 당시 QEMU 검사는 합성 프로그램 실행이며 OEM 실행 또는 차량 시험이 아니다. 이후 실제 OEM 실행은 위의 2026-09-29 기록에 별도로 남겼다. 새 커밋의 검사 결과와 과거 결과를 섞지 않는다.

과거 검토에는 Astra 하위 에이전트가 참여했다. 당시에는 Claude 실행 경로가
없었다. 이후 USB 수정에서는 실제 Claude Code 실행과 독립 Codex 리뷰를
수행했다. 각 후속 검증 기록의 실행 범위를 따르며 차량 시험을 의미하지 않는다.
