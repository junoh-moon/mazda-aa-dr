# CMU 재부팅과 기동 진단 회수 — 2026-10-02

## 추가로 확인한 실차 자료

사용자가 처음 설치 뒤 했던 조작은 **차량 전원을 껐다 켜는 것**이었다고
확인했습니다. 설치 안내가 이 조작을 실제 CMU Linux 재부팅과 구분하지 않았고,
새 부팅·자동 기록을 확인할 방법도 충분히 제공하지 않았습니다. 사용자의 주행을
부정하거나 설치 안내의 누락을 사용자 책임으로 돌리지 않습니다. 이번에 CMU가
이전 커널을 유지했는지, 새 커널에서 기동에 실패했는지는 아직 확정하지 못했습니다.

이후 주차 중 셸 사진과 직접 회수한 설치 폴더·autostart archive를 확인했습니다.
개인 공유 주소, 사진, 원본 파일 본문과 archive는 공개하지 않습니다.

- 현재 `/dev/mmcblk0p2`가 `/tmp/mnt/data_persist`에 `relfs`, `rw,noatime`으로
  마운트되어 있습니다. 현재 여유 공간은 522,330 KiB입니다. `/tmp`의 tmpfs와
  별개이며 시험 당시 마운트 상태를 증명하는 것은 아닙니다.
- 설치 폴더·guard·logs의 소유권과 권한, 실제 arm의 형식, 설치된 제품·도구의
  바이트는 검사한 공개판과 일치합니다. SHADOW 설정과 arm은 남아 있지만
  `guard/last-boot`, `guard/consumed`, trace·collector JSONL은 없습니다.
- 실제 autostart에는 NORMAL/WCP 각 원본 SM 호출 직전의 설치 블록이 있습니다.
  블록을 제거한 내용은 원본과 같습니다. 설치 블록 부재나 다른 제품 바이너리를
  이번 자료의 원인으로 확인한 것은 아닙니다.
- arm의 제품·설정·trial template 다섯 입력은 실제 파일과 일치합니다.
  **현재 `/jci/sm/sm.conf`, `sm_WCP.conf`는 회수하지 못했습니다.** 나머지 두
  지문과 보존 백업의 일치는 현재 SM 파일의 일치 증거로 승격하지 않습니다.
- OFF 설정을 담은 앞선 빈 archive와 SHADOW 설정을 담은 뒤의 빈 archive가
  있습니다. 초기화된 벽시계·파일명·PID로 모든 설치 순서를 복원하지 않습니다.

이 자료로 기동·저장 실패의 원인은 여전히 미확정이며 실제 관성항법 적용이나
센서·폰 수용을 입증하지 못했습니다. 차량 셸 작업은 종료했고 추가 명령이나
반복 주행을 요청하지 않았습니다.

## 원본 재부팅 경로

제공된 NA 74.00.324A 원본 BusyBox의 **무옵션 `/sbin/reboot`**를 별도 PID
namespace에서 실행했습니다. 원본 ARM BusyBox와 원본 libc는 `sync` 후
PID 1에 `SIGTERM`을 보내고 종료 0을 반환했습니다. 이 실행에서 직접
`reboot` syscall은 하지 않았습니다. PID 1은 신호를 기록하는 작성한 witness이며
호스트 PID·네트워크를 공유하지 않았습니다. 첫 실행은 입력 디렉터리 권한으로
QEMU 실행 전 실패했고 그 실패도 비공개 증거에 보존했습니다.

원본 `init_cmu`의 정적 분석에서는 SIGTERM 처리기가 동기화·프로세스 종료·
파일시스템 해제/읽기 전용 복원 시도·보드 reset/kernel RESTART 경로로 이어집니다.
서비스별 정상 종료 handshake나 실제 CMU 하드웨어의 재시작 완료를 검증한 것은
아닙니다. `reboot -f`는 실행하거나 설치 절차에 넣지 않았습니다.

숫자 메뉴의 `5`는 USB에 요청 전 boot ID와 uptime을 저장하고 sync 및 USB의
원래 읽기 전용 상태 복원을 마친 뒤 이 명시적 명령을 한 번 호출합니다.
요청 저장·명령 종료 0만으로 완료를 선언하지 않습니다. 다시 열린 메뉴 `2`가
유효한 이전·현재 boot ID를 비교해 `new_boot_observed`, `same_boot`,
`unavailable`을 표시하고 USB `startup-result.txt`에 보존합니다.

