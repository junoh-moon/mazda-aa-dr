# OEM execution and account correction — 2026-09-29

## 판정

순정 커널과 OEM 실행 파일을 실제로 기동했습니다. 정상적인 전체 CMU 기동이나
휴대폰/지도 작동까지 검증한 것은 아닙니다. AA standalone 실행에서는 기존
터치 DSO와 새 preload를 함께 로드하고 실제 AA 후크 설치를 확인했습니다.
센서 입력이 없는 SHADOW 결과를 항법 성공으로 세지 않았습니다.

이 기록은 `ffef803a193b58ab58437d2fd3e10c11a52a02c2` 이후 수정의 근거입니다.
이전 BusyBox chroot 검사는 OEM SM/AA/VBS 실행 검사가 아니었습니다.
특히 그 검사의 비특권 `cmu` 가정은 잘못됐으며 당시 로컬 ZIP은 교체 대상입니다.
역사적 검사 수와 결과는 기존 기록에 보존합니다.

## 실제 입력과 환경

- 대상은 제공된 NA 74.00.324A입니다. 배포 펌웨어 identity 네 개를 유지했습니다.
- OEM Linux 3.0.35 zImage와 initramfs를 업데이트에서 추출했습니다.
  QEMU 7.2.22 `sabrelite`의 진입 시 `r1` machine ID를 3837로 지정했습니다.
  커널 바이너리 기계어, OEM 함수 반환값, 펌웨어 해시 조건을 패치하지 않았습니다.
  이것은 실제 CMU 보드 전체의 에뮬레이션과 다릅니다.
- 별도 `virt` 실행은 Debian ARM Linux 6.1.0-50-armmp입니다. 이를 순정 커널
  실행 결과와 합치지 않습니다. 두 경로 모두 Cortex ARM full-system VM입니다.
- 전체 OEM rootfs를 RAM initramfs로 구성하고 작성한 진단 PID 1을 사용했습니다.
  flash/relfs 부팅, 실제 지속 저장소, 전원 차단 복구를 이 RAM 환경으로 입증하지 않습니다.
- passwd는 공식 backup/update의 이름·UID/GID만 사용하고 암호 필드를 제거했습니다.
  기존 터치 라이브러리는 사용자가 보유한 ARM DSO를 비공개로 공급했습니다.
- 게스트 네트워크는 `-nic none`, 호스트 장치와 공유 디렉터리는 연결하지 않았습니다.
  OEM init은 호스트나 컨테이너 PID 1로 실행하지 않았습니다.

## 확인한 단계와 실패

| 실행 | 관찰 | 판정 한계 |
| --- | --- | --- |
| 원본 kernel + 원본 initramfs | Linux 3.0.35와 초기 OEM LVDS userspace 실행 | 디스플레이/장치 오류; 전체 rootfs 전환과 정상 기동 미확인 |
| 일반 ARM 커널 + 순정 SM | 실제 SM health 초기화 시 watchdog GPIO ENOENT | 서비스 정상 기동 실패. GPIO 파일을 만들어 성공으로 처리하지 않았음 |
| 순정 커널 + RAM rootfs + 순정 SM, r5 | 실제 watchdog GPIO 접근, SM 초기화, settings/VBS 서비스 시작 | nativegui/devices/LVDS/VIM 준비 실패와 watchdog 중단 요청; AA 전체 기동 실패 |
| 순정 커널 + evdev + VM 부팅 옵션, r7 | 실제 VIM 초기화 완료, SPI 장치 open, IPC listen, VIM `STARTED`; settings·USBMGR·LDS도 진행 | nativegui/LVDS/VBS 실패와 watchdog 중단 요청; 정상 전체 기동 실패 |
| 같은 순정 커널, 새 패치 없는 기준 SM 실행, r8 | 기존 터치만 적용한 상태에서도 nativegui display 생성 실패·SIGSEGV와 watchdog 중단 요청 | 새 preload가 없는 상태에서도 해당 기동 실패가 발생함. 다른 결함의 부재를 증명하지 않음 |
| 일반 ARM 커널 + OEM standalone launcher, 기준 실행 | 실제 AA/VBS/settings 실행, 기존 AA 터치 DSO 로드 | 실제 터치 입력이나 휴대폰 연결 검사 아님 |
| 같은 조건의 SHADOW standalone | 실제 AA `boot: mode=4, install=ok`, `health: hook_installed=true`, 기존 터치와 동시 로드 | `runtime_mode=1`은 순정 송신 유지에 맞음. 후크 설치가 센서 수신이나 폰 수용의 증거는 아님 |
| 순정 커널 + OEM standalone, r8 | 실제 VIM·settings·VBS·AA·aap_service 실행, 실제 AA 후크 설치와 기존 터치 동시 로드 | VBS CAN 준비 timeout, SM/USB 서비스 의존성 실패. 전체 서비스 기동·센서 callback·폰 연결 성공 아님 |
| 실제 production collector | UID 0에서 선택 계정으로 전환, journal 시작/종료. 순정 커널 실행에서 일부 SMDB 숫자 응답 | timeout과 위치 서비스 unavailable도 기록됨. 숫자 응답은 신선한 차량 센서가 아님 |

