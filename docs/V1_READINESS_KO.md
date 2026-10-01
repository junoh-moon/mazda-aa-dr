# v1.0 완료 조건과 작업 목록

최종 목표는 NA 74.00.324A의 2019 MX-5 ND2 6MT에서 기존 AA 터치·km/L
구성을 보존하면서, Galaxy S25와 무선 AA 동글로 연결한 네이버 지도가 GNSS
단절 중에도 차량의 이동·회전을 반영하도록 하는 것입니다. 설치 성공이나
SHADOW 로그 생성만으로 이 목표를 완료한 것으로 보지 않습니다.

2026-09-29 시작 기준은 `e1f7ea2624317ba630c4a0077e561ab8a998d587`입니다.
사용자는 중간 커밋, master push, v0.x 릴리즈 게시를 승인했습니다. 아래 작업과
필요한 증거가 남아 있는 동안 v1.0 완료를 선언하지 않습니다.

## 작업 목록

- [x] 공개판과 로컬 수정본의 차이, 실제 OEM 실행 근거를 재확인합니다.
- [x] 고정 커밋의 새 checkout에서 전체 host/ARM 검사를 다시 실행하고 skip을 확인합니다.
  e1f7ea2 기준선과 bcdfda9, af1a24a에 이어 260528c의 전체 검사도 생략 없이
  통과했습니다. [최신 릴리즈 검증](../validation/RELEASE_V033_2026-10-01.md)을
  따릅니다.
- [x] 기존 설치 오류를 고친 v0.x ZIP을 게시하고 게시된 파일을 다시 받아 검사합니다.
  v0.3.3-shadow.1의 SHA-256·CRC·원본 바이트 일치를 확인했습니다.
  [새 릴리즈 검증](../validation/RELEASE_V033_2026-10-01.md)을 따릅니다.
- [x] 센서·항법, OEM 호출/복구, USB/분석기의 독립 감사를 수행하고 확인된 결함을 수정합니다.
- [x] 실제 OEM SM의 명시적 재시작과 지연 종료 정책을 실행·정적 근거로 확인합니다.
  정상 전체 기동과 물리 watchdog 재부팅은 아래 별도 조건으로 남깁니다.
- [ ] 실제 센서 callback부터 계산까지의 통합 경로와 입력 단위·품질·시간 근거를 확보합니다.
  [원본 callback 실행](../validation/VIM_CALLBACK_2026-09-30.md)에서 합성 세 건의
  원본 등록·MQ·callback·AA 수신은 확인했습니다. 물리 센서와 유효 위치 계산은 남습니다.
  [후속 센서→제품 MODEL 통합](../validation/SHADOW_RUNTIME_2026-09-30.md)에서는
  합성 raw 916건의 실제 전달, 정차 영점 적용·직진/회전·yaw 단절과 재기준점
  복구를 완료하고 계산을 독립식과 대조했습니다. 센서 수신 대기를 개선했지만
  같은 최종 제품의 다른 실행에서 요청 관측 잠금 경합이 발생했습니다.
  물리 센서·위치 정확도와 이 경합의 원인 분리는 남아 있어 상위 항목은 미완료입니다.
  [요청 잠금 분리 후속 검사](../validation/REQUEST_CONTENTION_2026-09-30.md)에서
  작성 코드의 상호 간섭을 수정하고 원본 VM 두 실행의 요청 loss 0을 확인했습니다.
  과거 실패의 정확한 호출 조합은 재현되지 않았고, 새 실행의 별도 journal drop도
  미해결입니다. 후속 성공 실행의 합성 raw 927건·계산 일치를 전체 안정성으로 세지 않습니다.
  [Journal 큐 후속 검사](../validation/JOURNAL_QUEUE_2026-09-30.md)에서 정상 입력·배출
  잠금 경합을 제거하고, 미완성 예약의 조기 완료와 전역 초기화 경계를 보강했습니다.
  독립 ARM 검토에서 찾은 64비트 load 후행 장벽 문제도 수정했습니다.
  최종 원본 VM 두 실행(raw 936·855건)은 journal/요청 loss 0과 합성식 대조를
  완료했습니다. 과거 drop의 정확한 원인과 물리 입력 자격은 확정하지 않았습니다.
