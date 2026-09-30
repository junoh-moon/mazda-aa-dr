# 독립 세션 진단 검토와 직접 재실행 — 2026-09-30

독립 브랜치의 세션 진단·상태 조회 회귀를 소스까지 검토하여 통합했습니다.
진단 판정기의 거짓 통과와 작성 콜백 검사의 결손을 수정한 뒤 원본 커널 VM에서
직접 두 번 실행했습니다. 각 실행의 두 세션은 생성·송신·시작 API가 0을 반환했지만
실제 상태는 `INVALID(0)`, 상세값은 -1이었습니다. **API 반환 성공과 정상 연결
상태는 같은 증거가 아닙니다.**

제품 `src/`, 설치 기본값과 공개 ZIP은 이번 단위에서 변경하지 않았습니다.
새 probe는 진단 전용이며 USB에 설치되지 않습니다. ASSIST와 qualified provenance는
비활성이고 v1.0은 미완료입니다. 폰 수용이나 전체 애플리케이션 기동·종료를
검증한 것으로 세지 않습니다.

## 확인한 독립 작업과 출처

SSH fetch로 다음 커밋과 validation 기록을 읽었습니다. 원래 기록의 실행 횟수나
판정을 수정하지 않았으며, 이번 직접 검증은 아래에 별도로 기록합니다.

