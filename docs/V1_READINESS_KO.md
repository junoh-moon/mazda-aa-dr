# v1.0 완료 조건과 작업 목록

최종 목표는 NA 74.00.324A의 2019 MX-5 ND2 6MT에서 기존 AA 터치·km/L
구성을 보존하면서, Galaxy S25와 무선 AA 동글로 연결한 네이버 지도가 GNSS
단절 중에도 차량의 이동·회전을 반영하도록 하는 것입니다. 설치 성공이나
SHADOW 로그 생성만으로 이 목표를 완료한 것으로 보지 않습니다.

사용자가 재확인한 완료 기준은 **계산한 관성항법 위치의 실제 적용**입니다.
GPS 단절 중 보정 위치가 실제 AA 송신에 선택되고, GPS 복귀 시 원본 위치로
전환되며, 대상 지도 앱에 반영되어야 합니다. 현재 제품의 live ASSIST는
비활성이며 이 적용 경로는 미완료입니다. 관측·SHADOW와 연결 경계 검사는
해당 경로를 구현하기 위한 중간 작업으로만 기록합니다.

2026-09-29 시작 기준은 `e1f7ea2624317ba630c4a0077e561ab8a998d587`입니다.
사용자는 중간 커밋, master push, v0.x 릴리즈 게시를 승인했습니다. 아래 작업과
필요한 증거가 남아 있는 동안 v1.0 완료를 선언하지 않습니다.

**2026-10-01에 사용자가 v1.0 전 실차 설치·시험 기회를 한 번 허용했습니다.**
2026-09-30의 펌웨어 파일만 사용하는 제한을 해당 통합 시험에 한해 갱신합니다.
그 기회는 v0.3.9-shadow.1 설치·주행과 후속 자료 회수에 사용됐으며,
회수 파일에는 기동·센서 기록이 없었습니다. 추가 방문이나 휴대폰·동글
탁상 시험까지 승인된 것은 아닙니다. [통합 시험 계획](FIELD_TRIAL_KO.md)의
남은 검증은 기존 자료와 오프라인 시험으로 먼저 진행합니다.
완성된 v1.0에서 관성항법 위치를 실제 적용한다는 최종 목표는 유지합니다.
한 번의 시험이 모든 물리 센서·폰 수용·복구 조건을 해결한다고 약속하거나,
SHADOW 자료만으로 ASSIST 완료 조건을 충족한 것으로 표시하지 않습니다.

차량의 USB 포트 하나를 AA와 설치·회수용 쉘이 번갈아 사용합니다.
AA 연결 중 메뉴2 확인을 요구하지 않습니다. 수집은 시험 부팅에서 자동으로
시작하며, 주차 후 AA를 분리하고 USB·쉘로 돌아와 바로 메뉴3으로 회수합니다.
현재 연결·부팅의 진단과 보존된 시험 기록은 별도로 해석합니다.
실차 사진에서 v0.3.9-shadow.1의 메뉴3이 정상 `/proc/mounts` 링크를 거부한
결함을 확인했습니다. [v0.3.9-shadow.2 핫픽스](../validation/TRIAL_EXPORT_HOTFIX_2026-10-02.md)는
회수 코드와 잘못된 fixture를 수정했습니다. 이미 제거했어도 USB 파일을 교체하고
기존 한 줄과 `3`으로 회수하며 재설치·재주행은 필요하지 않습니다.
이후 회수는 성공했으나 기동·센서 기록 파일이 없었습니다. 사용자는 재부팅과
무선 AA 동글·S25 연결 및 주행을 확인했습니다.
[빈 기록 조사](../validation/EMPTY_CAPTURE_2026-10-02.md)에 따라 실제 기동·저장
실패를 구분하고 있으며 센서 수집·계산 결과는 아직 미판정입니다.
이 순서는 [v0.3.9 최종 ZIP](../validation/RELEASE_V039_2026-10-01.md)의 순정
BusyBox에서 USB 부재·작성한 새 boot 뒤 메뉴3 회수로 검사했습니다.
해당 과거 fixture의 링크 구조 누락을 실차 회수 검증으로 인정하지 않습니다.
물리 USB 전환·실차 재부팅 검증은 아닙니다.

## 작업 목록

- [x] 2026-10-01의 한 번의 통합 실차 시험 허용을 현재 지침에 반영합니다.
- [x] 저장 공간 부족 대응과 실제 수집·계산 진행 확인을 보완하고 회귀를 검증합니다.
- [x] 고정 커밋의 설치 후보와 한 번의 자동 수집·회수·복구 절차를 준비합니다.
  [기존 a29f1b8 후보 검증](../validation/TRIAL_PREPARATION_2026-10-01.md)과
  [Shift 없는 숫자 메뉴를 포함한 후속 후보](../validation/KEYBOARD_TRIAL_2026-10-01.md)를
  완료했습니다. 동시 부하 시 지연 원인과 실제 차량 동작은 검증 완료로 세지 않습니다.
