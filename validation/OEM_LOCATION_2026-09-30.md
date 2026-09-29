# 실제 OEM 위치 서비스 실행 — 2026-09-30

이 기록은 NA 74.00.324A의 원본 LDS·내비 서비스를 격리 VM에서 실행한
관찰입니다. 유효한 위치, 정상 전체 차량 기동, 폰 수용의 PASS 기록이 아닙니다.
기존 [릴리즈 검증](RELEASE_2026-09-30.md)의 standalone 실행은 settings,
VBS, AA까지였으며 이 실행에서는 위치 제공자를 추가했습니다.

## r1: 원본 LDS·내비 standalone 실행

기준 소스는 `1411ddf`이며 추가한 진단 phase는 `location`입니다.
원본 SM 설정의 LDS·내비 launcher 인자를 대조하여 사용했습니다. LDS XML,
시작 모드, 원본 서비스·라이브러리, 반환값은 변경하지 않았습니다. 서비스는
원본 launcher의 standalone 모드로 실행했으며 실제 SM은 시작하지 않았습니다.
따라서 원래 SM 의존성이 충족된 실행으로 해석하지 않습니다.

위치 서비스 기동 전·후와 후속 시점에 원본 service bus에서 GetNameOwner,
GetPosition, GetSelectedGPS_sync, GetReadStatus_sync를 조회했습니다.
메서드 이름과 읽기 상태 enum은 원본 client 및 introspection XML에서
확인했습니다. OEM Start/설정 메서드는 호출하지 않았습니다.

| 항목 | 실제 관찰 |
| --- | --- |
| 제공자 기동 전 | owner 없음, 위치·수신기 조회는 ServiceUnknown |
| 기동 후 owner | 같은 실행의 unique owner, PID, 원본 LDS launcher argv와 라이브러리 매핑 일치 |
| GetPosition | 원래 아홉 반환 인자 수신. mode=0, UTC=0, 나머지 값도 0 |
| 수신기·읽기 상태 | 선택 GPS=0, READ_NOT_READY=5. guest uptime 약 39초·64초의 두 조회에서 동일 |
| 비특권 collector | 실제 UID/GID 1001, 보조 그룹 없음. 초기 unavailable 5건 후 위치 응답 22건, 총 27 poll |
| collector 출처·종료 | owner poll 5건과 receiver=0 5건. 같은 LDS PID를 관찰했고 stop_marker로 정상 종료 |
| AA | health 51건 모두 hook=true, audit_fault=0, drop=0 |
| SHADOW | 406건 모두 E_NO_SEED, model_valid=false, 센서 이벤트 0 |

위치 응답이 존재한다는 사실은 유효한 GPS fix 또는 신선한 센서 측정을
뜻하지 않습니다. UID 0의 직접 조회와 UID 1001 collector에서 실제 LDS 응답을
확인했으므로, 이번 실행의 위치 조회를 전부 unavailable로 분류해서도 안 됩니다.
collector의 owner 관측은 특정 AA 요청의 provenance를 증명하지 않습니다.

### 실패와 아직 진행하지 못한 경계

- jcinavi는 원본 argv로 실행했지만 SM 연결 거부와 ServiceTerm 뒤 SIGSEGV가
  관찰됐습니다. 구형 strace wrapper의 종료 값 0을 OEM 정상 종료로 세지 않습니다.
- LDS에도 SM 연결 오류 102와 NNG 조회 오류 120이 있었습니다. 이는 이번
  standalone 환경의 빠진 SM·NNG 연계이며 물리 GPS 고장 증거가 아닙니다.
- 내보낸 syscall 기록에서 GPS serial port open은 관찰되지 않았습니다.
  READ_NOT_READY를 단순 STOP 상태나 직접 확인된 GPS/MCU 고장으로 바꾸어
  해석하지 않습니다. VBS CAN ready timeout은 계속 발생했습니다.
- 실행은 240.026초 제한으로 종료됐습니다. metadata의 timed_out=true,
  QEMU exit=0은 관찰 종료 상태이며 PASS 조건이 아닙니다.

### 입력과 재현

- 배포 입력: `v0.3.1-shadow.1`, 소스 `bcdfda9b6a98e1ec9bc6d880015d5de2222c593b`.
  다섯 production 바이너리 해시는 [배포 기록](RELEASE_2026-09-30.md)과 같습니다.
- 순정 커널 Linux 3.0.35 SHA-256:
  `57fe0cda7c1f994a6ca6b3cce7d9cf247f3b5607ebf1ddc12f02598662d5e240`.
- 진단 init SHA-256:
  `c05dbe2ebe59b2235b3241437a0e650f1908bcc6f64fc25eccd7d9b1a231cb9e`.
- 비공개 initrd SHA-256:
  `49745fcffa4dd3c1ab4d5b9c2d29bdbd85911ac5baa9d10a03be8251e2ec443b`.
- 비공개 console SHA-256:
  `c139551c81d7d6c8108dbf0964c131a61d16aa7e090523162268c786d128debc`.

기존 릴리즈 VM과 동일한 원본 rootfs·touch 라이브러리와 QEMU 7.2.22
Sabrelite를 사용했습니다. CMU entry의 r1 조정과 `nohlt`,
`enable_wait_mode=off`는 기존 진단 조건입니다. 네트워크·호스트 장치·공유
디렉터리는 VM에 연결하지 않았습니다. 원본 binary, 전체 console, 계정 정보는
공개하지 않습니다.

기존 [OEM 실행 도구](OEM_RUNTIME_2026-09-29.md)의 새 build 이미지로
`run --board cmu --mode shadow --phase location --seconds 240`을 사용하고,
위 두 kernel 인자를 명시했습니다. location에서만 collector 관찰 시간을
60초로 늘리고 끝에 기존 stop_collector 경로를 호출합니다. 일반 standalone
기동 순서와 설치 ZIP은 이 진단 변경의 대상이 아닙니다.

셸 문법, 기존 OEM retry 회귀 4개, diff 검사를 통과했습니다.
새 출력 디렉터리의 `make test BUILD=build/host-v1-followup-20260930`도
통과했습니다. 원본 firmware와 게시 ZIP fixture를 지정했으며 Python 253개와
C/C++ 실행 모두 통과, 생략 0입니다. 실제 제품 ARM 코드는 바뀌지 않았으므로
ARM 합성 전체 검사는 재실행하지 않았습니다. 그 결과는 기존 배포 기록을
따르며 이번 OEM 실행의 성공 범위로 확대하지 않습니다.