- [ ] 실제 요청의 provider/receiver/session 출처를 보존하는 ASSIST 연결을 구현·검사합니다.
  [버스 연결 관측 검토](../validation/BUS_CONNECTION_REVIEW_2026-09-30.md)에서
  실제 발행/응답 연결 수명과 daemon 종료의 단절 관측을 제품에 연결했습니다.
  세 독립 리뷰와 host/ARM 전체 검사 뒤 원본 VM의 LDS 요청 30건, 단절 뒤
  generation 철회와 bus fault 0을 확인했습니다. 관측 ID는 daemon/provider
  인증이나 receiver 자격이 아닙니다.
  [후속 MODEL 버스 경계 검토](../validation/MODEL_BUS_REVIEW_2026-09-30.md)에서
  버스 reset을 구현하고 동시 출처 표시·분석기·실제 수신 시각 결함을 수정했습니다.
  최종 제품의 원본 VM에서 LDS 응답 42건, 세션을 유지한 복수 연결·해제와
  raw 보존을 확인했습니다. 유효 원본 GPS나 qualified 입력·폰 수용은 아니므로
  이 상위 항목은 미완료입니다. 외부 SM·SYSTEM·USB 기동 조사도 따로 추적합니다.
  [원본 커널 GPIO 되읽기 진단](../validation/LDS_GPIO_VM_2026-10-01.md)에서는
  격리 VM의 USB 출력 latch 1과 PSR/sysfs 0을 직접 확인했습니다. 원본 LDS의
  legacy 선택을 가로막는 에뮬레이터 조건을 좁혔지만, 당시에는 원본 LDS의
  유효 NMEA·GetPosition과 실제 하드웨어·receiver 자격을 검증하지 못했습니다.
  [후속 원본 LDS 합성 NMEA 실행](../validation/LDS_PATH_VM_2026-10-01.md)은
  진단용 GPIO 되읽기 보정에서 원본 GetPosition의 유효→무효→재획득을
  확인했습니다. 첫 무효 응답의 UTC·좌표 경계 혼합과 후속 fixture의 WFI
  정지를 보존합니다. 수정 fixture는 진단용 CPU 유휴 방지 조건에서
  START 이후 공급과 고유 주기 대조까지 완주했습니다. 합성 위치·부분
  SM·진단 interposer와 바뀐 CPU 부하의 근거이며 물리 수신기나 제품
  자격·폰 수용은 아닙니다.
  [CPU 루프 제거 재실행](../validation/LDS_WFI_REPLAY_2026-10-01.md)의
  같은 이미지 네 번은 모두 질의 도중 완료하지 못했습니다. 한 번의 두
  vCPU는 커널 WFI 경로에 있었고 나머지 정지 원인은 미분리입니다.
  기본 QEMU에서 이 fixture의 안정적 완주는 아직 입증하지 못했습니다.
  같은 원본 커널·진단 이미지에만 `nohlt`를 추가한 후속 비교는
  30/37개 질의를 완료했지만, mode 1·UTC 0과 전환 경계의 서로 다른
  NMEA 주기 혼합을 발견했습니다. [세부 검증](../validation/LDS_WFI_REPLAY_2026-10-01.md)은
  이를 자격 없는 응답으로 분리합니다. idle/IRQ 정지 원인은 미확정입니다.
  QMP trace에서 진행 중 GIC 이벤트와 중단 뒤 8초간 0건을 대조했지만,
  마지막 timer 예약과 물리 하드웨어 경로는 확인하지 못했습니다.
  [후속 타이머 인과 계측](../validation/LDS_TIMER_CAUSALITY_2026-10-01.md)은
  그 당시의 미확인 범위를 좁혔습니다. 원본 진단 VM에서 가상 시계가
  계속 흐르는 동안 두 `local_timer`에 제어값 0이 쓰이고 IRQ가 멈췄습니다.
  guest timer 목록은 `mxc_timer1` broadcast와 CPU별 `local_timer`를
  확인했고, 계측 부하가 큰 별도 실행에서는 이미 지나간 GPT 비교값을
  QEMU가 가까운 만료로 선택하지 않는 장면도 포착했습니다. 마지막
  guest deadline과 QEMU 계산의 원인 관계, 기본 VM의 안정적 완주,
  차량 경로는 여전히 미확정입니다.
  파일 trace를 끈 후속 실행에서는 guest의 직전 TCN 읽기보다 254 tick
  앞선 GPT 비교값이 QEMU 계산 시 이미 295 tick 지난 상태였습니다.
  별도 실행에서는 325 tick 뒤였고, 마지막 비교값 쓰기 뒤 같은
  스레드의 TCN 재읽기는 제한 종료까지 미관측이었습니다. TCG 스케줄링
  지연과 정확한 guest 재시도·clockevent 결과는 아직 분리하지 못했습니다.
  [추가 타이머·IRQ 대조](../validation/LDS_TIMER_IRQ_FOLLOWUP_2026-10-01.md)는
  단일 TCG 스레드에서도 42 tick 늦은 GPT 비교값과 출력선 low를
  관찰했습니다. 다른 실행의 정시 GPT callback은 제한 종료 직전이라
  그 뒤 동작을 판정할 수 없습니다. GIC 전달·CPU 수락을 포함한
  같은 실행의 원인 경계는 미확정입니다.
  [사건 동기화 후속 계측](../validation/LDS_TIMER_WAKE_BOUNDARY_2026-10-01.md)은
  늦은 GPT 비교값 뒤 가상 시계 증가, 두 vCPU의 halted 상태와
  새 GPT·GIC IRQ 87 요청 부재를 같은 VM에서 기록했습니다. OEM
  쓰기 후 검사·재시도와 차량 하드웨어 동작은 여전히 미검증입니다.
  [LDS 진단값 출처 연결 검토](../validation/LDS_DIAGNOSTIC_PROVENANCE_REVIEW_2026-10-01.md)에서
  원본 `GetPosition`과 `GetUbloxDiag`의 출력 형식·별도 cache mutex를
  직접 대조했습니다. 두 응답을 같은 생산 측정으로 묶는 ID가 없어 진단
  polling으로 요청별 자격을 만들 수 없습니다. 원본 갱신 순서의 근거는
  [외부 정적 분석](../validation/LDS_DIAGNOSTIC_PROVENANCE_2026-10-01.md)이며
  제품의 생산자→snapshot→요청·응답 연결은 미구현입니다.
  [세션 후보 철회·MODEL 초기화 검토](../validation/MODEL_SESSION_REVIEW_2026-09-30.md)에서
  이전 세션의 기준점·학습 보정·대기 입력 잔류를 제거하고 원시 입력을 보존했습니다.
  원본 VM의 두 조건에서 각각 지연 요청 네 건을 새 MODEL에서 제외했고, 재시작
  조건의 원본 LOCATION 네 건은 그대로 전달됐습니다. 이는 음성 조건의 근거이며
  qualified 출처·정상 폰 연결·유효한 원본 위치 계산을 구현했다는 뜻은 아닙니다.
  [요청 경로 복사·저장 검토](../validation/REQUEST_ROUTE_REVIEW_2026-09-30.md)에서
  외부 변경을 통합하고 네 issue-time 문자열을 journal까지 연결했습니다.
  세 독립 리뷰의 파서/회귀 결함 수정, host/ARM 검사와 직접 원본 VM의 요청
  30건 대조를 완료했습니다. 이는 route 이름 관측이며 bus 수명·실제 provider
  소유권이나 receiver/session 자격을 구현한 결과는 아닙니다.
  최신 [세션 관측 수정·실행](../validation/SESSION_CONTEXT_REVIEW_2026-09-30.md)에서
  issue 당시 문맥과 send 저장소를 연결하고 동시성·판정 결함을 수정했습니다.
  원본 세션 재생성 뒤 이전 요청 네 건이 새 세션으로 송신되는 경우도 구별했습니다.
  관측된 API 수명은 요청 소유권이나 폰 수용을 증명하지 않으므로 이 항목은 미완료입니다.
  아래는 조사 순서에 따른 이력입니다. 요청 관측의 제품 설치·journal 연결은
  완료했으며, 최신 결과는 이 항목 마지막의 제품 연결 기록을 따릅니다.
  [원본 위치 객체 ABI](../validation/REQUEST_PROVENANCE_2026-09-30.md)를 실행했으나
  큐 lifetime·실제 요청 연결·qualified runtime 구현의 완료로 세지 않습니다.
  [별도 요청 관측 자료구조](../validation/REQUEST_TRACE_2026-09-30.md)와 합성 회귀를
  추가했습니다. 제품 연결과 실제 수신기·세션 자격은 여전히 미구현입니다.
  [원본 LDS 비동기 요청 실행](../validation/LDS_ASYNC_2026-09-30.md)으로 요청→reply
  sender→callback→정상 정리의 실제 identity 경계는 확인했지만, BLM 이후 큐와
  취소·timeout 정리, 제품 hook은 남아 있습니다.
  [별도 다중 요청·오류 응답 시험](../validation/LDS_CLIENT_API_2026-09-30.md)에서
  64건의 userdata 대응과 버스·제공자 부재를 검사했습니다. data client API
  직접 호출이며 제품의 util·worker·송신 경로 검증을 대신하지 않습니다.
  [후속 AA용 util·연결·지연 시험](../validation/LDS_CLIENT_LIFECYCLE_2026-09-30.md)은
  원본 AA용 공개 요청 함수까지 실행했습니다. 오류 정보 손실과 뒤늦은 응답을
  확인했고 동일 객체 재연결 실패는 미해결입니다.
  [후속 원본 BLM 큐 시험](../validation/BLM_QUEUE_2026-09-30.md)에서는 실제 LDS
  AA용 API→원본 callback→큐→위치 worker→RequestSendPosition과 OBSERVE 후크를
  실행했습니다. 정상 버스 요청/reply와 제공자 종료 뒤 0값 전달을 대조했습니다.
  AA 세션이 없어 send는 0건이며, 요청별 자격의 제품 연결·송신·폰 수용은 남습니다.
  [후속 로컬 AA 세션 시험](../validation/AA_SESSION_2026-09-30.md)은 원본
  생성·파괴와 별도 송신 API의 256/0/256 반환 및 OBSERVE 전달을 확인했습니다.
  send 0은 폰 수용이 아니며, 실제 LDS부터 송신까지의 활성 경로·원본 정리
  오류의 영향·요청 자격의 제품 연결은 여전히 남아 있습니다.
  [후속 원본 AA manager 시험](../validation/AA_PIPELINE_2026-09-30.md)에서는
  원본 시작 API의 자동 LDS 요청→큐→위치→실제 send를 실행하고 native
  OBSERVE/SCRUB 전달을 대조했습니다. 정상 폰 연결 사건·폰 수용, 오류 없는
  정리와 실제 요청 identity의 제품 연결은 남아 있어 이 항목을 완료하지 않습니다.
  [후크 없는 원본 정리 비교](../validation/AA_CLEANUP_BASELINE_2026-09-30.md)에서는
  같은 mutex 오류가 제품 후크 없이도 발생함을 확인했습니다. 세션 재생성·task
  복귀의 근거이며 오류 없는 전체 종료나 누수 부재의 증명은 아닙니다.
  [실제 요청 관측 연결](../validation/REQUEST_LINK_2026-09-30.md)은 원본
  요청/reply와 worker를 연결하고 위치/send까지 metadata를 보존한 비공개
  fixture 검사입니다. 제품 cold-install·journal, 실제 취소/예외 경계와
  receiver/session·센서 자격이 남아 있어 ASSIST 연결 완료로 세지 않습니다.
  [실제 요청 종료 시험](../validation/REQUEST_LIFECYCLE_2026-09-30.md)에서는
  free/disconnect의 취소 12건과 새 연결의 지연 응답 네 건을 검사했습니다.
  timeout 만료·같은 이름 재연결·userdata 정리·제품 연결은 여전히 남습니다.
  [adapter 예외 정리 수정](../validation/ADAPTER_UNWIND_2026-09-30.md)에서는
  기존 ARM 후크의 합성 예외 abort를 고쳤고, 실제 제품 DSO와 원본 공유
  runtime의 작성 target에서 8개 예외/취소 사례를 통과했습니다. OEM 호출
  체인 전체와 요청 후크의 예외 경계 완료로 세지 않습니다.
  [요청 관측의 제품 설치·journal 연결](../validation/REQUEST_PRODUCT_2026-09-30.md)을
  구현·실행했습니다. 실제 제품 DSO에서 정상/제공자 부재의 24개 요청과
  위치/send metadata, 축소된 8개 취소의 슬롯 회수를 검사했습니다. 전체 manager
  이후 반복 취소·재개 timeout은 원인 미분리입니다. 최종 파이프라인 시험의
  명시적 제외와 실패 기록을 유지하며 이 상위 ASSIST 항목은 완료하지 않습니다.
  [후속 전체 취소·반복 비교](../validation/MANAGER_CANCELLATION_2026-09-30.md)에서는
  동일 제품으로 전체 경로와 취소 12건·지연 응답 4건을 완료했습니다. manager를
  실행한 후크 유무 비교도 각각 요청 80건·free/resume 20회와 전후 API 응답을
  확인했습니다. 앞선 timeout의 재현·원인 분리는 여전히 남습니다.
  [독립 세션 진단 통합·재실행](../validation/SESSION_DIAGNOSTIC_REVIEW_2026-09-30.md)에서는
  원본 생성·송신·시작의 0 반환과 INVALID callback을 두 VM에서 직접 대조하고
  진단 판정기를 보강했습니다. 정상 폰 연결이나 제품 요청에 연결된 세션 자격의
  구현으로 세지 않으며 이 항목은 미완료입니다.
