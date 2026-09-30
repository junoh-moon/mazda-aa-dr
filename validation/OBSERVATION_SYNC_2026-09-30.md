# 세션·요청 경로·버스 관측 통합 — 2026-09-30

NA 74.00.324A 전용입니다. `feat/session-observation`의 `4037b5b`에 외부
master `e79ca52`와 `d0c74d3`를 통합했습니다. 새 작업 브랜치를 만들지 않았습니다.
외부 리뷰와 검증 수치를 이번 직접 실행으로 옮기지 않습니다. 추가 독립
에이전트 리뷰는 수행하지 않았고 차량·폰·동글은 사용하지 않았습니다.

## 통합한 변경과 보존한 동작

- MODEL의 새 세션 경계보다 앞선 receipt뿐 아니라 기존 MODEL 해석의 양수
  transport 시각도 계산에서 제외합니다. 원시 입력과 제외 사유는 저장합니다.
  음수·overflow·미래 transport 및 현재 clock 변경의 기존 fault는 유지합니다.
  이 시각 비교는 물리 생산 시각의 qualification이 아닙니다.
- 분석기에 세션 revision/event 이력, MODEL frontier/경계 시각, byte encoder가
  만들 수 없는 문자열의 검사를 통합했습니다. 이전 로그 형식도 계속 읽습니다.
- 캐시된 세션 문맥의 진행 중 송신, 학습·적용 보정값의 초기화, 입력 원본 보존,
  네 getter의 구별/누락과 긴 JSON 행의 일반 worker 저장 회귀를 반영했습니다.
- 기존 버스 API/단절 signal 관측, 발행·응답 각각의 연결 수명, 원본 송신·errno와
  고정 펌웨어 guard를 유지합니다. 원본 storage의 반환 후 재읽기는 복구하지
  않았습니다. 버스 DSO 회귀와 기존 16개 GOT cold 설치 검사도 유지합니다.

## 이전 체크포인트에서 직접 재현한 결함

`4037b5b`의 runtime/분석기에 강화된 회귀만 적용한 별도 checkout을 사용했습니다.
새 fixture의 세션 revision을 실제 가능한 값으로 맞춘 뒤 실행했습니다.

- `old_transport_new_receipt`는 제외되어야 할 입력 한 건을 계산에 넣었고
  `events=1`로 assertion에 실패했습니다.
- MODEL 로그 8개 검사에서 11개 assertion 실패를 확인했습니다. 제외 기록,
  경계 시각·frontier와 불가능한 revision을 놓쳤습니다.
- 요청 로그 9개 검사에서 35개 assertion 실패를 확인했습니다. callback 이력
  5개와 여섯 문자열 필드의 불가능한 byte 값 30개입니다.

실패 기록은 비공개 evidence에 그대로 남깁니다. 외부 기록의 mutation·리뷰를
이번에 다시 수행했다고 주장하지 않습니다.

## 통합본 직접 검사

`make test`의 호스트 Python 302개와 C/C++ 전체 검사를 통과했으며 skip 0입니다.
동일 제품의 고정 ARM 전체 검사도 통과했으며 skip 0입니다. 실제 제품 DSO의
위치 8개·요청 14개·세션 29개·버스 16개, 초기 생성자 관측과 cold 설치의
25개 권한 실패 경계, 긴 로그의 일반 worker/종료 저장을 포함합니다. 기존
worker 세션 여섯 사례와 새 보정 초기화·센서 입력 회귀도 host/ARM에서 모두
통과했습니다. 원본 공유 runtime에서 작성 세션 DSO 29개·버스 DSO 16개를
별도 통과했으며 아래 원본 JCIDBUS/LDS 실행과 구별합니다.

