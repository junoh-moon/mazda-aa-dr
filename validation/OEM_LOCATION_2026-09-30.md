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

## r2: 실제 SM의 부분 서비스 그래프 (`location-sm-r1`)

`02c5f9d`에 새 `location-sm` 진단을 적용했습니다. 원본 SM이 원본
launcher와 서비스들을 기동하도록 했으며, 별도 standalone 실행이나 수동
Start 호출로 실패한 의존성을 우회하지 않았습니다. 원본 전체 그래프의 정상
부팅이 아니라 아래 19개 서비스를 선택한 진단입니다.

- settings, jciUSBMGR, jciVBS, jciLDS, jcinavi, jciBLMSettings,
  jciTime, aap_service, jciAAPA
- stage_1, stage_2, stage_3, stage_navi, usb_drivers, jciBLMTIME
- 실제 PID 파일을 사용하는 vim_app, dbus_service, dbus_hmi
- 원본 `autorun=no` 일반 프로세스 NNG

원본 전역 설정과 서비스의 argv·환경·계정·재시도·reset_board·watchdog·
시간 제한을 유지하고, 기존 touch와 guard가 선택한 AA/VBS trial preload를
환경에 추가했습니다.
stage 스크립트와 USB readiness 스크립트도 원본을
실행했습니다. NNG는 원본 `autorun=no`와 SD 경로를 유지합니다. 실제 guest에
생성된 XML을 원본과 파싱 대조하여 내부 dependency 21개와 connection 2개를
확인했습니다. 다음 외부 연결만 명시적으로 생략했습니다.

- dependency 10개: stage_3→jciMMUI;
  jciBLMSettings→devices/audio_config/dsp_config/system_mazda_my14;
  aap_service→devicemanager;
  jciAAPA→devicemanager/audio_manager/jciRM/jciUpdatea.
- connection 1개: jciBLMSettings→jciaudiosettings.

### 실제로 진행한 경계와 실패

| 항목 | 실제 관찰 |
| --- | --- |
| SM→LDS | 원본 SM이 LDS를 기동. 실제 localhost SM 연결 반환 0, guest 약 39.892초에 STARTED |
| LDS→SM의 NNG 상태 조회 | 실제 callback state=9 수신. r1의 연결 거부·NNG 조회 오류는 이 실행에서 해소 |
| VBS | 약 29.914초 STARTING 후 CAN ReadyHandler timeout 반복. 74.744초에 원본 45,000ms 시작 제한 도달 |
| 순정 SM의 실패 처리 | SIGTERM 뒤 약 79.791초에 5,002ms 종료 제한으로 SIGKILL. 79.862초에 reset_board 정책에 따라 watchdog ping 중단을 기록 |
| jcinavi·jciAAPA·NNG | jcinavi와 jciAAPA 실행은 관찰되지 않음. 마지막 상태에서 VBS·jcinavi·jciAAPA·NNG STOPPED, aap_service를 포함한 나머지 15개 RUNNING |
| USB readiness | 원본 스크립트가 모듈의 No such device 및 usb0 부재 오류 뒤 readiness 파일 생성. USB 장치 성공으로 세지 않음 |
| 외부 PID | VIM과 두 D-Bus의 원본 PID 파일이 실제 살아 있는 해당 실행 파일과 일치 |
| SM 조회 | 초기·마지막 조회 반환 0. 중간 조회는 timeout 143이며 유효한 상태 응답으로 세지 않음 |

VBS의 유지된 의존성 때문에 후속 jcinavi·jciAAPA가 시작되지 않았습니다. CAN 준비
실패의 모든 원인이 물리 하드웨어 부재라는 것까지 분리해 입증하지는 않았습니다.
watchdog ping 중단은 관찰했지만 실제 보드 재부팅·다음 전원 주기의 복구를
검증한 것은 아닙니다.

### 위치 관측과 판정