- `bcf69f4`: [제공 펌웨어 재생](https://github.com/junoh-moon/mazda-aa-dr/blob/bcf69f44798e56fc97865dc59fb37af4eb75c062/validation/FIRMWARE_REPLAY_2026-09-30.md).
  부분 OEM 실행, mode 0/READ_NOT_READY, 물리 센서와 LOCATION 표본 부재를 구분했습니다.
- `76ea17a`: [원본 세션 callback 진단](https://github.com/junoh-moon/mazda-aa-dr/blob/76ea17ac922b454210b578008c8ea96ea664a2f0/validation/SESSION_EVENTS_2026-09-30.md).
  원본 세션 API와 callback의 차이를 관찰하는 probe·전용 init·이미지 빌더·판정기가
  이번 진단 통합의 원형입니다. 기존 11개 Python 회귀도 가져와 보강했습니다.
- `098662b`: [상태 조회 병행 회귀](https://github.com/junoh-moon/mazda-aa-dr/blob/098662b31ffccf73a7980dece062ac36a2c6c0aa/validation/REQUEST_STATUS_POLL_2026-09-30.md).
  두 health reader와 2,000회 요청 lifecycle 검사를 가져와 host/ARM suite에
  연결했습니다. 중단 없는 hang을 판정 성공으로 기다리지 않도록 테스트에만
  60초 alarm을 추가했습니다. 제품의 잠금 분리 구현은 기존 `05ddd5f`입니다.
- `fb8f215`의 펌웨어 기반 개발 지침도 읽었습니다. 이 작업은 위 독립 브랜치의
  전체 병합이나 시험 패키지의 차량 설치 요청이 아닙니다.

위 자료는 사용자께서 알려 주신 독립 작업의 기록입니다. 이번 단위에서 주
에이전트가 Claude를 직접 실행했다고 주장하지 않습니다.

## 재현하여 수정한 판정 오류

1. NULL 상태 payload를 callback 관측 횟수만으로 알려진 상태 0으로 표시했습니다.
   실제 빈 포인터 fixture가 `session_state_observed=true`, `states=[0]`으로
   승인되는 것을 먼저 확인했습니다. probe는 이제 state/detail에 null을 기록하고,
   판정기는 `data_nonnull`을 확인합니다. 과거의 false/숫자 0 형태도 unknown입니다.
2. start·stop·identity가 없는 기록도 local cycle 완료로 처리했습니다.
   두 cycle의 전체 작업 순서, 명시적인 callback 개수, scope와 필드 타입을
   검사하도록 바꿨습니다. bool을 정수 반환값·식별자로 받아들이지 않습니다.
3. 독립 리뷰에서 RC=0의 위치를 버리는 거짓 통과를 추가로 발견했습니다.
   scope 앞·cycle 중간·complete 앞의 RC 세 사례가 모두 승인됐습니다.
   해당 기록의 마지막 행 뒤에 나온 단 하나의 정상 종료값만 인정하게 했습니다.
   온전한 RC 앞의 OEM 접두어도 읽으며 139나 `0 trailing`은 거부합니다.

event 번호는 원자 증가 시 배정되고 실제 출력은 뒤에 실행됩니다. 따라서 동시
callback 번호의 출력 순서를 강제로 정렬하지 않습니다. 전역 번호의 집합·개수와
각 callback 진입/복귀 대응을 검사합니다. 늦은 이전 세대의 callback도 원래
cycle에 보존합니다. 실제 callback 0건은 local API 시도의 완료와 구분하여
상태 미확인으로 보고합니다.

start/stop의 원본 반환값은 결과에 별도로 남습니다. 비zero stop 반환을 0으로
고치거나 정상 연결로 판정하지 않습니다. `complete=true`는 정해진 로컬 API
진단의 실행 기록이 완결됐다는 뜻이며 phone/vehicle 판정은 항상 false입니다.

## 작성 probe와 회귀 보강

복사한 76바이트 callback table을 생성별 정적 문맥에 보관합니다. 원본 API가
테이블을 복사하는지에 의존하지 않고 진단 종료까지 수명을 유지합니다. 두 세대의
문맥을 재사용하지 않으며 NULL userdata와 전체 SessionInfo 포인터를 전달합니다.
이 제한된 두 세대 fixture를 제품의 일반적인 세션 수명 구현으로 쓰지 않습니다.

새 ARM 합성 회귀는 실제 작성 probe 소스를 포함하되 OEM main을 실행하지 않습니다.
테이블을 보관하는 작성 API, 지연된 이전 세대 callback, NULL payload와 errno를
검사합니다. 독립 리뷰에서 처음 fixture의 원본 역할 함수가 모두 0을 반환하고
포인터 동일성만 확인하는 공백을 찾았습니다. 서로 다른 양수·음수 int32 반환과
520바이트 전체 payload를 진입/복귀 양쪽에서 대조하도록 보강했습니다.

최종 ARM 검사에서 create/destroy의 반환을 0으로 고정한 두 변형과 callback
전·후의 payload 마지막 바이트를 바꾼 두 변형을 모두 assertion 종료 134로
거부했습니다. 잘못된 코드를 실제로 거부한 근거이며 OEM 자체를 변형한 것이 아닙니다.

## 이번 직접 실행과 고정 입력

기준 소스는 `1884228b87f015cfc3ca758254075058918896e6`와 이번 작성 진단 변경입니다.
주 에이전트가 기존 격리 컨테이너에서 고정 GCC 4.9.1로 새 probe를 빌드하고,
원본 NA 74.00.324A 커널/rootfs·aap_service로 같은 이미지를 두 번 실행했습니다.
guest NIC·호스트 장치·공유 디렉터리가 없고 userspace debugger도 없습니다.
진단 PID1이며 정상 OEM 전체 boot가 아닙니다.

시작 입력은 독립 진단과 동일한 **304바이트 합성 0값**입니다. 실제 장치 연결
정보가 없고 원본 큐·LDS·제품 request hook을 시작하지 않습니다. 공통 빌더가
요구하는 기존 공개 USB 묶음은 이미지에 포함되지만 이 init은 설치기나 preload를
실행하지 않습니다. API send의 합성 48바이트 LOCATION도 물리 위치가 아닙니다.

| 관측 | r1 | r2 |
| --- | --- | --- |
| 생성·송신·시작 반환 | 각 2건 모두 0 | 동일 |
| 실제 상태 callback·복귀 | 2/2 | 2/2 |
| state/detail | 0 / -1 두 건 | 동일 |
| userdata 보존·non-NULL payload | 두 건 모두 확인 | 동일 |
| stop 반환 | 264 두 건 | 동일 |
| destroy 반환·핸들 | 0·NULL 두 건 | 동일 |
| 프로그램 종료·판정 | 0·local complete | 동일 |

두 runner의 제한 종료 124와 QEMU 종료 0은 성공 판정 근거가 아닙니다.
종료 후 회수한 console 해시를 metadata와 대조하고 최종 판정기로 검사했습니다.
원본의 transport·semaphore 등 오류가 없는 정상 연결/정리라고 주장하지 않습니다.
서비스 로그는 진단 init이 읽은 범위이며 서비스 전체 수명의 로그가 아닙니다.

| 최종 입력 또는 결과 | SHA-256 |
| --- | --- |
| ARM probe | `a259a57f8fa20a028aba0fdf52f3fe5c1322d5eef7521a63cc0bcb90593a8141` |
| 두 실행의 initrd | `61dc7e140aa842a81d88cea43dc4923e54079836f53251900c378d45f5fe4036` |
| r1 console | `039ac9523ad837b7fcb490841bab99481e0bc275e5b6f7b9424b4f7da4e77262` |
| r2 console | `b1f045fb3bfefe545de4c6414bb74f7e4803abbcc1462a2a346eb4da090fc3e2` |
| 최종 판정기·실행기 | `37406fa5ccc8537a21563e7980f707987af914c01b87d2c78ad7c605b3f2ca5d` |

r1 실행 중에는 host 판정기와 합성 callback 시험만 후속 수정했습니다. probe와
initrd는 불변이며 r1도 수정된 최종 판정기로 검사했습니다. r2는 최종 실행기
소스 snapshot을 고정하여 실행했습니다. 모든 판정 수정 전 실패는 별도로 보존했습니다.

## 전체 검사와 독립 리뷰

- `make test`: Python **289개**(41/53/28/1/10/126/30)와 C/C++ 통과,
  생략 0입니다. 이전 281개 또는 287개 결과를 소급 고치지 않습니다.
- 고정 도구체인으로 제품 다섯 개를 새로 빌드하고 전체 ARM suite를 생략 없이
  통과했습니다. 이후 강화한 callback fixture는 root와 독립 ARM 리뷰에서
  다시 빌드·실행했습니다. 제품 DSO의 위치 예외/취소 8개·요청 wrapper 13개도
  전체 suite에 포함됩니다.
- 새 제품 `libmx5dr.so`는 `ed6821129a3b437e484a212a546c96a81259578f97ccbd5dea337fd041376832`로
  [journal 수정](JOURNAL_QUEUE_2026-09-30.md)의 최종 제품과 같습니다.
- 상태 조회 회귀는 native·ASan/UBSan·TSan과 ARM에서 각각 2,000회를
  무손실 완료했습니다. 독립 리뷰는 같은 회귀를 수정 전 `ccfb255`의 작성 Ledger에
  실행하여 세 번 모두 첫 주기의 실제 실패를 검출했습니다. 스케줄 의존 검사이며
  매 주기의 특정 겹침이나 모든 실행 순서를 증명하지 않습니다.
- 상태 count를 항상 0으로 만든 변형은 이 stress를 통과했지만 함께 연결된
  기존 `test_request_trace`가 거부했습니다. 새 검사 단독의 범위를 확대하지 않습니다.
- 판정기 독립 리뷰의 별도 작성 fixture 10개와 저장소의 19개가 최종 통과했습니다.
  네 번째 통합 리뷰도 별도 기록 변형 147개와 합성 빌더 선택/복사 검사 두 개를
  통과했습니다. 합성 빌더 검사는 실제 원본 archive 실행과 구분합니다.

판정기, 회귀 품질, 고정 ARM ABI, 최종 통합의 네 독립 리뷰를 완료했습니다.
발견한 종료 표식의 P2와 callback 시험의 두 결손을 수정·재검증했고 최종 검토
범위에서 미해결 P1/P2는 발견하지 못했습니다. 리뷰어들은 공개 작성 코드와
합성 입력만 검토했으며 원본 VM 실행·로그는 주 에이전트가 직접 담당했습니다.

마지막 staged diff 검토에서 순서 오류 fixture의 callback 개수도 틀린 것을
확인했습니다. 개수를 실제 기록과 맞추고 실패 사유가 정확히 callback_order인지
대조하도록 수정했습니다. 실행기·probe·제품은 불변이며, 보강 후 GNU host의
해당 Python 회귀 19개를 다시 실행했습니다. 독립 추가 검토는 순서 검사만
제거한 변형이 이전 회귀를 통과하고 보강된 회귀에서는 실패함을 확인했습니다.

macOS에서 기존 회귀의 `/bin/true` 가정이 실패하여 현재 Python 실행 파일을
잘못된 ARM 입력 예제로 쓰도록 수정했습니다. 초기 실패 로그를 보존했습니다.
호스트/컨테이너에 패키지를 설치하지 않았으며 기존 도구체인·컨테이너를 사용했습니다.
비공개 원본 입력·console·명령·소스 snapshot·실패·리뷰 원문은
`evidence/independent-intake-20260930/`에 보존했습니다.

## 남은 구현 경계

원본 API의 NULL userdata만으로 세션 수명을 식별하지 않습니다. 현재 제품의
`Observer::request_begin`은 세션 자격이 없는 Issue를 계속 만듭니다. 생성별
수명·실제 receiver 상태와 요청 발행 시점의 연결은 아직 구현하지 않았습니다.
늦은 응답에 당시 상태 대신 최신 전역 상태를 붙이는 방식으로 대신하지 않습니다.

이번 진단은 물리 센서의 단위·부호·생산 시각, 정상 연결 사건, 폰/앱 수용과
전체 기동·물리 복구를 검증하지 않았습니다. 해당 범위의 미확인을 유지하며
동작 중인 차량 설치나 주행 시험을 요청하지 않습니다.