- [x] 기존 MODEL 로그의 valid/state/result/좌표 의미 모순을 재현·수정했습니다.
  [생산자 대조·독립 검토](../validation/MODEL_RESULT_REVIEW_2026-09-30.md)에서
  실제 경계 출력, host Python 330개와 C/C++, 고정 ARM journal 범위를 확인했습니다.
  기존 host/ARM/원본 VM 로그 51개의 전체 보고서는 변경되지 않았습니다.
- [x] generation 소진 뒤 fault가 기존 MODEL 유효성을 다시 노출하고,
  위치 순번 소진 시 큐 범위를 벗어나는 Pipeline 결함을 수정했습니다.
  [공개 API 재현·음성 대조·host/ARM 검사](../validation/PIPELINE_EXHAUSTION_2026-09-30.md)를
  확인했습니다. 극단적 순번의 합성 시험이며 실차 검증은 아닙니다.
- [x] 원본 VIP의 16비트 요레이트 합계 넘침을 작은 정상 평균으로 수락하던
  MODEL 결함을 수정했습니다. [원본 생산자 조사](../validation/VIP_ACCUMULATOR_2026-10-01.md)의
  입력 해시를 직접 확인하고, [host/ARM 통합 검사](../validation/VIP_ACCUMULATOR_INTEGRATION_2026-10-01.md)를
  완료했습니다. count 자체의 넘침·손실 창·생산 시각·물리 센서 품질은
  이 경계식으로 복원되지 않으며 ASSIST 자격으로 세지 않습니다.