collector는 실제 UID/GID 1001, 보조 그룹 없이 65회 poll을 수행했습니다.
초기 위치 오류 12건 뒤 실제 LDS 위치 응답 53건, owner 10건, receiver 10건을
기록했습니다. 모든 위치는 mode·UTC·좌표·heading·속도 0이며 receiver도 0입니다.
직접 D-Bus 조회는 `before-sm`과 `after-sm`에서 아직 owner/서비스가 없어
오류였고, `after-start-timeout`과 `late`에서 READ_NOT_READY=5를 반환했습니다.
제공자의 PID·PPid·argv·매핑과 실제 SM 자식 관계를 대조했습니다.

collector는 `stop_marker`로 종료됐습니다. 요청 provenance는 false,
producer time은 unknown이며 GPS serial open은 내보낸 syscall에서 관찰되지
않았습니다. 우리 AA 후크가 들어가는 jciAAPA가 시작되지 않아 이 실행에는
해당 runtime의 health나 SHADOW 계산 성공도 없습니다. 별도의 aap_service가
RUNNING인 사실을 폰 수용으로 해석하지 않습니다. LDS API 응답도 유효한 GPS
fix의 증거가 아닙니다.

### 입력과 실행 범위

배포 ZIP의 다섯 production 바이너리와 touch·원본 rootfs·순정 커널은 r1과
같습니다. QEMU 7.2.22, Sabrelite의 기존 CMU r1 조정, `nohlt`,
`enable_wait_mode=off`, 네트워크·호스트 장치·공유 디렉터리 없는 조건입니다.
SM은 `taskset 0x02`와 `strace -ff`로 실행했으며 원본 서비스 설정의
`affinity_mask=0x01`은 유지했습니다. 각 자식의 최종 CPU affinity를 이
기록에서 실측한 것은 아닙니다. ptrace와 SHADOW의 VBS preload는 타이밍에
영향을 줄 수 있습니다. syscall 로그는 실행 중 내보낸 부분 관찰입니다.

| 비공개 실행 입력/출력 | SHA-256 |
| --- | --- |
| 진단 init | `62270105905004e9cf2d86d0ffc411e2cd136f421bde7445614ca50bbacd2af9` |
| initrd | `82215879eff5269532abcb7bc5d22b1a49631d4ca08cee60df3e5d5dad32d40b` |
| console | `146d90ae5434d542748ec9bf55e0157bd5428feaf59cf16893daf4bab2b25934` |

`run --board cmu --mode shadow --phase location-sm --seconds 360`과 위 kernel
인자를 사용했습니다. collector 한도는 이 phase에서 120초입니다. 실제 마지막
상태 관측은 guest 약 115.58–118.10초였고 inspection shell에 도달했습니다.
360.018초 제한 종료, timed_out=true, QEMU exit=0이며 PASS 판정이 아닙니다.
runner 기본 제한 120초로 실행하면 마지막 snapshot까지 도달하지 못할 수
있습니다. collector 종료 사유도 이 실행의 `stop_marker` 관찰이며, 다른
실행에서 120초 duration 만료보다 항상 먼저 끝난다는 보장은 아닙니다.

이 실행 후 독립 코드 리뷰에서 진단 XML 필터가 주석 속 서비스를 다시 살리거나
잘린 XML 끝을 복구할 수 있는 두 입력 결함을 재현했습니다. 해당 패턴은 이번
정확한 원본 입력에 없었습니다. 각각 실패하는 회귀를 먼저 확인한 뒤 주석과
실제 닫는 태그를 검사하도록 수정했습니다. 실행 init 해시는 수정 전의 실제
입력을 가리키며, 후속 소스를 실행한 것처럼 소급하지 않습니다.

후속 검증에서 정확한 normal 원본 XML의 구·신 필터 출력이 바이트까지 같았고,
원본 XML에 실제 console의 AA/VBS 환경을 복원한 full trial도 동일했습니다.
그 출력은 LF로 정규화한 기존 guest XML과 일치했습니다. 이 비교는 console에서
복원한 입력이며 실행 VM에서 원본 파일을 다시 추출한 것으로 표현하지 않습니다.