- [x] 기준점 준비 중 정상 yaw 구간을 늦은 입력으로 거부하는 MODEL 결함을 수정합니다.
  [구간 순서 수정과 e2341fb 후보](../validation/MODEL_WINDOW_ORDER_2026-10-01.md)에서
  수정 전 실패·수정 후 실제 worker 복구와 전체 host/ARM·순정 셸 설치를 확인했습니다.
  원본 입력 시각·나이 제한을 유지하며 과거 최초 부하 지연의 원인 확정으로 세지 않습니다.
- [x] 원본 LDS·AA 경로의 요청 발행 당시 세션 문맥과 실제 송신 대상을 제품에 기록합니다.
  [세션 제품 연결](../validation/SESSION_PRODUCT_2026-09-30.md)의 원본 요청 12건과
  별도 상태 callback 실행을 확인했습니다. 이는 요청 소유권의 증명이 아닙니다.
- [x] 세션 전환을 가로지르는 실제 지연 callback과 원본 송신 대상을 검사합니다.
  [후속 검사](../validation/SESSION_TRANSITION_2026-09-30.md)에서 파괴 뒤 남은
  요청이 재시작한 새 세션으로 송신되는 것을 확인했습니다. 클라이언트 전달을
  지연한 진단이며 정상 폰 연결이나 물리 입력 지연을 재현한 것은 아닙니다.
- [x] 관측된 세션 경계에서 adapter의 이전/전환 중 송신 후보를 철회합니다.
  [후속 수정](../validation/SESSION_PREDICTION_LIFETIME_2026-09-30.md)의 새 회귀 8개,
  제품 DSO·stock runtime 세션 22개와 원본 lifecycle의 generation 변경을 확인했습니다.
- [x] MODEL/holdout의 세션 reset·새 기준점 요구와 늦은 이전 요청 처리를 구현·검사합니다.
  [세션 계산 경계 검사](../validation/MODEL_SESSION_2026-09-30.md)는 관측 revision을
  사용하는 MODEL 입력 제한입니다. 세션 소유권·물리 센서의 qualification이나
  live ASSIST 구현 완료로 승격하지 않습니다.
- [x] 외부 세션 경쟁 수정을 기존 revision·MODEL 경계·요청 경로와 통합합니다.
  [통합본 검증](../validation/SESSION_MERGE_2026-09-30.md)에서 storage race의 수정 전후,
  최종 host/ARM 전체 검사와 원본 LDS 실행을 확인했습니다. ARM 합성 송신 한 번의
  실패는 후속 검사에서 재현되지 않았으며 원인은 미분리입니다.
- [x] 실제 JCIDBUS 연결의 API/signal 경계와 요청 발행/응답의 로컬 수명을 기록합니다.
  [버스 관측 검사](../validation/BUS_CONNECTION_2026-09-30.md)에서 원본 LDS 요청
  17건과 daemon 종료를 검사했습니다. close callback만으로 놓친 실제 단절을
  signal 관측으로 보강했습니다. 이 기록 당시 MODEL 버스 reset은 미완료였으며
  아래 후속 작업에서 연결했습니다. daemon/provider 신원과 qualified 요청 연결은 남습니다.
- [x] 외부 MODEL 시각·파서 보강을 버스 관측과 같은 브랜치에 통합합니다.
  [통합 검사](../validation/OBSERVATION_SYNC_2026-09-30.md)는 이전 transport 입력과
  상태 이력·문자열 오류를 재현하고 통합 제품의 원본 LDS·버스 경계를 검사합니다.
  계산에서 제외한 입력의 원시 기록을 보존하며 ASSIST 자격은 채우지 않습니다.
- [x] 실제 LDS 버스의 관측 경계에서 MODEL/holdout을 초기화하고 이전 입력을 제외합니다.
  [버스 계산 경계 검사](../validation/MODEL_BUS_2026-09-30.md)에 수정 전 실패,
  작성한 학습·새 기준점 회귀와 원본 LDS/버스 해제 실행을 구분하여 기록합니다.
  생성·접속부터 관측한 로컬 수명이며 qualified 요청 자격을 부여하지 않습니다.
- [x] 외부 버스 주소 재사용·분석기 수정을 기존 MODEL 초기화와 통합합니다.
  [통합 제품 검사](../validation/BUS_MODEL_MERGE_2026-09-30.md)에서 수정 전후 회귀,
  host/ARM 전체 검사와 원본 LDS 요청 14건·버스 단절을 확인했습니다.
  같은 작업 브랜치를 유지하며 추가 도구를 제거했습니다.
  ASSIST 자격이나 유효 GPS→단절→복귀의 완료로 세지 않습니다.