- [ ] 지원 범위의 위치 정확도와 Galaxy S25/무선 AA/네이버 지도 수용을 검증합니다.
- [ ] 정상 전원 주기·실패 복구·기존 터치/km/L 공존의 실제 결과를 확인합니다.
- [ ] 최종 커밋과 게시 ZIP을 고정하고 아래 조건 전체를 다시 감사합니다.

## 요구사항과 증거

| 요구사항 | 현재 근거 | 부족한 구현 또는 증거 |
| --- | --- | --- |
| 깨끗한 USB에 압축 해제 후 `sh install.sh` | MP3/JS·정적 해시 도구 포함. 실제 ARM BusyBox/libc에서 최종 ZIP 설치·제거·재설치 성공 | 실제 CMU 미디어의 MP3→shell 동작, 실제 저장소의 remount·내구성은 미검증 |
| 정확한 펌웨어·계정·경로 | 네 원본 identity 유지. `cmu=0`, `service=1001`, 순정 저장소 symlink 반영 | 다른 펌웨어에 일반화하지 않음. 현장 설치 정보와 대조 필요 |
| OEM 호출 계약·터치 공존 | ARM veneer/encoder 합성 시험. 순정 커널에서 AA 후크와 터치 DSO 동시 로드. 원본 manager의 자동 LDS→native send와 OBSERVE/SCRUB 실행 | 정상 폰 연결 상태의 AA 송신·터치 입력, 수명·동시성·지연 장애의 전 범위 미검증 |
| 자동 수집·원본 증거 보존 | 별도 collector의 실제 UID 전환·SMDB 응답. 원본 VBS callback과 AA 수신을 합성 입력으로 실행. journal·종료·회수 회귀, [주차 중 수집/계산 분리 진단](../validation/TRIAL_STATUS_REVIEW_2026-10-01.md), [MODEL 초기화 원인 기록](../validation/MODEL_RESET_REVIEW_2026-10-01.md) | 물리 센서 callback과 주행 전체의 누락·부하·로그 보관량 미검증. 상태 명령의 종료 코드 0은 항법 계산·폰 수용이 아님 |
| 물리 센서 해석 | 원본 펌웨어의 callback ABI/필드 정적 분석과 합성 parser 시험. VIP 요레이트 생산자의 선택 경로 해석 실행과 MODEL 합계 넘침 거부 | 휠/yaw 부호·단위·bias·6MT 후진·cadence 및 품질의 실제 대조 없음. count 넘침과 손실 창도 식별 불가 |
| 적분 시간·신선도 | receipt와 producer 시각 분리, MODEL이 qualified로 승격되지 않는 검사 | 현재 IPC payload에는 생산자 시각/순번이 없음. 검증된 지연 상한 등 대체 근거도 없음 |
| 위치 계산·재획득 | 코어·정차 보정·GPS holdout·wheel 보정 합성 시험 | 실제 경로·독립 기준 위치 비교 없음. GPS holdout 차이를 ground truth로 세지 않음 |
| 요청 출처·ASSIST 실행 | adapter의 출처/epoch 검사와 qualified 파이프라인은 합성 입력으로 검사 | live `provenance()`는 항상 false, `allow_assist=false`. 요청 관측 연계는 제품에 연결했으나 qualified 자격 입력은 미구현. 전체 manager 이후 반복 취소 timeout 원인 미분리 |
| 휴대폰·앱 수용 | OEM LOCATION 경로의 정적 근거와 VM의 실제 native API 호출. 폰 없이도 send=0을 반환함 | Galaxy S25·동글·네이버 지도에서 위치가 반영되는 실행 증거 없음 |
| 다음 부팅과 장애 복구 | 일회 소비 가드·설치 중단 회귀. 실제 SM에서 명시적 재시작과 지연 SIGKILL 뒤 보드 재부팅 요청 관찰 | 다른 실패 경로와 물리 watchdog·전원 차단·다음 부팅의 복구는 미검증 |
| 기존 설정 보존 | touch 설정 편집/제거 roundtrip, 무관한 파일을 변경하지 않는 설치기 | 기존 터치와 km/L의 실제 화면·입력 결과 미검증 |
| 재현 가능한 릴리즈 | 고정 GCC 4.9.1과 다섯 ARM 바이너리, ZIP manifest/source commit. v0.3.3-shadow.1 공개 다운로드 재검사 완료 | v1.0의 최종 커밋·ZIP과 전체 조건 감사 필요 |