원본 ARM BusyBox·loader·libc·libm만 넣은 별도 최소 VM에서도 정상 원본,
복원 trial, 작성한 정상 fixture의 구·신 출력 일치와 두 음성 fixture의
구 필터 반환 0→신 필터 반환 2를 확인했습니다. OEM 서비스는 이 parser
검사에서 실행하지 않았습니다. 새 location 회귀 6개와 기존 retry 회귀 4개,
셸 문법·Python 문법·diff 검사도 통과했습니다. 새 진단은 정확한 normal
설정용이며 범용 XML 변환기가 아닙니다.

production 코드와 ZIP은 바뀌지 않았으며 전체 host/ARM 합성 검사는 이번
진단 변경에서 다시 실행하지 않았습니다. 앞선 전체 검사와 이번 제한된
parser/OEM 실행의 범위를 구분합니다.

parser 전용 실행의 initrd SHA-256은
`c6307049c5e87177c22fa654c58b388b35d8a3e2dc9f31183102dabd5f5d2e61`,
console은 `6e92d535c59752ada4ccae673aefdfebfb373cbe6f615d1f60c6c126c1d297c3`입니다.
이 단계에서 검사한 첫 수정 AWK의 SHA-256은
`3884d322bff38f0a3455d9be020bb1e6a7f310f70fa8563306e3c40b492499a4`입니다.
`PARSER_FINISHED_FAILURES=0` 및 각 RC/cmp로 판정했으며,
120.004초 제한 종료나 QEMU exit=0을 PASS 조건으로 쓰지 않았습니다.

실제 Claude Code 2.1.284의 도구 없는 독립 텍스트 리뷰도 수행했습니다.
원본 OEM 파일·전체 console은 입력하지 않았으며 리뷰 프로세스는 종료 0,
success를 반환했습니다. 이 리뷰는 실행 검증이 아닙니다. 중간 서비스의
닫는 태그 누락을 추가 재현하여 거부하도록 고쳤고, 모든 선택 서비스에 대해
회귀를 확장했습니다. 혼합 multiline comment 배치 거부도 검사합니다.
설정 생성 실패 시 부분 XML을 제거하고 실패를 반환하도록 수정했습니다.
실행 init은 `02c5f9d` 위의 당시 미커밋 작업본이며 그 initrd와 수정 전
필터를 비공개 증거로 보존했습니다.

## r3: 같은 이미지의 baseline 대조 (`location-sm-baseline-r1`)

VBS timeout이 우리 preload 없이도 발생하는지 확인하려고 r2와 동일한
initrd·순정 커널·부분 그래프·strace·taskset·kernel 인자로 실행했습니다.
변경한 runner 인자는 `--mode baseline`입니다. 이 모드는 USB 설치·guard
선택과 collector를 시작하지 않습니다. 따라서 preload 외 모든 실행 부하가
같은 실험으로 표현하지 않습니다. 기존 touch 설정은 유지했습니다.

내보낸 XML을 파싱 대조하여 전역 설정·19개 서비스 속성·dependency 21개·
connection 2개가 일치함을 확인했습니다. 서비스 자식 설정 차이는 jciVBS와
jciAAPA의 trial preload 환경뿐이었습니다. baseline XML과 내보낸
exec/maps에는 우리 설치 경로의 참조가 0개였고 collector 기록도 없었습니다.

- 원본 LDS는 guest 약 24.023초에 STARTED였습니다. `after-start-timeout`과
  `late`의 직접 조회는 mode·UTC·좌표 0, receiver=0, READ_NOT_READY=5였습니다.
- VBS는 CAN ReadyHandler timeout을 반복했고 약 60.913초에 원본 45,000ms
  시작 제한, 약 65.970초에 5,002ms 종료 제한에 도달했습니다.
  약 66.065초에 순정 SM의 watchdog ping 중단 정책도 관찰됐습니다.
- 마지막 상태는 동일하게 VBS·jcinavi·jciAAPA·NNG STOPPED,
  aap_service를 포함한 15개 RUNNING이었습니다. 중간 smctl은 timeout 143,
  마지막 조회는 반환 0이었습니다.