- [x] 외부 동시 버스 표시·수신 시각 수정을 통합하고 동일 제품을 다시 검사합니다.
  [통합본 검사](../validation/BUS_CLOCK_MERGE_2026-09-30.md)에서 이전 소스의
  두 실패, 수정 후 host/ARM 전체 검사와 원본 LDS 요청 17건을 확인했습니다.
  이 항목은 관측·계산 경계의 회귀 검사이며 실제 ASSIST 적용 완료가 아닙니다.
- [x] 평탄화 전 raw 요청·응답 헤더를 실제 product token에 연결합니다.
  [wire 연결 검증](../validation/WIRE_REQUEST_2026-10-01.md)에서 원본 라이브러리의
  역순·재사용·timeout·취소, 제품 cold 설치와 원본 data-client 연결을 확인했습니다.
  작성 서버/worker fixture이며 producer snapshot 자격이나 폰 수용을 뜻하지 않습니다.
- [x] 원본 등록에서 AA 요청의 서버 주소 GUID와 client 고유 이름을 소유 사본으로 기록합니다.
  [연결 식별자 검증](../validation/AA_ENDPOINT_IDENTITY_2026-10-01.md)에서
  새 제품 DSO와 원본 client의 두 daemon·여덟 요청을 대조했습니다.
  같은 serial·이름 재사용을 구분하고 원시 위치 아홉 필드를 보존합니다.
  이 식별자의 존재는 LDS sideband 연결이나 물리 입력 자격을 뜻하지 않습니다.
- [x] 작성 NMEA 입력을 실제 원본 parser·callback·cache·service 응답과 제품 token까지 연결합니다.
  [원본 필드 출처 실행](../validation/LDS_FIELD_LINEAGE_2026-10-01.md)에서 부분 갱신,
  무갱신 재조회, serial 분리, 같은 바이너리의 계측 on/off와 snapshot 뒤 새 쓰기를
  대조했습니다. 관측기 쓰기 번호는 개별 필드의 물리 생산 시각이 아니며, 초기화·입력·
  대기 순서는 작성한 조건입니다. 제품 qualified 공급부와 전체 SM 기동은 미완료입니다.
  [후속 원본 reader 실행](../validation/LDS_DRIVER_READ_2026-10-02.md)은 직접
  parser/callback 호출 없이 PTY 문장 다섯 개의 캐시 효과와 원본 Close를
  확인했습니다. 작성한 부분 초기화이며 실제 제품 응답까지의 결합은 남습니다.
- [x] LDS 부분 갱신의 필드 출처를 실제 읽기 사본에서 계승하는 코어를 구현합니다.
  [원본 실행 대조](../validation/LDS_OWNED_LINEAGE_2026-10-01.md)에서 읽기 뒤 중간 쓰기,
  같은 값 재할당·미확인 쓰기·응답 사본을 검사했습니다. 새 코어는 비공개 관측 서버에
  소스 링크했습니다. 당시 공개 v0.3.9-shadow.3에는 LDS 제품 자동 기동·등록이
  없었고, v0.3.10-shadow.1부터 관측 제품 연결이 포함됐습니다.
  qualified 입력 공급부는 여전히 미구현입니다.
  관측 할당 identity를 물리 측정 시각이나 qualified 출처로 세지 않습니다.
- [x] qualified ASSIST 큐를 폐기할 때 미처리 POSITION의 음성 경계를 보존합니다.
  [실제 제품 대조](../validation/ASSIST_QUEUE_CUTOFF_2026-10-01.md)에서 같은 tick의
  큐 폐기로 이전 기준점이 발행되는 실패를 재현하고 정상 회복까지 검사했습니다.
  새 물리 입력 공급부·live ASSIST 활성화는 포함하지 않습니다.
- [x] 원본 위치 journal의 고도·horizontal·vertical 누락을 수정합니다.
  [v0.3.7 검증](../validation/RELEASE_V037_2026-10-01.md)에서 실제 제품 DSO의
  아홉 필드·nonfinite/0 구별·버퍼 경계와 host/ARM 전체를 검사했습니다.
  데이터 보존이며 해당 필드의 품질·생산자 시각을 자격화한 것은 아닙니다.
- [x] 지연된 GPS holdout 결과에 실제 원본 위치의 호출·generation 식별자를 보존합니다.
  [연결 검증](../validation/HOLDOUT_REFERENCE_2026-10-01.md)에서 실제 지연 큐·worker와
  formatter, 분석기의 누락·중복·session·과거 형식을 검사했습니다. 연결 진단은
  기존 MODEL 비교와 분리하며 GPS 참값·생산자 시각·ASSIST 자격으로 세지 않습니다.