`oem-launchers-shadow-r5`의 기록은 health 55건, SHADOW 471건입니다.
해당 SHADOW는 모두 `E_NO_SEED`, `model_valid=false`, 이벤트 없음이었습니다.
센서/위치가 없는 상태를 성공 데이터로 채우지 않았습니다. 기록 중 오류 없는
health만으로 OEM send 호출이나 VBS callback 전달이 검증됐다고 하지 않습니다.
실행은 시간 제한으로 종료했으며 timeout/프로세스 생존은 PASS가 아닙니다.

후속 r6에서는 순정 init이 사용하는 `evdev.ko` 누락을 고쳤습니다. 원본 kernel의
해당 기능은 module이며, VIM이 생성한 virtual input에서 event 노드를 찾는 단계가
이 모듈 없이 끝나지 않는 것을 확인했습니다. 적재 후 키보드·마우스 생성까지
진행했으나 관찰이 멈췄습니다. GDB의 읽기 전용 관찰에서 두 CPU가 WFI 경로에
있었습니다. 순정 커널이 실제 지원하는 `nohlt`, `enable_wait_mode=off`를
**VM 부팅 인자에만** 추가한 r7에서는 관찰이 계속됐습니다. 두 인자를 함께
변경했으므로 개별 인자의 효과나 실제 차량의 시간·성능을 입증하는 시험은 아닙니다.

r7 순정 커널 실행은 모듈 8개 적재, 실제 collector UID/GID 1001 및 빈 보조 그룹,
VIM의 실제 PID 파일 생성과 SM 등록까지 확인했습니다. 진단 코드가 성공 PID 파일을
만들거나 하드웨어 응답을 합성하지 않았습니다. OEM VIM의 초기화 완료 로그와
서비스 등록을 센서 신선도 또는 MCU 통신 성공으로 해석하지 않습니다.
이 실행의 collector 13회 poll은 speed/yaw/gear 모두 숫자를 반환했습니다.
OEM이 준비한 초기값일 수 있으며 품질과 생산 시각은 계속 unknown입니다.

일반 커널의 r7에서는 OEM `/usr/bin/aap_service`도 추가 실행했습니다.
실제 프로세스·스레드·IPC 실행을 확인했지만 폰 연결이나 미디어 기능 검사가
아닙니다. 초기에 SIGILL이 관찰된 뒤에도 프로세스가 진행했으므로 해당 신호만으로
종료했다고 보고하지 않습니다. AA 후크 health 64건과 SHADOW 561건이 별도로
기록됐으며 유효한 센서 DR 증거는 없었습니다.

원본 `/sbin/init`를 PID 1로 선택한 r6/r7도 실행했습니다. 진단 `/init`가 실행되지
않았음을 구분하고 원본 init의 모듈·VIM 초기화 진행을 관찰했습니다. 시간 제한
안에 정상 전체 부팅을 확인하지 못했으며 원본 flash 부팅 성공으로 보고하지 않습니다.

마지막 r8 비교는 candidate6를 넣은 새 RAM 이미지를 사용했습니다. 새 패치를
설치하지 않은 기준 SM 실행에서도 nativegui display 생성 실패 뒤 SIGSEGV 종료와
watchdog 중단을 관찰했습니다. 이후 LVDS/nativeguictrl/settings도 중단 사유에
추가됐습니다. 별도 순정 커널 standalone 실행에서는 VIM 초기화와 VBS 모듈 진행,
실제 AA 후크 설치 및 기존 터치 라이브러리의 동시 로드를 확인했습니다. 이때 health
41건은 모두 `hook_installed=true`, `audit_fault=0`이었으며 SHADOW 327건은 모두
`E_NO_SEED`, `model_valid=false`, 센서 이벤트 0이었습니다. 일반 커널에서만 후크를
실행한 이전 관찰보다 범위가 넓어졌지만, VBS CAN 준비 timeout·ping 미처리와
SM 연결·구독 및 USB 서비스 의존성 오류가 남았습니다. collector UID/GID 1001을
다시 확인하고 14회 poll 및 정상 session 종료를 기록했습니다. 그중 1회 poll에
timeout이 있었고 위치 조회는 14회 모두 unavailable이므로 센서·위치 성공으로
세지 않습니다. 두 r8 실행은 180초 제한으로 종료했으며 PASS를 뜻하지 않습니다.