ACC, 엔진이 꺼진 ON, 실제 엔진 가동은 별개 상태입니다.
[2019 MX-5 공식 전원 상태 설명](https://www.mazdausa.com/static/manuals/2019/mx5/contents/05010101.html)과
[공식 엔진 시동 절차](https://www.mazdausa.com/static/manuals/2019/mx5/contents/05010200.html)를
대조했습니다. 설치 안내는 실제 엔진을 가동한 주차 상태를 유지하면서 CMU만
재부팅하도록 일관되게 정합니다. 이는 이번 설치 절차의 전원 유지 선택이며
Mazda가 이 패키지를 승인했다는 뜻은 아닙니다.

## 회수 범위와 실패 처리

메뉴 `3`은 종료 표식 생성 전 읽기 전용 기동 진단을 확보하고, 상태·종료 확인이
실패해도 회수를 시도합니다. archive에는 다음을 담습니다.

- `data_persist/mx5-aa-dr` 전체: guard 표식·설정·제품·도구·백업·모든 보존 로그.
- 실제 autostart, 현재 NORMAL/WCP SM 설정, 버전 파일, 남아 있는 임시 trial 설정.
- 현재 boot ID·uptime·mounts/mountinfo·커널·메모리·관련 프로세스 cmdline/maps,
  기동 입력인 실제 `data_persist/testmode.conf`, 재부팅 요청·주차 중 확인 결과.
- 순정 `uname`·`ps`·`dmesg`·저장 공간 출력과 종료값, 관련 OEM 로그의 제한된 끝부분.

원본 파일은 USB tar로 직접 읽어 소유권·권한·링크 형식을 보존합니다. 설치 폴더를
FAT에 중복 복사하거나 CMU 내부에 archive를 만들지 않습니다. 순정 tar의
append·여러 `-C` 미지원도 실제 BusyBox에서 확인해 한 root-relative archive를
사용합니다. 기존 PC 분석기는 로그의 상위 경로가 추가된 형식을 읽을 수 있습니다.
예상 밖 링크·특수 파일은 그대로 보존하며 분석기의 기존 경고를 완화하지 않습니다.

개별 추가 진단 출력은 128 KiB, 관련 프로세스는 64개, OEM 로그는 두 대상
디렉터리에서 각각 16개 파일의 끝 128 KiB로 제한합니다. FIFO인 원본 stdout/stderr
경로는 메타데이터만 남기고 읽지 않습니다. 전체 설치 폴더와 누적 백업의 크기는
별도이므로 **전체 archive가 고정 크기라는 뜻은 아닙니다.** CMU의 기존 수집
로그 26 MiB 상한과도 구분합니다.

누락·형식 오류·잘린 진단·명령 실패를 자료와 함께 표시합니다. tar 쓰기 실패는
성공으로 바꾸지 않으며 USB의 `.partial`과 진단 디렉터리를 보존합니다. 완료한
archive에는 SHA-256 sidecar를 만듭니다. 원본 파일은 지우지 않습니다. archive에
OEM 설정과 차량 자료가 포함되므로 공개 저장소·릴리즈에는 게시하지 않습니다.

## 검증과 한계

- 회수된 50개 항목을 바이트·소유권·권한·시각 그대로 복원한 NORMAL/WCP
  두 fixture에서 원본 autostart를 새로 호출했습니다. 실제 공개 guard가 arm을
  consumed로 옮기고 last-boot와 각 trial을 만들며 collector·SM 호출에 도달했습니다.
  archive에 없는 현재 SM 두 파일은 지문이 맞는 백업으로 대신했으며, collector·
  하드웨어·최종 SM 호출은 작성한 witness입니다. 회수 후 stop 표식도 그대로
  유지하여 이 결과를 실제 캡처 재개의 증거로 세지 않습니다.
- 앞선 네 원본 autostart fixture는 NORMAL/WCP 및 persist 준비/지연을
  나누었습니다. persist가 숨겨진 경우 guard를 호출하지 않고 OEM SM으로
  진행하며, 나중에 persist를 보여도 자동으로 다시 선택하지 않았습니다.
  작성한 마운트 지연의 결과이지 이번 차량 원인 확정은 아닙니다.
- 새 읽기 전용 진단 helper는 host 18개 및 순정 BusyBox 6개 사례를 통과했습니다.
  실제 정적 SHA 도구 fallback, 설치 자료의 불변, 임시 helper 제거를 검사했습니다.
- 재부팅 helper host 14개는 명시 선택, receipt, sync·원래 USB 상태 복원 순서,
  실패 전달과 fixture의 호스트 reboot 차단을 검사합니다. 작성한 remount/witness
  검사이며 위의 원본 BusyBox 실행과 구분합니다.
- 확대 회수 host 12개, 숫자 메뉴 host 26개를 통과했습니다. 전체 설치·현재 OEM
  파일·metadata·proc alias·FIFO·누락·잘린 진단·실패를 검사합니다. USB 경로를
  도중에 교체하는 작성 fixture에서는 기존 구현의 잘못된 성공을 재현한 뒤,
  원래 USB 디렉터리를 유지하여 새로 노출된 경로에 쓰지 않고 원래 USB에
  partial을 남기도록 수정했습니다. 실제 USB 탈착 검사는 아닙니다. 새 파일 수정
  이후 최종 릴리즈의 고정 소스·전체 host/ARM·ZIP 실행 결과는 별도 발행 기록에
  남기며 이 부분 검사만으로 최종 ZIP 검증 완료를 선언하지 않습니다.

QEMU 사용자 모드는 호스트 커널을 사용합니다. 실제 CMU 부팅·전원 유지·플래시·
물리 USB 전환을 완벽하게 재현했다고 주장하지 않습니다. 기존 보호 조건을
완화하거나 새 기동 차단 조건을 넣지 않았으며 live ASSIST는 여전히 비활성입니다.
v1.0은 센서 기반 관성항법 위치를 실제 Android Auto 전달에 적용하는 목표이며,
이 진단 수정만으로 완료 처리하지 않습니다.