- [ ] 실제 요청별 provider/session/receiver 자격을 구현·검사합니다.
  [LDS 진단 API 조사](../validation/LDS_DIAGNOSTIC_PROVENANCE_2026-10-01.md)에서
  위치·진단 캐시의 별도 갱신과 공통 snapshot 식별자의 부재를 확인했습니다.
  진단 polling으로 자격을 채우지 않으며 물리 자격을 갖춘
  생산자→snapshot→응답 공급부는 미구현입니다.
  해당 정적 조사와 위 별도 실행을 상위 물리 자격·ASSIST 항목의 완료로 세지 않습니다.
  [요청 경로 보존](../validation/REQUEST_ROUTE_2026-09-30.md)은 발행 당시 원본
  목적지·경로·인터페이스·메서드명을 응답까지 연결합니다. well-known 서비스
  이름은 실제 제공자 인증이 아니므로 이 상위 자격 항목은 미완료입니다.
- [x] LDS 전용 cold 설치기·제품 빌드와 normal/WCP 기동 연결을 구현합니다.
  [원본 설치기 검사](../validation/LDS_COLD_INSTALL_2026-10-02.md)는 실제
  원본 등록 ID 10개와 25개 데이터 슬롯, 실패 시 원본 전달을 확인합니다.
  같은 프로세스의 cache 할당 출처를 보존하는 기반이며 물리 생산 시각이나
  실제 요청별 ASSIST 자격이 아닙니다. 최종 제품·ZIP 통합과 정상 전체
  기동 검증은 별도 결과로 확인합니다. 공개 v0.3.10-shadow.1에 포함했고
  과거 `.3` ZIP은 변경하지 않았습니다.
- [x] 실제 LDS 제품의 원본 callback·응답 출처를 실제 AA worker 기록까지 연결합니다.
  [제품 실행](../validation/LDS_PRODUCT_RUNTIME_2026-10-02.md)에서 응답 9건·
  원시 값 81개와 부분 할당 출처를 대조했습니다. 작성한 AA 기동·WorkerScope이며
  정상 전체 SM·AA 송신·물리 입력 자격의 완료로 세지 않습니다.
- [x] 같은 worker에서 immutable POSITION과 LDS sideband를 정확한 요청으로 연결하는
  크기 제한 공급부를 구현합니다. [실제 입력 연결](../validation/LDS_REQUEST_SOURCE_2026-10-02.md)에서
  같은 worker의 source callback 조회와 충돌·폐기·회복을 검사합니다.
  비동기로 늦게 도착한 관측 때문에 이미 실행한
  callback의 출처를 사후 유효로 바꾸지 않습니다. cache 할당 번호·관측 시각은
  물리 측정 순번·시각이 아니며 센서·receiver 자격과 readiness는 별도로 구현합니다.
- [ ] 연결 결과를 검증된 기준점·제어 입력으로 바꾸는 실제 공급부와 송신 전에
  확립하는 요청별 출처를 구현합니다. 새 연결을 `ASSIST_POSITION`에 바로 넣거나
  원본 callback provenance를 사후 수정하지 않습니다. 물리 센서 시각·보정·품질과
  현재 요청·세션·버스 자격은 아직 공급되지 않습니다.
- [x] 같은 위치 callback에서 확보한 요청 Trace·호출·세대를 출처 검증 함수에 전달합니다.
  [inline 요청 문맥 검사](../validation/PROVENANCE_CONTEXT_2026-10-02.md)에서
  one-shot 재조회 없이 동일 요청을 전달하고 중첩 frame·실패·세대 철회를
  확인했습니다. 실제 provider·receiver의 자격 판단 구현과 구분합니다.
- [x] 외부 journal 큐 수정을 통합하고 동일 제품의 원본 실행에서 관측 loss 0을 확인합니다.
  과거 drop의 정확한 호출 조합·원인을 확정한 것으로 세지 않습니다.
- [x] 세션 관측·전환 검사의 추가 도구는 설치 전후 목록을 남기고 모두 제거했습니다.
  이후 작업에서도 도구를 추가하면 같은 정리를 반복합니다.
- [x] 공개판과 로컬 수정본의 차이, 실제 OEM 실행 근거를 재확인합니다.
- [x] 고정 커밋의 새 checkout에서 전체 host/ARM 검사를 다시 실행하고 skip을 확인합니다.
  e1f7ea2 기준선과 bcdfda9, af1a24a에 이어 외부 260528c의 전체 검사도
  생략 없이 통과한 [발행자 기록](../validation/RELEASE_V033_2026-10-01.md)을
  확인했습니다. 이번 작업의 별도 b185b99 host/ARM 검증은 위 후보 기록을 따릅니다.
