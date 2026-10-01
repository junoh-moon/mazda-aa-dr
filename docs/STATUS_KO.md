# 현재 상태와 인계 — 2026-10-01

이 문서는 새 리뷰어·LLM의 첫 진입점이다. 과거 상세 설계와 설치 가능 판단보다 우선한다. 현재 USB는 SHADOW 시험 후보이며 OFF·폴링 분리·일회성 기동 보호를 구현했다. 호스트·ARM·부분 OEM 실행과 실제 차량 검증을 구분한다. 소스 커밋과 공개 릴리즈는 별개다.

**2026-10-01 사용자 변경 지시: v1.0 전에 실차 설치·시험 기회를 한 번 허용했습니다.**
이전 펌웨어 파일만 사용하는 제한을 해당 통합 시험에 한해 갱신합니다.
가능한 구현·원본 런타임·오프라인 검증을 먼저 끝내고, 주차 중 기동·복구 확인과
자동 원본 관측·원시 센서·SHADOW/GPS 제외 비교·회수를 한 번에 준비합니다.
반복 방문이나 폰/동글 탁상 시험의 추가 승인이 아닙니다. 설치 성공만으로
측정 준비를 판정하지 않으며 저장 공간과 실제 계산 시도·중단 사유를 확인합니다.
실차 센서·폰 수용·물리 복구는 아직 미검증이고 제품 ASSIST는 미완료입니다.
[통합 시험 준비](FIELD_TRIAL_KO.md)와 [v1.0 완료 조건](V1_READINESS_KO.md)을
따르십시오. 현재 공개 ZIP을 새 시험 준비가 끝난 묶음으로 간주하지 않습니다.

외부 master에서 [v0.3.3-shadow.1](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.3-shadow.1)을
커밋 `260528c751739c0afb59153f7a5cfa3585eb6048`에서 빌드하여 게시한 것을 확인했습니다.
해당 host/ARM·순정 BusyBox 설치 검사는 외부 발행 기록을 따릅니다. 이번 작업은
공개 ZIP을 직접 내려받아 SHA-256·CRC·전체 manifest와 고정 커밋을 대조했습니다.
[이번 릴리즈 검증](../validation/RELEASE_V033_2026-10-01.md)에 파일 해시와 실행 범위를
고정했습니다. 기본 모드는 SHADOW이며 ASSIST는 비활성입니다. 실차 센서 callback,
위치 정확도, 정상 전체 차량 기동·복구와 폰 수용은 미검증입니다. 조사 문단의
`공개 ZIP 미갱신`은 해당 시점의 기록이며 포함 여부는 고정 커밋으로 대조하십시오.

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