진단 과정에서 폐기한 실행도 구분합니다. 처음 ARM strace에 지원하지 않는
`mmap` 필터를 줘 launcher 전에 실패한 r3는 OEM 실행 증거가 아닙니다.
r4 standalone의 NULL crash는 launcher 필수 logLevel 인자 누락이었습니다.
순정 SM의 실제 argv와 비교해 `... library.so 0 -a`로 수정한 후 r5를 실행했습니다.
이 오류를 제품 크래시나 해결된 제품 결함으로 보고하지 않습니다.

## 계정 결함 수정

공식 passwd update와 factory backup 모두 `cmu=UID 0/GID 0`,
`service=UID 1001/GID 1001`입니다. `root`라는 사용자 이름은 필요하지 않습니다.

- 설치기와 production collector는 비특권 `cmu`가 있으면 그것을 유지하고,
  `cmu`가 UID 0이면 기존 비특권 `service`를 선택합니다.
- collector는 보조 그룹을 비우고 GID/UID를 바꾼 다음 설정·로그·D-Bus에 접근합니다.
  설치기가 지정하는 logs 소유권도 같은 정책을 사용합니다.
- 설치·재예약은 필요한 계정 검사를 쓰기 전에 합니다. 제거는 계정 상태와
  무관하게 arm 제거·OFF·기존 설정 복원이 가능하도록 분리했습니다.
- UID 0 `cmu`의 OEM AA writer와 별도 비특권 collector는 로그 파일을 구분합니다.
  shared CPU/flash 부하, 실제 버스 서비스 준비 여부까지 보장하는 정책은 아닙니다.
  `service` UID는 OEM 서비스와 공유하므로 별도 보안 sandbox를 뜻하지 않습니다.

실제 Claude Code에도 계정 diff를 읽기 전용으로 제공했습니다(실행 결과 보관).
보조 그룹과 버스 권한에 관한 지적은 원본 group의 service 보조 그룹이 비어 있고
순정 bus policy가 사용자 연결을 허용함을 대조했습니다. 실제 숫자 SMDB 응답도
관찰했지만 모든 필드·실차 권한의 확인으로 확대하지 않습니다. 기존 UID의 0600
collector 파일이 재시작을 막을 수 있다는 지적에 따라 알려진 collector 파일만
잠금 하에 소유권을 이관하며, AA trace나 다른 파일을 재귀 변경하지 않습니다.
추가 Codex 리뷰에서 잠금 전 검사가 정상 PID 삭제·로그 회전과 경쟁하는 문제를
찾았습니다. 안정적인 `collector.lock`을 먼저 잠그도록 수정했습니다. 실행 중이면
디렉터리와 lock의 소유권만 확인하며, 같은 UID에서는 설치 준비를 계속합니다.
다른 UID에서는 소유권을 바꾸기 전에 중단하고 실행 중인 collector를 죽이지 않습니다.

## 회귀 검사와 리뷰

- 새 출력 디렉터리에서 `make test`: 성공. 이 최초 전체 실행에는 fixture 환경
  변수가 없어 packaging 21건이 생략됐습니다. 이를 통과로 세지 않았습니다.
- 실제 stock identity와 candidate4를 공급한 `make test-packaging`:
  **89/89, 생략 없음**. 변경 후 다시 실행했습니다.
- 순정 BusyBox 1.19.2/libc/NSS + production ARM 실행: 전체 설치/검증/가드/수집/
  로그 회수/제거 harness 성공. 여기의 mount는 호스트 보호를 위한 명시적 모델입니다.
- 계정 변형 6건, 정상 설치 후 service 삭제/UID 0 변경 제거 회귀 2건:
  **8/8 성공**. 후자에서는 arm 제거, OFF, SM/autostart 복원, 기존 로그 bytes와
  소유권 보존을 확인했습니다.