- [x] 기존 설치 오류를 고친 v0.x ZIP을 게시하고 게시된 파일을 다시 받아 검사합니다.
  외부 v0.3.3-shadow.1의 ZIP·체크섬을 직접 받아 SHA-256·CRC·manifest와
  고정 커밋을 대조했습니다. [발행자의 전체 검증](../validation/RELEASE_V033_2026-10-01.md)과
  작업 브랜치의 새 후보를 구분합니다. 이후 사용자 지시로
  [v0.3.4 발행](../validation/RELEASE_V034_2026-10-01.md)을 완료했습니다.
  master 반영·고정 소스의 새 빌드와 검증 제품의 동일성, 최종 순정 셸 설치 및
  게시된 ZIP의 재다운로드를 확인했습니다.
  [v0.3.5 발행](../validation/RELEASE_V035_2026-10-01.md)은 외부 master의 코드까지
  통합해 host·ARM 전체와 최종 ZIP을 새로 검사했습니다. 첫 host 큐 막힘과
  변경 없는 재실행 통과, 미확정 원인을 함께 보존했습니다.
  [v0.3.6 발행](../validation/RELEASE_V036_2026-10-01.md)은 오래된 ASSIST 기준점
  재사용 결함을 수정한 커밋에서 다섯 ARM 파일과 SHADOW ZIP을 새로 만들고
  clean host·고정 ARM 전체, 원본 OEM VM의 개별 결과, 순정 BusyBox 설치와
  공개 파일 재다운로드를 검사했습니다. VM runner의 제한 종료와 실제 차량·폰
  미검증을 구분합니다.
  [v0.3.7 발행](../validation/RELEASE_V037_2026-10-01.md)은 누락 위치 필드 보완을
  고정한 새 제품의 host Python 378개·C/C++·고정 ARM 전체, 실제 DSO 기록 함수,
  원본 LDS parser부터 응답까지, 최종 순정 BusyBox ZIP과 공개 파일을 대조했습니다.
  임시 도구를 제거하고 최초 호스트 목록과 일치함을 확인했습니다.
  [v0.3.8 발행](../validation/RELEASE_V038_2026-10-01.md)은 holdout 원본 연결을
  포함한 `40051f8`의 새 checkout에서 host Python 401개·C/C++·고정 ARM 전체를
  생략 없이 통과했습니다. 최종 순정 BusyBox ZIP과 공개 파일을 대조하고
  추가 설치 도구를 제거했습니다. 이번 판에서 원본 LDS 실행이나 실제 DSO의
  holdout formatter 직접 호출을 새로 검사한 것은 아닙니다.
  [v0.3.9 발행](../validation/RELEASE_V039_2026-10-01.md)은 `3346854`의
  AA 연결 식별자와 단일 USB 회수를 포함합니다. host Python 421개·C/C++,
  고정 ARM 전체·실제 DSO 145개 사례와 최종 순정 BusyBox ZIP을 생략 없이 검사하고
  공개 다운로드·고정 소스·전체 manifest를 대조했습니다. 원본 여덟 요청은
  선행 `f2ec7c2`에서 실행했고 최종 63개 빌드 입력·다섯 산출물의 동일성을
  확인했습니다. 추가 도구를 제거했으며 ASSIST 자격은 추가하지 않았습니다.
- [x] 센서·항법, OEM 호출/복구, USB/분석기의 독립 감사를 수행하고 확인된 결함을 수정합니다.
- [x] 실제 OEM SM의 명시적 재시작과 지연 종료 정책을 실행·정적 근거로 확인합니다.
  정상 전체 기동과 물리 watchdog 재부팅은 아래 별도 조건으로 남깁니다.
- [ ] 실제 센서 callback부터 계산까지의 통합 경로와 입력 단위·품질·시간 근거를 확보합니다.
  [VIP 생산자 조사](../validation/VIP_ACCUMULATOR_2026-10-01.md)에서 원본 MCU의
  선택한 요레이트 명령어를 해석 실행해 합계·개수의 넘침을 확인했습니다.
  구별할 수 없는 합계를 MODEL이 수락하던 조건을 수정했으나, 개수 넘침·오류
  표본 혼합·실제 주기와 물리 센서의 자격은 입증되지 않았습니다.
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
  [후속 양성 입력 통합 VM](../validation/INTEGRATED_POSITIVE_VM_2026-10-01.md)은
  외부 `d3c6fa1` 제품 DSO에 원본 LDS·VIM 경유 합성 입력을 동시에 주어 약 5초
  no-fix 동안 연속 MODEL 유효 47건과 복귀 뒤 철회를 관측했습니다. 물리 센서의
  생산 시각·주기·품질, 완료된 GPS holdout, 차량 정확도는 여전히 미확인입니다.
  [외부 `c62b313` DSO 재실행](../validation/INTEGRATED_CURRENT_DSO_VM_2026-10-01.md)도
  두 합성 구간에서 계산을 재현했다고 기록했지만 guest halt 뒤 runner가 제한
  종료됐고 전체 로그는 미결론입니다. 원시 자료가 없어 이번 작업에서 수치를
  독립 재검증하지 않았으며 v0.3.9 endpoint 제품의 검사로 합산하지 않습니다.
  상위 물리 입력·시간 자격 항목은 미완료입니다.