독립 감사에서 정차 GPS·지연 수신 시 heading 상실, 자이로 보정의 수신 시각,
단일 휠 모순과 빌드/검사의 오래된 입력·상속 환경 문제를 재현하고 수정했습니다.
처음부터 지연된 transport MODEL 입력의 새 GPS anchor 실패도 별도 수정하여
독립 native와 pinned ARM에서 검증했습니다. 현재 live receipt 입력을 생산자
시각으로 승격하지 않습니다.

## 판정 방법

실패를 재현하는 검사를 먼저 만들고 수정 후 같은 검사로 확인합니다. host fixture,
합성 ARM 실행, 순정 OEM 실행, 실제 차량·휴대폰 검사는 각각 구분하여 기록합니다.
원본 호출을 성공 stub으로 바꾸거나 장치 응답을 합성한 검사는 물리 장치 성공으로
계산하지 않습니다. OEM 원본·전체 로그·자격증명·개인 공유 링크는 게시하지 않습니다.

순정 커널 VM에서 VIM 초기화·AA 후크와 원본 LDS의 mode=0 위치 응답을
확인했습니다. 실제 SM 부분 그래프에서도 LDS는 시작했지만 VBS의 CAN 준비
timeout으로 jcinavi·jciAAPA는 시작되지 않았습니다. 별도 aap_service는
RUNNING이었습니다. 전체 그래프 baseline의 GUI
크래시도 해결된 것으로 세지 않습니다. [위치 서비스 실행 기록](../validation/OEM_LOCATION_2026-09-30.md)의
부분 진행과 정상 전체 기동을 구별하며 timeout·프로세스 생존·health만으로
성공을 선언하지 않습니다.

ASSIST의 미구현 연결은 실제 근거를 확보하여 구현해야 합니다. 상수 false를 true로
바꾸거나, receipt 시각과 MODEL 보정을 검증된 센서 입력으로 바꾸어 종료하지 않습니다.
오프라인에서 가능한 수정과 검증을 먼저 마치며, 실차 접근이 필요해지면 기존 증거로
답할 수 없는 질문과 한 번에 수집할 증거를 [통합 시험 계획](FIELD_TRIAL_KO.md)에
모읍니다. 운전 중 CMU 조작은 요구하지 않습니다.
