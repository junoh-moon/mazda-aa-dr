# MODEL 계산의 세션 경계 — 2026-09-30

NA 74.00.324A 전용입니다. `feat/session-observation`의 `903e2d7` 이후 변경이며
같은 작업 브랜치를 유지합니다. 과거 실행 횟수·결과는 변경하지 않습니다.

## 문제와 변경

이전 adapter 수정은 송신 후보를 철회했지만 MODEL/holdout 계산기의 기준점과
대기 입력은 남았습니다. 실제 worker를 실행한 수정 전 회귀에서는 재생성 뒤
오래된 기준점·요청으로 `model_valid=true`가 14회 기록됐습니다. 원시 위치
9건, 초기 holdout BEGIN과 새 기준점 이후 정상 계산 5회를 확인한 상태에서
이전 결과의 철회 검사와 세션 ABORT 검사가 실패했습니다.

- 세션 snapshot에 완료된 lifecycle의 64비트 `revision`을 추가했습니다.
  생성 실패, 늦은 callback, 같은 상태값의 callback도 포함합니다. 전환 중에는
  coherent revision을 주장하지 않으며 카운터 소진은 관측 fault로 남깁니다.
- worker는 관측 경계 변경 시 두 계산기, 보정값·후보와 대기 입력을 초기화합니다.
  holdout에는 `session_reset` ABORT를 기록합니다. 동일 lifetime/event를 남기는
  짧은 생성 실패도 revision으로 구분합니다.
- 세션 부재·전환·중복·fault 중에는 MODEL 입력을 중지합니다. 실제 요청 관측이
  없거나 issue revision이 현재와 다르거나 관측 시각 순서가 잘못된 위치는
  계산에서 제외합니다. 원본 POSITION/SEND와 원시 센서 기록은 유지합니다.
- 경계 확인보다 앞선 센서 수신 자료는 새 계산에 재사용하지 않습니다. 회복에는
  이후 센서 입력과 새 GPS 기준점 쌍이 필요합니다. 일반 GPS→GAP 전환은 AA 세션
  경계가 아니므로 정상 터널 MODEL 적분을 초기화하지 않습니다.
- `shadow_session`, `shadow_position_rejected`와 snapshot의 계산 session epoch를
  기록합니다. 분석기는 경계 불일치·잘못된 revision 형식을 검출하고 reset·제외를
  inconclusive로 남깁니다. revision 없는 과거 로그도 읽습니다.

관측된 unique context는 요청의 세션 소유권이나 정상 폰 연결을 증명하지 않습니다.
qualified issue 필드, live `provenance()`와 `allow_assist=false`는 변경하지 않았습니다.

## 검사

| 범위 | 결과와 한계 |
| --- | --- |
| 최종 `make test` | 파이썬 295개와 C/C++ 전체 검사 통과, skip 없음. 기존 stock USB를 packaging fixture로 사용했습니다. |
| worker + 실제 로컬 socket | 파괴, 재생성, 상태 callback, 생성 실패, 중복 세션, 진행 중 callback의 6개 검사. 각 원시 위치 9건 보존, 오래된 MODEL 출력 0, 새 기준점 후 회복. 합성 입력입니다. |
| 고정 ARM 전체 검사 | GCC 4.9.1·softfp, 최종 빌드 다섯 파일을 고정하여 전체 통과. 같은 worker 6개와 실제 제품 DSO의 position 8·request 14·session 22개 포함. |
| 순정 공유 runtime | 최종 제품 DSO로 session 22개 통과. 원본 libc/libstdc++에서 작성한 호출 fixture를 실행했으며 실제 폰 세션은 아닙니다. |
| ThreadSanitizer | 호스트의 진행 중 callback·생성 실패 worker 검사 통과. 실행 프로세스에만 `setarch x86_64 -R`을 적용했습니다. 전체 실행 경로의 무경합 증명은 아닙니다. |
| 로그 분석 회귀 | 새 5개 검사에서 수정 전 16개 assertion 실패를 확인한 뒤 통과. revision 범위·호환성, 초기화·제외 기록, 계산 session 일치 검사입니다. |
| 현재 다섯 바이너리의 USB | 별도 비공개 ZIP으로 packaging 126개 중 124개 통과·2개 skip. ZIP 경로 대신 압축 해제한 bundle 경로를 지정하여 남은 2개도 통과했습니다. 게시하지 않았습니다. |

초기 fixture는 GPS 쌍의 실제 간격이 1초보다 조금 짧을 수 있어 양성 대조가
실패했습니다. 간격을 1.1초로 늘린 뒤 위 수정 전 실패를 확인했습니다. 경계 직후
동시에 넣은 첫 GPS도 후속 센서 수신 근거가 없었으므로 회복 fixture에 그 근거를
추가했습니다. 두 calculator의 시간·센서 조건은 낮추지 않았습니다.