- [x] 원본 직렬 읽기·GPS 파서의 유효 응답으로 단절·복귀와 제품 MODEL을 검사합니다.
  [LDS 입력 기동 조사](../validation/LDS_INPUT_STARTUP_2026-09-30.md)의 여섯 VM에서
  SYSTEM 응답·USB 목록 요청까지 진행했지만 mode 0·READ_NOT_READY입니다.
  작성한 NMEA의 체크섬 검사나 빌드만 한 caller를 이 항목의 완료로 세지 않습니다.
  [수신기 대기 후속 조사](../validation/LDS_RECEIVER_2026-09-30.md)의 다섯 실행에서도
  mode 0입니다. USB 응답·callback·타이머와 최초 전원 write 성공은 확인했으나
  당시에는 GPIO 읽기가 0인 원인과 원본 파서의 소비 경로가 미완료였습니다.
  [GPIO 모델 후속 실행](../validation/LDS_GPIO_2026-09-30.md)에서 원본 설정에 따른
  되읽기를 보완하고, 작성한 NMEA의 원본 파싱과 유효→무효→복귀를 확인했습니다.
  제품의 원본 위치 26건·MODEL 유효 결과 51개와 독립 직진 계산식 일치를
  검사했습니다. 두 핀의 실험용 QEMU 모델과 합성 센서 입력 범위입니다.
  보정 위치의 실제 ASSIST 송신·물리 센서·폰 수용은 아래 미완료 항목으로 남습니다.
- [ ] 실제 요청의 provider/receiver/session 출처를 보존하는 ASSIST 연결을 구현·검사합니다.
  [원본 AA 선택 시험](../validation/ASSIST_SELECTION_2026-10-01.md)에서 기존 core·adapter를
  별도 실행 파일에 연결하고 합성 자격으로 계산 위치의 선택·GPS 복귀를 실행했습니다.
  자격 거부·MODEL 조건의 원본 유지도 확인했습니다. 제품 runtime은 실행하지 않았고
  기준점·센서·시각·자격은 별도 작성 조건입니다. 물리 입력 자격과 제품 ASSIST 연결의 완료로
  세지 않으며, 최초 원본 manager 초기화 실패의 원인도 미분리입니다.
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
  외부 master의 [원본 커널 GPIO 진단](../validation/LDS_GPIO_VM_2026-10-01.md)은
  별도 VM의 USB 출력 latch 1과 PSR/sysfs 0을 보고합니다. 그 진단에는 LDS의
  유효 NMEA·GetPosition 실행이 없으며 실제 하드웨어·receiver 자격은 미검증입니다.
  이 작업 브랜치의 후속 파서 실행은 위 GPIO·제품 통합 기록에서 구분합니다.
  외부 master의 [후속 원본 LDS 합성 NMEA 실행](../validation/LDS_PATH_VM_2026-10-01.md)은
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
  제한을 300초로 늘린 무개입 대조에서는 긴 무진행 뒤 질의가
  4~18회 재개됐습니다. 180초 관찰을 영구 정지로 보지 않으며,
  복귀 원인과 게스트·QEMU 시각 차이는 아직 분리되지 않았습니다.
  후속 T16·T17에서는 롤오버와 다음 OCR1 비교/GIC IRQ 87 입력,
  콘솔상 질의 재개가 같은 QEMU 실행에서 이어졌습니다. 다른 CPU
  타이머 IRQ도 관찰되어 단독 원인·OEM 재시도·실차 성립은 여전히
  확정하지 않습니다. [계측 기록](../validation/LDS_TIMER_WAKE_BOUNDARY_2026-10-01.md)을
  따릅니다.
  [LDS 진단값 출처 연결 검토](../validation/LDS_DIAGNOSTIC_PROVENANCE_REVIEW_2026-10-01.md)에서
  원본 `GetPosition`과 `GetUbloxDiag`의 출력 형식·별도 cache mutex를
  대조한 외부 결과를 보존합니다. 두 응답을 같은 생산 측정으로 묶는 ID가 없어 진단
  polling으로 요청별 자격을 만들 수 없습니다. 원본 갱신 순서의 근거는
  [외부 정적 분석](../validation/LDS_DIAGNOSTIC_PROVENANCE_2026-10-01.md)이며
  해당 정적 조사 당시 제품의 생산자→snapshot→요청·응답 연결은 미구현이었습니다.
  후속 관측 공급부의 구현은 위 완료 항목과 구분하며 물리 자격은 아직 미완료입니다.
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
  [후속 세션 상태 진단](../validation/SESSION_EVENTS_2026-09-30.md)에서는
  원본 시작 반환 0 뒤의 INVALID callback과 생성별 NULL userdata를 실행으로
  확인했습니다. 합성 시작 입력의 실패 경로이며, 생성별 식별자·상태를 실제
  요청 발행 시점에 보존하는 제품 연결은 아직 미구현입니다.
  [후속 세션 제품 연결](../validation/SESSION_PRODUCT_2026-09-30.md)에서 이 문맥의
  복사와 실제 send 대상 관측을 구현했습니다. 원본 요청 12건의 lifetime 1·2와
  별도 실제 INVALID callback을 확인했습니다. ambient 문맥을 qualified 요청
  소유권으로 승격하지 않았으므로 상위 ASSIST 항목은 여전히 미완료입니다.
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
  [같은 브랜치의 통합·직접 재검증](../validation/PIPELINE_MERGE_2026-10-01.md)에서는
  수정 전 실패, 통합 host/ARM와 원본 LDS의 GPS 단절·복귀를 확인했습니다.
  새 제품의 MODEL 결과 56개를 수락된 기준점의 독립 계산과 대조했습니다.
