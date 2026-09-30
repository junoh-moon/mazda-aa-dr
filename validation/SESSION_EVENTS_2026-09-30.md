# 원본 AA 세션 상태 콜백 실행 — 2026-09-30

NA 74.00.324A 원본 `RaceAap::Init/UnInit`, AA interface API, aap_service를
두 격리 VM에서 실행했습니다. 각 VM에서 세션을 두 번 생성·파괴했습니다.
생성·송신·시작 반환은 모두 0이었지만 실제 상태 콜백은 `INVALID(0)`,
상세값 -1이었습니다. **API 반환 0과 동작 중인 세션을 구분해야 한다는
실행 근거**입니다. 제품 ASSIST 연결이나 폰 수용 완료로 세지 않습니다.

## 작성 코드와 시험 조건

[재현 절차](../tests/packaging/OEM_SESSION_PROBE.md), 작성한 ARM
[진단 프로그램](../tests/packaging/oem_session_probe.cpp), 전용 guest init과
`oem_system_emulation.py build --session-probe` 및 `check-session`을 추가했습니다.
제품 소스·기본 모드·설치 ZIP은 변경하지 않았습니다.

- 제공된 archive에서 rootfs를 다시 꺼내 이전과 동일한 SHA-256
  `61bbcea608cc915f45a1775d4f49fb1603e4d92a0163311ced15220d03cf9d44`를 확인했습니다.
  원본 kernel SHA-256은 `57fe0cda7c1f994a6ca6b3cce7d9cf247f3b5607ebf1ddc12f02598662d5e240`입니다.
- QEMU 7.2.22/Sabrelite, 원본 Linux 3.0.35·libc·C++ runtime을 사용했습니다.
  guest에 호스트 장치·네트워크·공유 디렉터리를 연결하지 않았습니다.
  kernel-entry hardware breakpoint에서 machine ID 레지스터만 조정한 뒤
  debugger를 분리했습니다. OEM 코드 바이트는 수정하지 않았습니다.
- 고정 GCC 4.9.1·ARMv7 softfp로 프로그램을 빌드했습니다. 원본 BLM·interface·
  common-util의 파일 해시, 매핑·export 위치·사용 함수 진입값을 검사했습니다.
  원본 singleton/getter로 RaceAap 객체를 얻었으며 객체 내부를 덮어쓰지 않았습니다.
- 진단 프로세스의 create/destroy GOT 호출을 관찰했습니다. 원본이 만든 76바이트
  콜백 테이블에서 상태 콜백 한 항목을 생성별 wrapper로 교체했습니다.
  나머지 항목, NULL userdata, 원래 SessionInfo 포인터와 원본 호출 횟수를
  보존했습니다. wrapper 두 개의 문맥은 재사용하지 않았습니다.
- 시작 API의 입력은 **304바이트 합성 0값**입니다. 실제 장치 연결 정보가 없으며,
  원본 서비스에도 device type 0과 transport 시작 실패가 기록됐습니다.
  폰 미연결만의 효과를 분리한 시험 또는 정상 폰 연결의 재현이 아닙니다.

## 두 실행의 결과

| 관측 | r1 | 최종 코드·공통 빌더를 사용한 r2 |
| --- | --- | --- |
| create 반환·핸들 | 2건 모두 0·non-NULL | 동일 |
| 송신 반환 | 2건 모두 0 | 동일 |
| start 반환 | 2건 모두 0 | 동일 |
| 원본 상태 callback·복귀 | 생성별 1건, 총 2/2 | 동일 |
| callback state/detail | 0 / -1 두 건 | 동일 |
| callback userdata | 두 건 모두 원래 NULL | 동일 |
| stop 반환 | 두 건 모두 264 | 동일 |
| destroy 반환·핸들 | 두 건 모두 0·NULL | 동일 |
| 진단 프로그램 종료 | 0, 완료 표식 있음 | 동일 |

r2에서 두 번째 생성은 같은 핸들 저장소 주소를 사용했습니다. 실제 할당된
핸들 주소는 첫 생성과 달랐습니다. 주소 재사용을 재현했다고 세지 않습니다.
상태 콜백의 사용자 문맥은 두 생성 모두 NULL이므로 그것만으로 생성 수명을
식별할 수 없습니다. 이후 제품 연결에는 생성별 식별자와 요청 발행 시점의
상태 보존이 필요합니다. 늦은 이전 세션 콜백의 재현·동시 생성은 미검증입니다.

원본 큐는 시작하지 않았으며 callback이 게시한 원본 worker의 실행·LDS 요청·
제품 요청 관측 hook은 이번 범위 밖입니다. 원본 서비스의 transport·Bluetooth·
video 및 semaphore 오류를 로그에 보존했습니다. 파괴 API 반환 0과 핸들 NULL은
오류 없는 전체 애플리케이션 종료, thread 정리나 누수 부재의 증명이 아닙니다.
서비스 로그도 실행 중 읽은 범위이며 최종 전체 로그라고 주장하지 않습니다.