첫 호스트 전체 실행은 병행 ARM 검사와 같은 abstract socket 이름을 사용하여
worker 시작에 실패했습니다(`shadow_boot.active=false`). PID별 시험 socket을
사용하도록 분리한 후 최종 host/ARM 전체 검사를 통과했습니다. 제품의 기본
channel 이름은 그대로입니다. 초기 실패 로그도 비공개 증거에 보존했습니다.

## 원본 LDS/AA 실행

제공된 원본 커널·공유 runtime·LDS·AA를 격리 VM에서 실행했습니다. 제품은 SHADOW
모드이며, 원본 manager가 발행한 요청의 클라이언트 dispatch를 지연해 세션 파괴를
넘긴 응답을 검사했습니다. 원본 응답 본문은 mode·UTC·좌표 모두 0입니다.
센서 자료는 명시적인 합성 channel 입력이며 물리 VBS callback 검사가 아닙니다.

| 실제 경로 | 세션 부재 | 세션 재생성 |
| --- | ---: | ---: |
| 관측한 원본 LDS 요청 | 5 | 12 |
| 파괴를 가로지른 이전 요청 | 1 | 1 |
| 늦은 요청의 계산 제외 사유 | `session_unavailable` | `session_changed_since_issue` |
| 늦은 요청의 원본 send / LOCATION | 0 / 0 | 2 / 1 |
| LOCATION 원본 반환 | 호출 없음 | 0 |
| 보존한 합성 raw | 534 | 492 |
| 지연 응답 전달 구간의 raw | 195 | 141 |
| journal / request loss, session fault | 모두 0 | 모두 0 |

재생성 후의 실제 send 대상은 lifetime 2, 요청 발행 당시 문맥은 lifetime 1이었습니다.
제품은 원본 송신 바이트·반환 계약을 유지하면서 이 응답을 MODEL 입력에서
제외했습니다. 별도 직접 주입한 GPS도 요청 관측이 없어 `request_unobserved`로
제외했습니다. 이 VM에서 유효한 MODEL 위치나 GPS 기준점 회복을 입증하지 않았습니다.
두 로그의 분석 결과는 **inconclusive**이며, 의도한 경계 reset·입력 제외와 합성
motion의 단절이 포함됩니다. 차량 위치 정확도나 폰 수용 판정이 아닙니다.

별도 원본 상태 callback 두 주기에서도 INVALID(0)와 stop 반환 264를 확인했습니다.
생성/상태/파괴의 adapter generation은 4/6/8, 10/12/14였습니다. 합성 시작 인자의
실패 경로이며 정상 연결로 세지 않습니다.

세 호출 프로세스의 반환 0, durable capture 종료와 완료 marker를 검사했습니다.
외부 VM 실행기는 175초 제한으로 124를 반환했고 QEMU 종료 코드는 0입니다.
시간 제한 자체를 성공으로 세지 않았습니다. 이미지 생성 도중 처음 시도한 staging은
입력 변경 검사에서 거부됐으며, 이미지 생성 프로세스 종료 후 다시 실행했습니다.
판정기에서 제외 기록·경계·raw·완료 표식을 없애거나 유효 MODEL 결과를 위조한
변형 5개도 모두 거부했습니다.

## 재현 자료와 남은 범위

- 최종 `libmx5dr.so`: `eae9fcec35ef043b186985741f70c0611a8a1b1052bd66a9f06aee276f073ebd`
- 비공개 VM initrd: `7486116720fc9cf619d51d7e254820131b3ea41da98990ba6120e681a7d6619e`
- VM console: `edcc85125e536625b2b2944ec2d560e07618da8593dfb498718042e1e7c4b968`
- 비공개 증거: `evidence/model-session-20260930-903e2d7/`.
  빌드 입력 53개, 작성 fixture·판정기, 성공/실패 로그와 도구 목록을 보존합니다.

QEMU 7.2.22를 포함한 추가 패키지 120개·갱신 6개와 도구체인 2,124개 blob은
전용 컨테이너 안에서만 사용했습니다. 컨테이너·도구체인·새 VM 이미지·시험 ZIP과
임시 빌드 폴더를 모두 제거했습니다. 호스트 패키지 목록은 그대로이고 기존
Docker 이미지를 보존했습니다. 정리 결과는 비공개 `cleanup.json`에 기록했습니다.
OEM 파일·주소표·전체 console과 시험 ZIP을 공개 커밋에 포함하지 않습니다.

요청별 provider/session/receiver qualification, 물리 센서 단위·시각·품질,
실제 폰·앱 수용과 전원 복구는 여전히 미구현 또는 미검증입니다. 차량·폰·동글
시험을 요청하지 않으며, 이 변경을 v1.0 완성이나 차량 설치 승인으로 표시하지 않습니다.
