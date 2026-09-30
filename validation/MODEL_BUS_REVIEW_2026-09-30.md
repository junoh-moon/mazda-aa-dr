# MODEL 버스 경계 통합·수신 시각 수정·직접 실행 — 2026-09-30

NA 74.00.324A 전용입니다. AA 세션이 유지되어도 LDS 연결이 바뀌면 이전
MODEL 기준점·학습값·대기 입력을 초기화하도록 외부 변경을 통합했습니다.
독립 리뷰와 직접 실행에서 동시 연결 표시 누락, 분석기의 모순 통과,
시험 fixture의 거짓 통과와 실제 수신 시각 순서 결함을 재현·수정했습니다.
최종 제품의 원본 VM에서는 실제 LDS 응답 42건과 세션을 유지한 복수 연결·
해제를 검사했습니다. 원본 GPS는 여전히 mode 0이며 live ASSIST는 비활성입니다.
공개 ZIP은 기존 v0.3.1-shadow.1이고 이 기록은 v1.0 완료 판정이 아닙니다.

## 출처와 제품 변경

부모 master는 `00c6c5ed20ac8d777e22fd8503cc5d7d368dcec5`입니다.
외부 `2548312ce1b4b20dc5aff6d69a56aca08c4f0ad1`의 `c595407` 이후 작성
변경을 선택 적용하여 기존 signal 주소 재사용 수정·요청 경로·분석기 검사를
보존했습니다. 외부의 [MODEL 버스 기록](https://github.com/junoh-moon/mazda-aa-dr/blob/2548312ce1b4b20dc5aff6d69a56aca08c4f0ad1/validation/MODEL_BUS_2026-09-30.md)의
실행 수치와 아래 직접 실행 수치를 합치지 않습니다.

알려진 원본 위치 callback으로 submit을 시도한 활성 연결의 수명을 표시하고,
worker가 연결 identity·관측 revision을 읽습니다. 연결 변경·단절·해제·복수
연결·관측 중간 상태는 navigation과 holdout을 함께 초기화합니다. 기준점,
학습·적용 보정과 대기 입력도 지웁니다. 요청의 발행·응답 연결과 worker의 새
경계를 대조하며, 원시 입력에는 session/bus 중 더 늦은 시각 하한을 적용합니다.
경계 전 receipt/transport 입력은 계산에서 제외하고 원시 값과 사유를 기록합니다.
계산 불가 중에도 raw 수집은 유지합니다. OEM 호출·바이트·반환 계약은 유지합니다.

이는 관측된 submit 시도의 연결 집합입니다. 알려지지 않았거나 CONNECTING인
연결, 실제 provider 소유권 또는 성공적인 LDS 응답 전체를 증명하지 않습니다.
관측 revision에는 다른 bus lifecycle도 포함되어 보수적인 MODEL reset이 생길 수
있습니다. 실패한 submit도 관측 시도에서 사라지지 않으며 qualified 자격은 아닙니다.

## 실패 재현과 수정

- 기존 master의 실제 작성 worker에서 AA 세션을 유지하고 bus를 끊으면
  이전 MODEL이 `stale_valid=13`으로 남고 bus reset ABORT는 0이었습니다.
  직접 실행과 독립 리뷰에서 각각 재현했으며, 수정 후 같은 조건에서 제거됩니다.
- 외부 구현은 연결을 표시하기 전에 전체 bus snapshot을 읽었습니다. 다른
  표시 작업의 TRANSITION 때문에 정상 활성 연결 하나를 누락하여 실제 두
  출처를 단일 출처로 보았습니다. 직접 동시 재현은 두 번째 반복에서 실패했고,
  수정 후 100,000회 표시를 통과했습니다. 재사용하지 않는 슬롯을 먼저 잡고
  상태·주소·수명을 재검사하며 source lifetime을 단조 CAS로 갱신합니다.
  늦은 이전 표시가 새 수명을 덮거나 단발 CAS 실패로 새 표시를 잃지 않습니다.
  lock-free 루프이며 상수 시간 또는 wait-free 상한을 주장하지 않습니다.
- 분석기는 새 MODEL 경계와 기존 request/health의 수명 소유권을 별개로
  취급하고, 불가능한 revision/상태 이력·잘못된 복합 raw 하한·경계를 넘긴
  holdout을 허용했습니다. 독립 32개 이력에서 수정 전 누락 21개, 수정 후
  불일치 0을 직접 확인했습니다. worker 순서와 늦게 배출된 요청 snapshot의
  순서를 구분하며 boot 초기화·legacy schema·정상 지연을 유지합니다.
- 기존 fixture는 최초 MODEL 경계보다 이른 boot 행만 기다렸습니다. 제어
  지연을 넣으면 경계 전 READY/holdout BEGIN이 각각 0인데도 성공했습니다.
  두 입력 가능 경계가 flush될 때까지 기다리고 경계 전 양성 대조를 요구합니다.
  독립 제어 재현에서 수정 후 READY 7·BEGIN 1·raw 528/528을 확인했습니다.
- fixture가 비차단 send의 즉시 성공을 가정했습니다. 독립 제어 정지에서
  원래 코드는 seq 110/EAGAIN/exit 134, 수정 fixture는 같은 datagram의
  32회 재시도·94.65ms와 raw 528/528로 완료했습니다. 재시도는 fixture에만
  있으며 sequence·시각을 바꾸지 않습니다. 관측 경과 100ms 전까지만
  재시도하고 실제 scheduler 완료 시간 상한으로 주장하지 않습니다.
  외부 과거 입력 실패의 정확한 원인은 이 제어 재현만으로 확정하지 않습니다.

첫 전체 ARM 실행에서는 **제품의 실제 수신 순서 결함**도 발견했습니다.
정상 raw seq 342의 checked 시각은 `166054886150827`, received 시각은
`166054886175119`로 24,292ns 뒤였습니다. caller가 clock을 읽은 다음
`recvmsg`가 실행되기 전에 도착한 정상 datagram을 미래 입력으로 거부했고,
다음 sequence까지 불연속으로 판정했습니다. 실행은 exit 134였습니다.

`MotionReceiver::receive`의 외부 now 인자를 없애고 `recvmsg` 뒤에서
CLOCK_MONOTONIC을 읽습니다. syscall errno는 clock 호출 전에 보존합니다.
실제 recvmsg 진입 뒤 packet을 보내는 공개 wrapper 회귀를 추가했고, clock을
다시 syscall 앞으로 옮긴 변형은 FUTURE assertion으로 실패합니다. 실제 미래
입력·1ns 경계·정확히 250ms의 허용/초과 거부는 유지합니다. 허용 오차를 늘려
문제를 숨기지 않았습니다. 후속 ARM 실행에서 두 navigation fixture의 새
clock 의존성에 `-lrt`가 빠져 링크 실패했으며 해당 두 링크를 보완했습니다.

## 독립 리뷰와 작성 코드 검사

세 독립 리뷰가 연결 동시성/전달, MODEL worker/학습 초기화, journal/분석기를
나누어 검사했습니다. 아래는 리뷰어의 실행이며 root의 ARM/OEM 실행과 별개입니다.

- 연결 리뷰는 GNU 공개 bus 31개, native·ASan/UBSan·TSan의 작성 동시성,
  100,000회 표시와 100,000회 실제 작성 submit loop를 검사했습니다.
  이전·새 수명 표시 순서를 제어한 plain-store/단발-CAS 변형을 검출했습니다.
  Darwin forced-unwind 한 사례의 skip은 GNU 성공으로 소급 변경하지 않습니다.
- 분석기 리뷰는 32개 이력·355개 schema, 기존 bus 관계 112개와 session/
  MODEL/motion/route 계약을 재검사했습니다. 공개 12개 메서드는 단일 결함
  변형 16개를 모두 검출했습니다. 최대 새 행 길이와 기존 3,933바이트
  observation의 일반 worker/종료 배출도 검사했습니다.
- worker 리뷰는 free 진행 중·같은 연결의 이전 issue·오래된 raw와
  경계 전 GAP의 양성 대조를 추가했습니다. 양쪽 계산기의 학습 zero 2067,
  scale 1.03을 만든 뒤 reset 후 명목값 2047/1과 version 0, 새 기준점
  회복을 검사했습니다. 최종 수신 API의 native 13개 실행과 수신 순서
  변형 검출도 완료했습니다. 별도 clock 오류 주입 리뷰 9개와 세 변형도
  실행했으며 진단 errno 보존과 전역 errno의 별도 계약을 구분했습니다.

공개 worker 검사는 session 6개, bus 9개, 이전 raw 1개, 경계 전 GAP 7개의
총 23회입니다. 각 성공 입력의 열 필드·순서와 journal을 대조합니다.
일반 실행은 528개, 오래된 raw 추가 실행은 530개입니다. 새 기준점 없는
경계 직후와 연결 회복 구간을 포함하여 오래된 valid가 남지 않는지 검사합니다.
마지막 리뷰의 지적에 따라 예정된 5200ms 대신 실제 `position call=5201`의
기록 시각을 종료점으로 사용합니다. 새 위치 행과 최초 reset이 없으면 실패합니다.

최종 `make test`는 Python **318개**와 C/C++ 전체를 skip 없이 통과했습니다.
stock rootfs·제품 bundle 경로를 명시하여 패키징 검사도 실행했습니다.
후속 runtime 범위와 고정 GCC 4.9.1 ARM 전체 재실행도 통과했습니다.
ARM은 실제 제품 DSO의 위치 8개·요청 14개·세션 29개·bus 31개와 실제
수신 순서 회귀를 포함하며 skip은 0입니다. 마지막 worker 판정 구간 보강의
host/ARM 23회씩 재실행도 모두 통과했습니다. 별도 JSON 재대조에서 각
실행의 최초 reset부터 실제 새 GPS 시각 전까지 invalid 30~31행, valid 0을
확인했습니다. 각 플랫폼의 raw 총 12,146개를 보존했고 일반 분석은 23개
모두 inconclusive·violation 0입니다. 이 판정 보강은 제품을 바꾸지 않습니다.
첫 parser 재실행은 macOS에서 Linux ELF 및 `/tmp` 조건을 잘못 사용하여
실패했습니다. GNU 환경의 올바른 fixture로 journal 82개를 다시 통과했으며,
이 환경 선택 오류를 제품 결함 또는 추가 통과 수로 계산하지 않습니다.

## 최종 제품의 원본 VM 직접 실행

원본 커널·공유 runtime·LDS·AA를 격리 VM의 진단 init으로 실행했습니다.
네트워크·호스트 장치·공유 폴더를 연결하지 않았습니다. 기존 CMU 진입의
machine ID 인자 조정 외 kernel/OEM 바이트는 보존합니다. 정상 전체 SM
기동이나 물리 센서·휴대폰 시험은 아닙니다. 제품 DSO의 자동 cold-install과
실제 BLM manager→LDS→callback→worker→native send를 검사했습니다.

| 직접 실행 항목 | 세션 재생성·manager 정지 | 세션 재생성·manager 재시작 | AA 세션 유지·bus 변경 |
| --- | ---: | ---: | ---: |
| 실제 LDS 응답 | 14 | 16 | 12 |
| 이전 세션의 지연 요청 / MODEL 제외 | 4 / 4 | 4 / 4 | 해당 없음 |
| 이전 세션 지연 응답의 SEND / LOCATION | 0 / 0 | 8 / 4 | 해당 없음 |
| 보존한 합성 raw | 437 | 549 | 480 |
| 시간 경계 제외 raw 대조 | 2 | 2 | 2 |
| journal loss / 요청 loss / bus fault | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |

세 조건마다 명시적인 unassociated 합성 위치 seed 하나를 별도로 넣었고
MODEL에서 제외했습니다. raw도 작성 sender가 MODEL channel에 넣은 합성
입력입니다. 실제 LDS 응답은 모두 mode·UTC·좌표 0이고 모든 MODEL은 invalid입니다.
raw 제외 여섯 건은 epoch/sequence로 원본 sensor·receipt·transport를 대조하고
실제 session/bus 중 더 늦은 경계와 사유까지 검사했습니다.

세 번째 조건은 AA lifetime/revision을 1/1로 유지했습니다. 추가 원본 bus를
연결한 것만으로는 위치 출처가 되지 않고, 알려진 위치 callback으로 실제
GetPosition을 submit한 뒤 복수 출처가 됩니다. 추가 응답 한 건은
`bus_unavailable`로 MODEL에서 제외했습니다. 추가 method 네 개의 원본 free
후에는 그 callback 없이 슬롯이 회수되며 주 연결로 돌아옵니다. 새 주 연결
응답 한 건 뒤 manager/bus를 해제하면 no_live_connection이 됩니다.
복수 출처 구간 raw 43개와 연결 부재 구간 raw 46개도 보존했습니다.
별도 bus monitor는 완료 요청 42개와 취소 요청 4개의 실제 wire 호출을 보았습니다.

기존 실제 daemon 종료도 재검사했습니다. connected→disconnected,
generation 18→20, 원본 close callback 0회와 bus fault 0을 확인했습니다.
동일 객체·이름 reconnect의 원본 반환 0 실패는 여전히 남습니다. 별도 원본
INVALID callback 두 주기와 원본 공유 runtime의 작성 세션 DSO 29개·bus
DSO 31개도 완료했습니다. 이 작성 API 사례를 원본 OEM 함수 전체의 예외
또는 취소 검증으로 확대하지 않습니다.

일반 분석기는 다섯 journal 모두 inconclusive, violation 0입니다.
inconclusive 수는 callback 17, manager 정지 45, 재시작 49, bus 변경 48,
독립 bus 8입니다. 세션·bus reset, MODEL 입력 fault, 없는 위치 입력 등
실제 제한을 정상 세션으로 바꾸지 않았습니다.

VM helper는 300초 한계에서 exit 124, QEMU exit 0·timed_out=true입니다.
guest는 caller와 작성 검사를 마친 뒤 halt했습니다. timeout/종료값을 성공
oracle로 사용하지 않고 확정 console의 완료·요청·단절·최종 health와
capture.done을 별도로 검사했습니다. 기록을 훼손한 판정기 변형 19개도
모두 거부했습니다.

## 고정 산출물·외부 후속 조사·남은 작업

| 산출물 | SHA-256 |
| --- | --- |
| 제품 libmx5dr.so | `a911773cfdfe0a7df87d77db56363ab22c99f94ef35dd4c94a3a437bc7a9cfae` |
| 비공개 initrd | `ed98777ff9c8738cbf48a6c9545716575c61f1399865e0e481604ad98553a4e3` |
| 최종 console | `7f80222052c7cbcd2fdc633029eb06dfbf251c8a9b40e6702b4d55330dfa1a24` |
| 최종 VM 판정기 | `800a6e77c448be572f3f43ee1012f04636e7eb74d7d678e12aa05251189d81ba` |

제품 입력 57개와 산출물 5개를 최종 소스와 대조했습니다. 명령, 초기 실패,
리뷰와 직접 실행 자료는 ignored `evidence/model-bus-review-20260930/`에
보관합니다. 원본 파일·전체 console·역어셈블은 게시하지 않습니다. 기존
컨테이너·고정 도구체인을 사용했고 이번 root 작업에서 새 도구를 설치하지 않았습니다.

주기적으로 fetch하여 외부 `c3e7ecc964aecf1b1b5b562700bc36cfdd375d31`의
[LDS 직렬 입력 기동 조사](https://github.com/junoh-moon/mazda-aa-dr/blob/c3e7ecc964aecf1b1b5b562700bc36cfdd375d31/validation/LDS_INPUT_STARTUP_2026-09-30.md)와
`bd4c2c942b96b8a05f1bb52ac2b95fe3d996e459`의 통합을 확인했습니다.
외부 여섯 VM은 원본 SM·SYSTEM·USB 관리자까지 확장하여 GPIO 모듈 누락과
SYSTEM 상태 응답, LDS의 USB 장치 목록 요청을 확인했습니다. 유효 GPS·
NMEA 소비는 미검증이며 제품 preload/AA caller도 실행하지 않은 조사입니다.
이를 root의 직접 실행으로 세거나 Claude CLI를 새로 실행했다고 주장하지 않습니다.
마지막 fetch의 `b4ea8f2f19a4bd2c09c22adf0a897c5f796bd2c3`도 확인했습니다.
[외부 통합 실행·정리 기록](https://github.com/junoh-moon/mazda-aa-dr/blob/b4ea8f2f19a4bd2c09c22adf0a897c5f796bd2c3/validation/BUS_MODEL_MERGE_2026-09-30.md)은
그 브랜치 제품의 host Python 312개·ARM 및 실제 LDS 요청 14건을 별도로
보고합니다. 추가 도구를 설치했던 외부 전용 컨테이너의 제거와 설치 전후
목록 대조도 완료했다고 기록했습니다. 이 수치·정리는 root의 실행과 별개이고,
이번 동시 표시·수신 시각 수정 전의 외부 제품에 대한 결과입니다.

**TODO:** 기존 분석기는 `model_valid=true`와 state/result/좌표가 서로
모순되는 일부 legacy 행을 허용합니다. 이번 bus 관계 수정과 별개의 기존
의미 검사 공백이며 아직 수정하지 않았습니다. provider/daemon 소유권과
receiver/session 자격, 유효한 원본 GPS 입력·정상 전체 기동·복구, 물리 센서의
단위/품질/시각, 실제 정확도와 휴대폰/앱 수용도 남습니다. 이전 반복 VM 정리
실패나 과거 입력 실패의 원인을 이번 성공으로 닫지 않습니다.
