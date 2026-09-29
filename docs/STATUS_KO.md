# 현재 상태와 인계 — 2026-09-29

이 문서는 새 리뷰어·LLM의 첫 진입점이다. 과거 상세 설계와 0.1 설치 가능 판단보다 우선한다. 현재 통합 브랜치는 0.2 시험 후보이며 OFF·폴링 분리·일회성 기동 보호를 구현하고 검증했다. master에 병합됐는지는 PR 상태와 구분한다.

## SHADOW 기능 변경

[PC 원본 로그 재생·보정 비교](NAVIGATION_REPLAY_KO.md)는 같은 센서 기록으로 고정/자동 보정을 재계산하고, 같은 기준점의 완료 구간만 비교한다. 센서별 수신 간격 요약도 기존 분석기에 추가한다. 당시 worker 처리 시각을 완전히 복원하거나 실차 검증을 대신하는 기능은 아니다.

후속 [GPS 기준점 검사와 제한된 휠 거리 보정](GPS_WHEEL_MODEL_KO.md)을 추가한다. GPS-visible 구간에서만 보정 근거를 모으고, 비교 구간에서는 적용값을 고정한다. 기존 모델 구조 안의 변경이며 설치 기본값·ASSIST 자격·배포 릴리즈를 바꾸지 않는다.

기존 VBS VIMC 콜백의 센서 입력을 복사하여 실제 SHADOW 항법을 계산하는 경로를 추가했다. [설계·소스 계약·남은 조건](LIVE_SHADOW_2026-09-29_KO.md), [검증 기록](../validation/LIVE_SHADOW_2026-09-29.md)이 이 변경의 기준이다. 아래 OBSERVE 시험 절차는 이미 발행된 v0.2.0-observe.2의 범위이며, 새 SHADOW 변경을 차량에서 확인했다는 뜻이 아니다.

정차 자이로 영점의 자동 추정·새 GPS 기준점에서의 적용과, 별도 GPS 제외 구간의 센서 DR 비교를 추가했다. [상세 계약](SHADOW_CALIBRATION_KO.md), [새 검증 기록](../validation/SHADOW_CALIBRATION_2026-09-29.md)을 따른다. 두 기능은 MODEL 계산이며 ASSIST 자격을 만들지 않는다.

## 목표와 범위

2019 MX-5 ND2 6MT, 1세대 Mazda Connect NA 74.00.324A에서 Android Auto 네이버 지도가 터널에서도 차량 속도·회전에 맞춰 움직이도록 하는 것이 최종 목표다. 대상 사용 환경은 기존 OEM AA touch preload, Galaxy S25, 무선 AA 동글이다. 운전 중 CMU 조작을 요구하지 않는다. 로그는 자동 수집하고 주차 후 회수한다.

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
| 차량 위치 → AA | 해당 펌웨어 정적 분석으로 LOCATION 전달 경로 확인. 실제 휴대폰 수용은 미검증 |
| 순정 DR | NNG의 mode=3 출력과 AA 통과 경로 존재. SD·프로파일·센서·지역 밖 조건에서 활성화되는지는 별도 문제 |
| mode=0 캐시 | 이전 좌표와 속도·방향이 재송신될 수 있음. 현재 OBSERVE/SCRUB은 순정 timestamp(분석 대상에서 0)를 보존. 미활성 ASSIST encoder의 DERIVED timestamp는 별도 wire-clock 검증 대상 |
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
| 첫 주차 OBSERVE 시험 | AA/touch 시작, 정상 hook/health 로그, collector 버스 권한과 별도 로그, 다음 부팅 baseline 복귀 |
| 순정 DR/폰 수용 | native provider의 mode·위치와 폰/앱 로그를 같은 부팅·세션에서 비교. send 성공으로 폰 수용을 대체하지 않음 |
| SCRUB/DROP 비교 | 실제 변형 이벤트·audit 상태로 유효 구간 판정. DROP은 caller/반환값/상태 부작용 분석 후 별도 계약 결정 |
| 자체 ASSIST | 생산자 시각·품질·보정·후진·오차 한계 확보. poll receipt로 대체하지 않음 |

NNG의 순정 DR이 실제로 충분하면 자체 ASSIST보다 기존 경로 확인을 우선한다. SD 없는 구성에서 yaw가 없거나 생산 시각·품질 계약을 확보하지 못하면 **현재 SMDB 기반 ASSIST 경로는 폐기**한다. 정상 차량 LOCATION을 폰/앱이 쓰지 않는다면 CMU 좌표 생성만으로 최종 목표를 해결할 수 없다.

## 검증 기록의 해석

0.1의 호스트/ARM 합성 시험 기록은 [VALIDATION.md](VALIDATION.md)에 보존했다. 공개 이관 검사는 [PUBLIC_IMPORT.md](../validation/PUBLIC_IMPORT.md), 후속 수정 검사는 [INTEGRATION_2026-09-28.md](../validation/INTEGRATION_2026-09-28.md)에 따로 적는다. QEMU는 합성 프로그램을 실행한 것이며 OEM 실행 또는 차량 시험이 아니다. 새 커밋의 검사 결과와 과거 결과를 섞지 않는다.

과거 검토에는 Astra 하위 에이전트가 참여했다. Claude를 실행했다고 주장하지 않는다. 호출 가능한 Claude 경로는 확보되지 않았다.