바깥 runner는 각 115.170/115.169초 뒤 제한 종료 124, QEMU는 0이었습니다.
이 종료값을 성공 기준으로 사용하지 않았습니다. 검사기는 두 생성·송신·파괴
주기와 callback 진입/복귀 대응, 명시적 fixture 종료 0을 판정했습니다.
`complete=true`는 그 범위의 완료이며 `phone_acceptance_verified=false`,
`vehicle_validation=false`를 유지합니다. stop 264는 원본 반환으로 남아 있습니다.

## 판정기 수정과 회귀

첫 판정기는 줄 시작의 JSON만 읽었습니다. r2에서 원본 SDK의 줄바꿈 없는
접두어 뒤에 완전한 callback JSON이 붙어 한 건을 놓쳤고, callback 대응·개수
검사에서 실패했습니다. 이 실패를 보존하고 같은 배치를 호스트 회귀로 만들었습니다.
접두어 뒤의 완전한 JSON을 읽도록 수정했으며, 본문 손상·잘림은 계속 거부합니다.

새 판정기/입력 검사 11개는 성공 반환의 과대해석, 완료/종료 누락, callback
식별자·순서·개수 오류, 혼합 로그, 잘못된 ARM 입력과 위 접두어를 검사합니다.
실패를 먼저 확인한 뒤 구현했습니다. 최종 `make test`는 Python **281개**와
기존 C/C++ 검사를 생략 없이 통과했습니다. 순정 BusyBox의 shell 문법 검사와
새 공통 빌더의 실제 이미지 생성·r2 실행도 완료했습니다.

제품 ARM 전체 회귀는 이번에 재실행하지 않았습니다. 제품 코드 변경이 없으며,
이번 ARM 근거는 작성 probe의 고정 컴파일러 빌드와 원본 runtime VM 실행입니다.
앞선 제품 ARM 결과는 [펌웨어 재실행 기록](FIRMWARE_REPLAY_2026-09-30.md)을 따릅니다.
실차·폰, 새로운 독립 에이전트 리뷰는 사용하지 않았습니다.

## 자료와 도구 정리

두 VM 실행의 소스 기반은 `2b959ff713a259bebe36ebe7f7dfd8da9ebed449`이며
새 진단 코드는 당시 작업 트리 변경입니다. 작성 소스와 바이너리·이미지·console 해시는 비공개
`evidence/session-events-20260930-bcpblw`에 보존했습니다.

| 최종 r2 자료 | SHA-256 |
| --- | --- |
| ARM probe | `f040db84134996af6cf8da9ef8608d7eaf9071f3a4bfcf9c0d9b8cf960c6355e` |
| initrd | `8dd0d3d6857c456ba1c49bbdb822abc8b6577452bcb7f766f9c549f4046c6401` |
| 종료 후 console | `e0641d3a1c40142eb16f957f89d1930cc26726d9f983e769997feb845023f7ac` |

호스트에 패키지를 설치하지 않았습니다. 전용 컨테이너에 추가한 패키지 123개·
갱신 6개, 고정 도구체인 2,124개 blob의 내역을 보존했습니다. 컨테이너·도구체인·
임시 작업 디렉터리를 제거했고 Docker 이미지 및 컨테이너 목록의 복원을 대조했습니다.
기존 Docker 기반 이미지는 보존했습니다. OEM 이미지·설정·원본 로그·분석 dump는
git에서 제외된 비공개 evidence에만 남겼습니다.

## 원격 변경 반영 후 재검사

사용자의 주기적인 fetch/pull 요청에 따라 원격 `master`의
`ccfb255474c6208dfc433d7bd715e8cb618cb9ff`를 `git pull --ff-only --autostash`로
반영했습니다. 센서 수신 대기와 MODEL 계산 주기를 분리하는 외부 변경입니다.
작성 중이던 진단 도구·문서는 충돌 없이 복원됐고 잔여 stash는 없습니다.

통합 상태에서 순정 rootfs·기존 USB fixture를 지정하여 전체 `make test`를
다시 실행했습니다. Python **281개**와 C/C++ 검사가 생략 없이 통과했습니다.
제품 ARM 전체 검사와 원본 커널 VM은 이 pull 뒤 재실행하지 않았습니다.
위 두 VM의 실행 기반과 해시를 새 커밋의 실행 결과로 바꾸지 않습니다.
외부 변경의 ARM/OEM 실행 근거는 별도
[SHADOW runtime 기록](SHADOW_RUNTIME_2026-09-30.md)을 따릅니다.

이 재검사는 새 전용 컨테이너에서 수행했습니다. 추가 패키지 **69개**, 갱신
**5개**의 이름·버전과 실행 로그를 비공개 evidence의 `results/post-pull/`에
보존했습니다. 호스트 패키지 설치는 없으며 컨테이너·임시 작업 폴더는 제거했습니다.
Docker 이미지·컨테이너 목록의 원복과 기존 기반 이미지 보존도 확인했습니다.

**미구현/미검증:** 요청별 bus/receiver/session 자격의 제품 연결, 센서 생산
시각·단위·보정 검증, 정상 연결 사건과 폰/앱 수용, 전체 기동·복구입니다.
live `provenance=false`, `allow_assist=false`와 v1.0 미완료 상태를 유지합니다.