- [x] 외부 master에서 같은 원본 VIP의 16비트 요레이트 합계 넘침을 작은 정상 평균으로 수락하던
  MODEL 결함을 수정했습니다. [원본 생산자 조사](../validation/VIP_ACCUMULATOR_2026-10-01.md)의
  입력 해시를 대조한 기록과, [host/ARM 통합 검사](../validation/VIP_ACCUMULATOR_INTEGRATION_2026-10-01.md)를
  보존합니다. 이번 후보의 검사 개수로 합산하지 않습니다. count 자체의 넘침·손실 창·생산 시각·물리 센서 품질은
  이 경계식으로 복원되지 않으며 ASSIST 자격으로 세지 않습니다.
- [x] qualified 코어의 원본 입력 lease·나이·시간·오차 예산에서 비동기 송신
  기한을 구하는 API와 파이프라인 연결을 구현했습니다.
  [송신 기한 검증](../validation/ASSIST_PUBLICATION_2026-10-01.md)은 지연 송신,
  기한 경계·GPS 복귀·대기 중인 기준점·MODEL 거부와 전체 host/ARM 통과를
  기록합니다. 해당 검사 당시 미구현이던 qualified worker 발행 연결은
  [후속 runtime 구현](../validation/ASSIST_RUNTIME_2026-10-01.md)에서 보완했습니다.
  live 자격 입력 공급부는 여전히 미구현입니다.
- [x] 실제 adapter 관측을 계산기에 연결해 GPS 품질 변경·복귀의 generation
  불일치를 재현·수정했습니다. [제품 DSO 연결 검사](../validation/ASSIST_GENERATION_2026-10-01.md)는
  별도 시험 worker의 계산→발행→송신 10개 사례를 고정 ARM 및 순정 공유 runtime에서
  확인합니다. 작성한 자격·센서 입력이며 live runtime 입력 연결 완료로 세지 않습니다.
- [x] 동일 qualified Pipeline을 유지한 GPS 복귀·새 기준점·다음 단절을 구현·검사했습니다.
  [연속 재획득 검증](../validation/ASSIST_REACQUISITION_2026-10-01.md)은 이전 ARM DSO의
  중복 복귀 실패와 수정 제품 15개 송신 사례를 기록합니다. 별도 초기화로 재시작한
  이전 시험과 구분합니다. 당시 미완료였던 runtime worker 연결은 위 후속
  구현에서 보완했으며, 실제 입력 자격 공급부는 여전히 미완료입니다.
- [x] 기준점이 결합되지 않은 새 GPS 콜백 뒤 이전 READY seed를 재사용하는
  결함을 수정했습니다. [콜백 순번 결합 검증](../validation/ASSIST_ANCHOR_PAIRING_2026-10-01.md)은
  이전 제품의 잘못된 DR 선택을 재현하고, 같은 POSITION의 ANCHOR만 후보로
  인정하는 최종 제품 검사를 기록합니다. qualified 입력 공급부는 여전히
  미구현이며 live ASSIST는 비활성입니다.
- [ ] 지원 범위의 위치 정확도와 Galaxy S25/무선 AA/네이버 지도 수용을 검증합니다.
- [ ] 정상 전원 주기·실패 복구·기존 터치/km/L 공존의 실제 결과를 확인합니다.
- [ ] 최종 커밋과 게시 ZIP을 고정하고 아래 조건 전체를 다시 감사합니다.

## 요구사항과 증거

