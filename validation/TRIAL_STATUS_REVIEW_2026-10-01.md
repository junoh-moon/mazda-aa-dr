# 주차 중 수집 상태와 MODEL 진단 분리 — 2026-10-01

기준 master `ad252cd`의 `trial_status.sh`는 현재 부팅의 가드·hook·원시
수신·collector·audit가 모두 보이면 종료 코드 0을 냈습니다. 계산이
비활성이거나 `shadow` 기록이 하나도 없는 합성 사례도 0으로 끝났습니다.
이 상태를 항법 계산이 진행된 증거로 읽지 않도록 출력 계약을 고쳤습니다.
원시 수집을 중단하거나 ASSIST를 켜는 변경은 없습니다.

외부 `feat/session-observation`의 `9f6e584` 상태 진단 변경을 검토했고,
Claude Code에 **읽기 전용**으로 해당 패치와 제품 journal 계약을 비판적으로
대조하도록 요청했습니다. Claude는 파일을 수정하거나 명령을 실행하지
않았습니다. 세 차례 검토에서 나온 문제를 root가 제품 소스·원본 BusyBox로
직접 확인해 다음 과장을 수정했습니다.

- `shadow.events`는 `Pipeline::insert()`의 **누적 대기열 삽입** 횟수이며
  POSITION도 포함합니다. `reset()` 뒤에도 합계가 남습니다. 따라서
  입력 처리 완료나 현재 세션의 센서 계산 성공이라고 표시하지 않고
  `events_queued_total`로만 출력합니다.
- `motion_rejected.checked_ns`는 worker가 거부를 확인한 시각입니다.
  이 값이 최근이라는 이유로 오래된 센서 수신을 새 수신으로 표시하지
  않습니다. 더구나 런타임에서 `future`로 거부한 `received_ns`가 상태
  확인 시각에는 과거가 되어 성공으로 보일 수 있었습니다. 수집 성공에는
  수락된 `motion_batch`만 사용하고, 거부된 datagram은 센서별 worker
  확인 진단과 부팅 중 거부 기록으로만 표시합니다. `received_ns`도 물리
  센서 생산 시각의 증거가 아닙니다.
- `capture_end`·`capture_incomplete`, 세션·버스 재설정과 입력 재설정
  이후의 직전 MODEL 해를 현재 해로 남기지 않습니다. `gps_anchor_gate`와
  거부·제외 이유는 관측 시간 범위를 표시하고, 시각 0의 clock 장애는
  별도 무시각 진단으로 보존합니다.
- journal이 조용히 실패하거나 worker가 멈추면 종결 행 없이 마지막 정상
  health가 남습니다. 기존 30초 창으로는 수집이 이미 멈춘 뒤에도 성공할
  수 있어 runtime health를 5초, collector poll을 8초, MODEL 진단·해를
  2초로 줄였습니다. runtime health는 약 1초 주기로 flush되고 collector는
  기본 1초 대기와 제한된 호출 timeout을 사용합니다. 이 창은 진단용
  선택이며 실제 장치의 최악 지연 보증이 아닙니다. 조용한 중단 직후의
  5초 안팎은 여전히 기록만으로 완전히 배제할 수 없습니다.

`oem_position_recent`, `computation_active`, `model_diagnostic_recent`,
`model_solution`과 최근 거부·기준점 이유를 별도 출력합니다. 위치의
mode 0도 단지 수신 관측이며, MODEL 진단의 `observed`도 센서 자격이나
앱 수용을 뜻하지 않습니다. 상태 명령의 종료 코드는 기존 **원시 수집
근거**만 판정합니다. AA 세션·버스가 아직 가용하지 않아 계산이 멈춰도
수집 근거가 있으면 0이고, 계산 불가 이유는 정보 줄에 남습니다. 수락된
원시 수집이 없거나 audit/drop이 있으면 계속 실패합니다.

## 재현과 검증

- 변경 전 원시 수집만 있고 `computation_active=false`, `shadow` 없음인
  사례가 종료 코드 0임을 확인했습니다. 외부 후보의 새 시험을 기존
  도구에 먼저 적용했을 때 19개 중 7개가 실패하여 누락된 진단을
  재현했습니다. 기존 Linux 상태 시험 12개는 통과했습니다.
- 최종 상태 시험 28개가 Debian 12의 일반 awk와, 같은 컨테이너의
  QEMU-user에서 실행한 **원본 CMU BusyBox 1.19.2 awk**에서 각각
  통과했습니다. 오래된 수신·미래 시각 거부·세 센서 모두 거부된 경우,
  MODEL 진단 부재·형식 오류·대기, 이전 유효 해 뒤 최신 무효 해와
  수집 종료·재설정, 조용한 기록 중단의 시간 창을 구별했습니다.
- 실행 중인 host collector의 두 번째 poll이 고정된 시험용 `/proc/uptime`
  뒤에 추가되어 원본 awk 시험 한 번이 실패했습니다. 이 시험은 실제
  collector가 출력한 첫 poll까지의 원문을 고정 snapshot으로 재생하도록
  고쳤고, 일반/원본 awk 시험을 모두 다시 통과했습니다. 실제 장치에서
  동시 journal 회전·추가 쓰기의 모든 경합이 없다는 증명은 아닙니다.
- 수정한 시험 묶음으로 `make test`가 Debian 12 컨테이너에서 종료 코드
  0을 냈습니다. 최종 실행에는 packaging 142개와 tools 30개가 포함됐고
  기록된 Python 시험에 skip이 없습니다. 기존 제품 ARM 바이너리와 변경된
  상태 helper를 별도 시험 묶음에 넣어 원본 ARM BusyBox/libc 설치·가드·상태·수집 종료·
  회수·제거 `cmu_emulation.py`를 통과했습니다. 이 시험은 mount를
  모델링하고 OEM 서비스·실차·폰을 실행하지 않습니다.

macOS에서 상태 시험을 직접 실행한 초기 시도는 Linux `/proc`가 없어서
실제 collector 사례 한 개가 오류로 끝났습니다. 위 Linux 컨테이너
검사로 해당 환경 차이를 확인했습니다. C/C++ 제품 소스와 다섯 ARM
바이너리는 변경하지 않아 이 단위에서 새 ARM 빌드·suite를 실행하지
않았습니다. 이번 시험 묶음은 배포 ZIP이 아니며 공개 릴리즈도 그대로입니다.

`trial_status.sh`의 성공은 센서 생산 시각·품질, 정상 전체 CMU 기동,
차량 위치 정확도, 다음 전원 주기 복구, Galaxy S25/동글/네이버 지도 수용을
확인하지 않습니다. [통합 시험 준비](../docs/FIELD_TRIAL_KO.md)와
[v1.0 완료 조건](../docs/V1_READINESS_KO.md)을 유지합니다.