- 잠금 우선 수정 후 새로 재빌드한 ARM 결과로 candidate6 ZIP을 만들었습니다.
  이 묶음으로 전체 CMU 설치/제거 harness, 계정 6건과 제거 회귀 2건을 다시
  통과했으며 `make test-packaging`도 **89/89, 생략 없음**이었습니다.
- 같은 candidate6에서 과거 UID의 네 collector 파일을 bytes 손실 없이 이전하고
  실제 ARM collector가 재개되는 것을 확인했습니다. AA trace는 바뀌지 않았습니다.
  네 경로의 symlink/FIFO 8건 거부, 실행 중 같은 UID 허용, 다른 UID 이전 거부와
  기존 소유권·기동 설정·예약 보존도 확인했습니다.
- 실제 ARM collector가 잠근 상태의 BusyBox `stat` 호출을 관찰하는 새 회귀는
  candidate5에서 mutable 경로 조회를 검출해 실패했고 candidate6에서 통과했습니다.
  임의 횟수의 경쟁 재현으로 무결함을 주장하지 않고, 문제였던 조회가 busy 경로에
  없음을 검사했습니다.
- 독립 Codex 리뷰 4개를 부팅 의존성, OEM 통합/검증 도구, ZIP/계정 회귀,
  계정/복구 최종 리뷰로 나누어 수행했습니다. 불필요한 제거 계정 gate와 동시성
  결함을 수정하고 회귀를 실행했습니다. 별도 두 리뷰어가 최종 잠금 수정도 다시
  읽었으며 추가 P0/P1/P2를 찾지 못했습니다. 리뷰를 실행 검사의 대체로 세지 않습니다.

ARM 바이너리는 고정 GCC 4.9.1 도구체인으로 새 `build/final-oem`에 재빌드했습니다.
다섯 바이너리의 SHA-256은 앞선 OEM 실행에 사용한 수정본과 모두 동일합니다.
이 기록의 OEM 실행은 actual ARM ELF를 사용하며, host fixture의 합성 ELF
헤더를 실행하지 않습니다. 이전 전체 ARM 합성 suite는 역사적 기록에 남기고
이번 계정 변경의 target 검사는 production collector 실행으로 별도 기록합니다.

## 재현과 비공개 증거

`tests/packaging/oem_system_emulation.py`는 비공개 rootfs tar와 압축을 푼 USB
묶음으로 initramfs를 생성하고 QEMU 실행·SHA·명령·종료·변형을 JSON으로 남깁니다.
`oem_guest_init.sh`는 진단 전용이며 USB나 실제 CMU에 설치하지 않습니다.

```sh
# 격리된 Linux 컨테이너 UID 0에서만 build합니다.
python3 tests/packaging/oem_system_emulation.py build \
  --rootfs-tar /private/rootfs.tar.gz --bundle /private/usb \
  --touch /private/libpatch-blmjciaapa.so --output /private/root.cpio.gz
python3 tests/packaging/oem_system_emulation.py run \
  --kernel /private/stock-zImage --initrd /private/root.cpio.gz \
  --board cmu --mode shadow --phase services --seconds 220 \
  --output /private/stock-shadow
```

유휴 경로 비교 r7은 위 run 명령에
`--kernel-arg nohlt --kernel-arg enable_wait_mode=off`를 추가했습니다.
원본 PID 1 비교는 검증된 build sidecar가 있는 이미지에 `--original-init`를
사용하며 실제 `/sbin/init` 선택을 JSON에 기록합니다.

일반 ARM 커널의 standalone 비교는 `--board virt --phase standalone`과
`--mode baseline`/`shadow`를 사용합니다. 이미지 안의 진단 코드·모듈·계정·RAM
저장소 변경을 결과와 함께 읽으십시오. 빌드/실행 도구도 전체 기동 성공 판정을
자동 생성하지 않습니다. 원본 OEM 바이너리·메모리 맵·해체 내용·전체 콘솔 로그는
비공개 `design_inputs/`, `build/`, `evidence/`에만 둡니다.

남은 질문은 실제 VIM 센서 전달, 같은 SM 실행 내 재시도, 기존 터치 입력,
정상 전원 주기 복귀와 폰/지도 수용입니다. 현재 에뮬레이션에는 이를 판정할
하드웨어·입력·연결이 없습니다. ASSIST를 켤 근거로 사용하지 않습니다.