| 요구사항 | 현재 근거 | 부족한 구현 또는 증거 |
| --- | --- | --- |
| 깨끗한 USB에 압축 해제 후 `sh install.sh` | MP3/JS·정적 해시 도구 포함. 실제 ARM BusyBox/libc에서 최종 ZIP 설치·제거·재설치 성공 | 실제 CMU 미디어의 MP3→shell 동작, 실제 저장소의 remount·내구성은 미검증 |
| 정확한 펌웨어·계정·경로 | 기존 네 원본과 LDS 의존성 여섯 identity. `cmu=0`, `service=1001`, 순정 저장소·libdbus symlink 반영 | 다른 펌웨어에 일반화하지 않음. 현장 설치 정보와 대조 필요 |
| OEM 호출 계약·터치 공존 | ARM veneer/encoder 합성 시험. 순정 커널에서 AA 후크와 터치 DSO 동시 로드. 원본 manager의 자동 LDS→native send와 OBSERVE/SCRUB 실행 | 정상 폰 연결 상태의 AA 송신·터치 입력, 수명·동시성·지연 장애의 전 범위 미검증 |
| 자동 수집·원본 증거 보존 | 별도 collector의 실제 UID 전환·SMDB 응답. 원본 VBS callback과 AA 수신을 합성 입력으로 실행. journal·종료·회수 회귀, [주차 중 수집/계산 분리 진단](../validation/TRIAL_STATUS_REVIEW_2026-10-01.md), [MODEL 초기화 원인 기록](../validation/MODEL_RESET_REVIEW_2026-10-01.md) | 물리 센서 callback과 주행 전체의 누락·부하·로그 보관량 미검증. 상태 명령의 종료 코드 0은 항법 계산·폰 수용이 아님 |
| 물리 센서 해석 | 원본 펌웨어의 callback ABI/필드 정적 분석과 합성 parser 시험. VIP 요레이트 생산자의 선택 경로 해석 실행과 MODEL 합계 넘침 거부 | 휠/yaw 부호·단위·bias·6MT 후진·cadence 및 품질의 실제 대조 없음. count 넘침과 손실 창도 식별 불가 |
| 적분 시간·신선도 | receipt와 producer 시각 분리, MODEL이 qualified로 승격되지 않는 검사 | 현재 IPC payload에는 생산자 시각/순번이 없음. 검증된 지연 상한 등 대체 근거도 없음 |
| 위치 계산·재획득 | 코어·정차 보정·GPS holdout·wheel 보정 합성 시험 | 실제 경로·독립 기준 위치 비교 없음. GPS holdout 차이를 ground truth로 세지 않음 |
| 요청 출처·ASSIST 실행 | raw 헤더→제품 token 관측, qualified 파이프라인→실제 worker→발행·철회와 코어 근거 송신 기한 검사 | live `provenance()`는 항상 false, `allow_assist=false`. 물리 센서·요청별 qualified 자격 입력 공급부는 미구현. 원본 snapshot과 각 필드 생산자 연결·전체 manager 이후 반복 취소 timeout 원인 미분리 |
| 휴대폰·앱 수용 | OEM LOCATION 경로의 정적 근거와 VM의 실제 native API 호출. 폰 없이도 send=0을 반환함 | Galaxy S25·동글·네이버 지도에서 위치가 반영되는 실행 증거 없음 |
| 다음 부팅과 장애 복구 | 일회 소비 가드·설치 중단 회귀. 실제 SM에서 명시적 재시작과 지연 SIGKILL 뒤 보드 재부팅 요청 관찰 | 다른 실패 경로와 물리 watchdog·전원 차단·다음 부팅의 복구는 미검증 |
| 기존 설정 보존 | touch 설정 편집/제거 roundtrip, 무관한 파일을 변경하지 않는 설치기 | 기존 터치와 km/L의 실제 화면·입력 결과 미검증 |
| 재현 가능한 릴리즈 | 고정 GCC 4.9.1과 여섯 ARM 바이너리, ZIP manifest/source commit. [v0.3.10-shadow.1](../validation/RELEASE_V0310_2026-10-02.md) 공개 다운로드 재검사 완료 | v1.0의 최종 커밋·ZIP과 전체 조건 감사 필요 |

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
오프라인에서 가능한 수정과 검증을 계속합니다. 파일만으로 답할 수 없는 조건은
위 표의 미검증 항목으로 보존합니다. 2026-10-01에 허용된 한 번의 실차 시험은
이미 사용됐고, 회수 파일에 기동·센서 기록이 없어 해당 항목은 미판정입니다.
[현재 통합 시험 계획](FIELD_TRIAL_KO.md)에 따라 기존 자료와 오프라인 검증을
먼저 진행합니다. 추가 방문이나 휴대폰·동글 탁상 시험은 승인된 범위가 아닙니다.
추가 도구는 격리된 환경에 설치하고 목록과 제거 결과를 검증 기록에 남깁니다.
