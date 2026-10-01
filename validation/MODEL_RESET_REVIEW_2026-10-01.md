# MODEL 초기화 원인 보존 검토 — 2026-10-01

대상은 1세대 Mazda Connect NA 74.00.324A의 SHADOW 시험 소스입니다.
`feat/session-observation`의 `d00c74d`와 `a29f1b8`을 검토했으나 브랜치를
그대로 병합하지 않았습니다. 주 계산기의 fault 직후 정상 raw가 들어오면
`status.result`가 OK로 바뀌어 초기화 사유를 잃는 문제를 수정했습니다.

## 변경과 경계

- `navigation.status().resets`가 raw·POSITION·`drain()` 경계에서 증가할 때
  사유, 동작, 해당 입력 순번과 단조 시각을 별도 `shadow_pipeline_reset`
  행에 기록합니다. raw는 원래 `motion_batch`를 먼저 flush한 뒤 원인
  진단을 기록합니다. 이 순서가 깨지는 초기 구현은 회귀 시험에서 실패했고
  수정 후 통과했습니다. journal 실패까지 원본의 영속성을 보장하는 것은
  아닙니다.
- `shadow` 행의 `drain_calls_total`은 worker가 실제 `drain()`을 부른
  누적 횟수입니다. 빈 큐의 호출도 세며, 입력 처리·유효 위치·ASSIST
  자격을 뜻하지 않습니다. 기존 `events`는 여전히 위치를 포함한 큐 삽입
  누적값입니다.
- 주차 상태 명령은 reset 뒤 이전 MODEL 해를 철회하고 마지막 사유를
  표시합니다. PC 분석기는 reset을 이름 있는 불충분 사유로 기록합니다.
  raw의 `input_ns`는 수신 시각, POSITION은 관측 시각, drain은 watermark로
  해석합니다. 원본 센서의 측정 시각/품질로 승격하지 않습니다.
- 외부 브랜치의 일률적 8 MiB 저장 공간 임계값과 설치 사전 검사는
  도입하지 않았습니다. 실제 `/mnt/data_persist` 파일시스템의 여유 공간·
  쓰기/회전 동작을 측정하지 않은 상수를 사용자 설치 차단 조건으로
  만들 근거가 없습니다. 현재 로그의 유한 보관량과 저장 실패 가능성은
  그대로 명시합니다. 설치 명령은 `sh install.sh`입니다.

## 직접 실행 근거

수정 전 journal에서 fault 행 부재, 상태 명령의 옛 MODEL 해 유지, 분석기의
미지 기록 판정을 각각 실패로 재현했습니다. 최초 수정의 raw/원인 순서
오류도 회귀에서 실패했습니다. 수정 후 아래 검사를 직접 통과했습니다.

| 검사 | 결과와 범위 |
| --- | --- |
| 전체 `make test` | Debian 12, 원본 펌웨어 fixture와 수정된 개발 묶음 지정. 종료 0, skip 0, Python 350개와 C/C++ 실행 통과. |
| 전체 고정 ARM/QEMU | GCC 4.9.1, 고정 sysroot, 다섯 새 빌드 입력의 `release_verified=true`와 해시 일치. navigation 2,740, bias 2,677, GPS/wheel 84,601, holdout 5,443 합성 검사 및 실제 배포 DSO의 position/request/session/bus 검사 통과. |
| 순정 ARM BusyBox 1.19.2 상태 파서 | 원본 BusyBox `awk`를 QEMU로 실행한 상태 표시 30개 통과. |
| 순정 BusyBox/libc 설치 에뮬레이션 | 수정 소스의 개발 ZIP을 풀어 손상 USB 거부, SHADOW 설치, loader, 일회 기동, collector UID/종료, 상태·종료·회수, touch 변경 후 재예약, 제거·재설치 통과. 마운트와 센서 행은 합성. |

이 검사의 개발 ZIP은 dirty source에서 생성되어
`build-info.json`의 `source_modified=true`입니다. 배포 파일로 게시하지
않았습니다. Claude Code CLI에 이번 diff의 읽기 전용 검토를 요청했지만
90초 제한 안에 출력이 없어 종료했습니다. 독립 리뷰 통과로 세지 않습니다.
외부 브랜치 기록은 설계 참고이며 이번 직접 실행으로 합산하지 않습니다.
원본 OEM 서비스 전체 기동, 물리 센서, 실차 복구,
Galaxy S25·무선 AA·네이버 지도 수용은 이번 변경으로 검증되지 않았습니다.
ASSIST는 계속 비활성입니다. 원본 바이너리·원본 로그·개인 링크는
공개하지 않습니다.
