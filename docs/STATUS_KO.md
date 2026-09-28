# 현재 상태와 인계 — 2026-09-28

이 문서는 새 리뷰어·LLM의 첫 진입점이다. 과거 상세 설계와 0.1 설치 가능 판단보다 우선한다. 현재 소스는 0.1 기준선이며 아래 수정 사항은 아직 구현하지 않았다.

## 목표와 범위

2019 MX-5 ND2 6MT, 1세대 Mazda Connect NA 74.00.324A에서 Android Auto 네이버 지도가 터널에서도 차량 속도·회전에 맞춰 움직이도록 하는 것이 최종 목표다. 대상 사용 환경은 기존 OEM AA touch preload, Galaxy S25, 무선 AA 동글이다. 운전 중 CMU 조작을 요구하지 않는다. 로그는 자동 수집하고 주차 후 회수한다.

현재 코어의 개발 범위는 짧은 GNSS 단절이다. 기본 제한은 60초·1,500m·추정 오차 100m이며 실제 정확도 보증이 아니다. 지도 매칭과 장터널 대응은 구현하지 않았다. **speed-only 직진 DR은 허용하지 않는다.** 유효하고 신선한 yaw를 증명하지 못하면 자체 DR 송신을 허용하지 않는다.

## 설치 판정

**OBSERVE를 포함해 실차 설치를 보류한다.**

1. OFF/잘못된 설정/disable 표식을 확인하기 전에 BLM에 RTLD_NOW를 강제한다.
2. RTLD_NOW 실패 시 원래 flags로 돌아가는 로드 경로가 없다.
3. 일반 크래시와 preload 자체의 로더 실패에 대한 재부팅 반복 복구가 없다.
4. 관찰용 D-Bus/SMDB 폴링이 OEM AA 프로세스 안에서 동작한다.

OFF 처리와 폴링 분리의 수정 방향은 확정적이다. 재부팅 복구는 필요성은 확정됐지만 실제 CMU 기동 경로에서 외부 보호 장치를 적용할 위치·방식은 아직 확정하지 않았다. 앞 두 가지를 고쳤다는 이유만으로 설치 보류를 해제하지 않는다.

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
- `src/runtime/`: dlopen 경계 설치, bounded queue/journal, 자동 관찰. 문제가 있는 in-process 폴링도 그대로 남아 있음.
- `packaging/`: 펌웨어 해시 검사, 서비스별 preload 토큰 추가·제거, 기존 touch 항목 보존. source baseline의 검토 대상이며 설치 승인 아님.
- `tools/analyze_logs.py`: 실제 이벤트·health·drop 등 분석. `audit_fault`를 이미 불완전 실험으로 판정함.

SCRUB은 mode=0 캐시 LOCATION에서 `hasSpeed=false`, `hasBearing=false`로 만들고 값도 0으로 지운다. 유효한 speed=0을 제공하는 것과 다르다. 오래된 좌표는 계속 남는다. SHADOW는 관찰만 하는 예약 모드다. ASSIST는 `allow_assist=false`와 항상 false인 provenance 검사 등으로 차단되어 있다.

## 다음 작업과 종료 경로

| 작업 | 완료 조건 |
| --- | --- |
| OFF/로더 수정 | 설정·disable 판정을 대상 부가 로딩 전에 수행. NOW 실패 시 원 flags 1회 재시도 후 후크 포기. 오류/errno/다음 호출 계약 테스트 |
| 폴링 분리 | 별도 프로세스에 D-Bus/SMDB 관찰 이동. AA에는 후크·ring·writer만 유지. 관찰 실패가 AA로 전파되지 않음 |
| 외부 복구 설계 | preload 매핑 전 보호 위치 확인. 조기/지연 크래시와 로더 실패 후 다음 시작에 원래 경로로 복귀하는 증거 |
| SCRUB/DROP 비교 설계 | 실제 변형 이벤트·audit 상태로 유효 구간 판정. DROP은 caller/반환값/상태 부작용 분석 후 별도 계약으로 구현 여부 결정 |
| 센서/순정 DR/앱 검증 | 생산자 시간·품질과 앱 수용을 독립적으로 증명. 단순 poll 값이나 send 성공으로 대체하지 않음 |

NNG의 순정 DR이 실제로 충분하면 자체 ASSIST보다 기존 경로 확인을 우선한다. SD 없는 구성에서 yaw가 없거나 생산 시각·품질 계약을 확보하지 못하면 **현재 SMDB 기반 ASSIST 경로는 폐기**한다. 정상 차량 LOCATION을 폰/앱이 쓰지 않는다면 CMU 좌표 생성만으로 최종 목표를 해결할 수 없다.

## 검증 기록의 해석

0.1의 호스트/ARM 합성 시험 기록은 [VALIDATION.md](VALIDATION.md)에 보존했다. 이 공개 이관에서 실행한 검사는 [PUBLIC_IMPORT.md](../validation/PUBLIC_IMPORT.md)에 따로 적는다. QEMU는 합성 프로그램을 실행한 것이며 OEM 실행 또는 차량 시험이 아니다. 새 커밋의 검사 결과와 과거 결과를 섞지 않는다.

과거 검토에는 Astra 하위 에이전트가 참여했다. Claude를 실행했다고 주장하지 않는다. 호출 가능한 Claude 경로는 확보되지 않았다.
