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
  e1f7ea2 기준선에 이어 최종 bcdfda9의 전체 검사도 생략 없이 통과했습니다.
  [릴리즈 검증 기록](../validation/RELEASE_2026-09-30.md)을 따릅니다.
- [x] 기존 설치 오류를 고친 v0.x ZIP을 게시하고 게시된 파일을 다시 받아 검사합니다.
  v0.3.1-shadow.1의 SHA-256·CRC·원본 바이트 일치를 확인했습니다.
- [x] 센서·항법, OEM 호출/복구, USB/분석기의 독립 감사를 수행하고 확인된 결함을 수정합니다.
- [x] 실제 OEM SM의 명시적 재시작과 지연 종료 정책을 실행·정적 근거로 확인합니다.
  정상 전체 기동과 물리 watchdog 재부팅은 아래 별도 조건으로 남깁니다.
- [ ] 실제 센서 callback부터 계산까지의 통합 경로와 입력 단위·품질·시간 근거를 확보합니다.
  [원본 callback 실행](../validation/VIM_CALLBACK_2026-09-30.md)에서 합성 세 건의
  원본 등록·MQ·callback·AA 수신은 확인했습니다. 물리 센서와 유효 위치 계산은 남습니다.
- [ ] 실제 요청의 provider/receiver/session 출처를 보존하는 ASSIST 연결을 구현·검사합니다.
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
- [ ] 지원 범위의 위치 정확도와 Galaxy S25/무선 AA/네이버 지도 수용을 검증합니다.
- [ ] 정상 전원 주기·실패 복구·기존 터치/km/L 공존의 실제 결과를 확인합니다.
- [ ] 최종 커밋과 게시 ZIP을 고정하고 아래 조건 전체를 다시 감사합니다.

## 요구사항과 증거

| 요구사항 | 현재 근거 | 부족한 구현 또는 증거 |
| --- | --- | --- |
| 깨끗한 USB에 압축 해제 후 `sh install.sh` | MP3/JS·정적 해시 도구 포함. 실제 ARM BusyBox/libc에서 최종 ZIP 설치·제거·재설치 성공 | 실제 CMU 미디어의 MP3→shell 동작, 실제 저장소의 remount·내구성은 미검증 |
| 정확한 펌웨어·계정·경로 | 네 원본 identity 유지. `cmu=0`, `service=1001`, 순정 저장소 symlink 반영 | 다른 펌웨어에 일반화하지 않음. 현장 설치 정보와 대조 필요 |
| OEM 호출 계약·터치 공존 | ARM veneer/encoder 합성 시험. 순정 커널에서 AA 후크와 터치 DSO 동시 로드. 원본 manager의 자동 LDS→native send와 OBSERVE/SCRUB 실행 | 정상 폰 연결 상태의 AA 송신·터치 입력, 수명·동시성·지연 장애의 전 범위 미검증 |
| 자동 수집·원본 증거 보존 | 별도 collector의 실제 UID 전환·SMDB 응답. 원본 VBS callback과 AA 수신을 합성 입력으로 실행. journal·종료·회수 회귀 | 물리 센서 callback과 주행 전체의 누락·부하·로그 보관량 미검증 |
| 물리 센서 해석 | 원본 펌웨어의 callback ABI/필드 정적 분석과 합성 parser 시험 | 휠/yaw 부호·단위·bias·6MT 후진·cadence 및 품질의 실제 대조 없음 |
| 적분 시간·신선도 | receipt와 producer 시각 분리, MODEL이 qualified로 승격되지 않는 검사 | 현재 IPC payload에는 생산자 시각/순번이 없음. 검증된 지연 상한 등 대체 근거도 없음 |
| 위치 계산·재획득 | 코어·정차 보정·GPS holdout·wheel 보정 합성 시험 | 실제 경로·독립 기준 위치 비교 없음. GPS holdout 차이를 ground truth로 세지 않음 |
| 요청 출처·ASSIST 실행 | adapter의 출처/epoch 검사와 qualified 파이프라인은 합성 입력으로 검사 | live `provenance()`는 항상 false, `allow_assist=false`. 실제 요청 연계와 자격 입력은 미구현 |
| 휴대폰·앱 수용 | OEM LOCATION 경로의 정적 근거와 VM의 실제 native API 호출. 폰 없이도 send=0을 반환함 | Galaxy S25·동글·네이버 지도에서 위치가 반영되는 실행 증거 없음 |
| 다음 부팅과 장애 복구 | 일회 소비 가드·설치 중단 회귀. 실제 SM에서 명시적 재시작과 지연 SIGKILL 뒤 보드 재부팅 요청 관찰 | 다른 실패 경로와 물리 watchdog·전원 차단·다음 부팅의 복구는 미검증 |
| 기존 설정 보존 | touch 설정 편집/제거 roundtrip, 무관한 파일을 변경하지 않는 설치기 | 기존 터치와 km/L의 실제 화면·입력 결과 미검증 |
| 재현 가능한 릴리즈 | 고정 GCC 4.9.1과 다섯 ARM 바이너리, ZIP manifest/source commit. v0.3.1-shadow.1 공개 다운로드 재검사 완료 | v1.0의 최종 커밋·ZIP과 전체 조건 감사 필요 |

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