따라서 이 VM 조건의 VBS 시작 실패는 우리 preload가 없어도 발생합니다.
센서 peer·에뮬레이션·계측 부하 등 정확한 원인을 분리한 것은 아니며,
production tap의 모든 동시성·지연 위험이 해소됐다는 증거도 아닙니다.

initrd SHA-256은 r2와 같은
`82215879eff5269532abcb7bc5d22b1a49631d4ca08cee60df3e5d5dad32d40b`,
새 console은 `3ad93cd7726756fb252d32ab27cd46a60befdcb94440b626c3eddec95be33cdd`입니다.
inspection shell까지 진행한 뒤 360.023초 제한으로 종료됐습니다.
timed_out=true, QEMU exit=0이며 이 상태를 정상 기동 PASS로 세지 않았습니다.

## 최종 XML 필터 회귀 — location과 retry

후속 독립 리뷰에서 같은 결함을 기존 retry reduced 필터에도 재현했습니다.
두 필터에 실제 root opening의 존재·순서·중복 검사, 주석 제외, 실제 닫는
태그와 중간 selected service의 종료 검사를 적용했습니다. 실패하면 부분
출력을 제거하고 1을 반환합니다. 선택한 원본 서비스의 속성·환경과 각
유지/제거 간선 수의 계약은 유지합니다. 범용 XML 검증기로 사용하지 않습니다.

수정 전 실패 회귀를 먼저 기록한 뒤 최종 host 검사 18개(location 8,
retry 10)를 통과했습니다. 별도 최소 VM의 순정 ARM BusyBox에서도 입력
99개(location 75, retry 24)의 실제 반환값을 manifest와 대조했습니다.
정상 7개·거부 92개가 모두 기대와 일치했고, 정상 7개의 이전/현재/host 출력은
바이트 단위로 같았습니다. 두 실제 셸 실패 분기도 반환 1과 부분 파일 삭제를
확인했습니다. `PARSER_FINISHED_CASES=99 FAILURES=0`이며 상위 작업자가
현재 소스·두 AWK·두 fixture·case manifest와 99개 고유 결과를 다시 대조했습니다.

location은 기존대로 normal sm.conf만 지원하며 WCP는 간선 수가 달라
거부합니다. retry의 원본 normal/WCP 출력은 기존과 같습니다. 이전 retry
실행 세 initrd의 해시를 해당 metadata와 확인하고 archive 안의 원본 설정도
대조했습니다. 주석 속 대상 서비스나 잘린 wrapper는 없었습니다. runtime
전체 trial은 별도 보존되지 않았지만, r3 환경을 원본에 복원한 출력이 기존
guest XML과 같습니다. 발견한 필터 결함을 과거 OEM 기동 위조로 소급하지 않습니다.

| 최종 parser-only 실행 입력/출력 | SHA-256 |
| --- | --- |
| 저장소 진단 init 소스 | `7aca78501df9327730b7184842ef97859e510fa44966eb0c4d7fe65a8c9d3226` |
| location AWK | `fdd771def39a8b81643aa2a8870eadc43aebe505b54bbf770039fd948937023d` |
| retry AWK | `50b4604e66dbe2226dd55644f3d41e38d4cc1eef78ccf9bcefe44fbbfd768287` |
| initrd | `7f7d91030d9e3d68a0bb6e3c4c8b2aa9b3182e6f3c967dc242290f2435dad043` |
| console | `081e9c82c7d9a2ccc05a9dc8bd6ac5f819615cb8cde483bb8ec4bedcca54a07d` |

이 최종 parser VM은 원본 BusyBox·loader·libc·libm만 사용하고 OEM 서비스는
실행하지 않았습니다. 120.016초 제한 종료와 QEMU exit=0은 판정 근거가
아닙니다. 앞선 parser-r1, 빌드만 한 중간 후보와 실제 최종 r2를 구분해
보존했습니다. 제품 및 USB 변경은 없으며 전체 host/production ARM suite와
원본 전체 부팅을 재실행했다고 주장하지 않습니다.