원본 커널·공유 runtime·LDS·AA를 NIC·호스트 공유 디렉터리·물리 장치가 없는
VM에서 실행했습니다. 이전 버스 검사와 같은 진단 init/caller를 통합 제품에
맞춰 재빌드했습니다. 커널 진입 machine ID 인자를 진단용으로 조정하며 원본
kernel/OEM 파일 바이트는 바꾸지 않습니다. 순정 전체 SM 기동 시험은 아닙니다.

| 원본 실행 | 세션 부재 | 세션 재생성 |
| --- | ---: | ---: |
| 원본 LDS 요청 / 이전 세션을 넘긴 지연 요청 | 3 / 1 | 9 / 1 |
| 지연 요청의 SEND / LOCATION | 0 / 0 | 2 / 1 |
| 지연 LOCATION 반환 / send session lifetime | 해당 없음 | 0 / 2 |
| 합성 raw / 지연 응답 구간 raw | 561 / 192 | 477 / 153 |
| MODEL 경계 / receipt 이전 입력 제외 | 5 / 0 | 6 / 3 |
| journal loss / 요청 loss / 세션 fault / 버스 fault | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |

요청의 네 route 필드와 발행/응답의 동일한 버스 수명, 이전 issue 세션 문맥과
새 송신 세션을 대조했습니다. 제외된 motion 세 건도 epoch·순번으로 원시 행을
찾아 sensor·receipt·transport 필드가 그대로 보존된 것을 확인했습니다.
별도 원본 INVALID callback 두 주기와 실제 daemon 종료도 통과했습니다.
단절 뒤 generation 30→32, disconnected 관측, 원본 close callback 0회와
최종 bus fault 0을 유지했습니다.

모든 원본 LDS 위치 응답과 MODEL 출력은 invalid입니다. 일반 분석기는 양쪽
inconclusive(exit 2), violation 0이며 세션 reset·입력 제외와 합성 source fault가
남습니다. 원본 반환 0이나 이 실행을 정상 폰 연결·터널 정확도 증거로 세지 않습니다.
VM helper는 175초 제한에서 exit 124, QEMU exit 0·timed_out=true입니다.
종료 값 자체 대신 helper 종료 후의 LDS/세션·callback·버스 판정기를 확인했습니다.

## 산출물과 정리

제품 입력 56개와 ARM 산출물 5개의 해시가 통합 소스 및 manifest와 일치합니다.
제품 `libmx5dr.so` SHA-256은
`4d7b66f3cdc78bf18df2107c9816e8aeed66a23c471d721b521b4ccb86a85168`입니다.
비공개 initrd SHA-256은
`741054ecc943ba16838f378c2d5296d5563705b0f6377c03038b0a7421e7205e`,
최종 console은
`d1748f683f2e48147528999ef1b5727ea13aee493cebaaa97dc3319193ff61ea`입니다.
명령·수정 전 실패·실행 자료는 ignored
`evidence/observation-sync-20260930-4037b5b/`에 보관합니다. 원본 OEM 파일,
전체 console 및 주소 자료는 공개하지 않습니다.

추가 도구는 전용 `mazda-observation-sync` 컨테이너 안에만 설치했습니다.
추가 패키지 120개·업데이트 6개와 고정 도구체인 manifest 2124개 항목을
기록하고 검사 후 컨테이너·도구체인·임시 폴더를 제거했습니다. 호스트 패키지와
기존 Docker 이미지·컨테이너 목록은 작업 전과 동일합니다. 제품·재현 소스·
비공개 VM 증거와 도구 목록만 보존하며 설치한 도구 실행 파일은 남기지 않았습니다.

MODEL의 버스 경계 reset, daemon/provider 신원과 요청/session/receiver 자격은
아직 미완료입니다. 실제 센서 단위·품질·시각, 유효한 원본 GPS→터널 계산,
폰/앱 수용과 물리 복구도 입증하지 못했습니다. ASSIST는 계속 비활성이며
공개 설치 ZIP은 변경하지 않았습니다. 이 기록은 v1.0 완성이나 차량 승인이 아닙니다.
