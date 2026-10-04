# 순정 BusyBox에서의 USB 메뉴 실행 — 2026-10-04

시험 묶음(`build/freeze/mx5dr-usb.zip`, SHA-256 `bbdd0ab9a51b61d0e3df94fb497bc5579d6ea32bbfda1c8d341c68c49109bdc9`, 소스 커밋
`1988686`)의 `trial` 메뉴를 CMU 순정 루트(ARM BusyBox 1.19.2, glibc 2.11.1)에서 실행했다. 차량·실제 CMU에서 실행한 것은 없다.

## 방법

root가 없는 서버라서 `proot -0`(가짜 root)에 `qemu-arm` 8.2.2를 붙여 `tests/packaging/cmu_emulation.py`의 사본으로 실행했다. 사본의 변경 7개
(저장소 경로 고정, chroot→proot, `/dev/null` 바인드, euid 검사 제거, 게스트 제한 90→900초, collector 저널 uid 1001 검사 제거,
아카이브 uid/gid 비교를 0으로 매핑)는 사본 안의 `DEVIATION` 주석에 있다. 재부팅은 하네스의 witness 스텁, `/proc` 서비스 항목과
trace·collector 기록은 하네스가 쓰는 합성 자료다. 소요 시간은 부하가 높은 호스트의 `proot`+QEMU 값이며 CMU 값이 아니다.

## 확인한 것

- 하네스 `main()` 전체 rc=0(손상 USB 거부, 설치, 로더, 한 번 부팅, 메뉴 3 회수, 4 이후 3, 복구 순서 3→4→5→2, `startup_menu_flow`).
- 메뉴 1·5·2·3·4·0과 반복 실행, USB 행 없음, USB 폴더 없음, 잘못된 키, 입력 종료의 출력과 종료 코드(별도 26단계 기록).
- 순정 환경: `sha256sum` 없음(묶음의 `mx5dr-sha256` 사용), uid 0 이름 `cmu`, `id root` 실패, `/data_persist`→`/mnt/data_persist`→`/tmp/mnt/data_persist` 심볼릭 연결,
  `/proc/mounts`가 `self/mounts` 심볼릭 링크, USB `sda1`은 `ro,noexec`이고 쓸 때마다 remount.
- 제거(메뉴 4) 뒤 `sm.conf`, `sm_WCP.conf`, `autostart`가 순정과 바이트 단위로 같음.
- 실제 ARM `mx5dr-guard`가 부팅 ID 변경 뒤 `guard_committed_after_new_boot`로 확정, 실제 ARM collector가 기록.
- **덮어쓰기 설치**: `v0.3.9-shadow.2`와 `shadow.3`을 먼저 설치한 위에 새 묶음을 설치. 이전 설치 직후(무장 상태)와 이전 시험 부팅 소비 뒤
  두 경우 모두 메뉴 1 성공, 메뉴 5 이후 시뮬레이션 부팅에서 메뉴 2가 일곱 근거(`reboot_check=new_boot_observed`,
  `startup_state=guard_committed_after_new_boot`, `one_boot=consumed_this_boot`, `config_mode=SHADOW`, `runtime_disable_next_start=absent`,
  `retained_bytes>0`, `collector_poll_recent=observed`)를 출력.
- 메뉴 3은 AA 쪽 `capture.done`이 없으면 `finish_exit=1`과 "Freeze not confirmed within deadline"을 출력하지만 내보내기(`export_exit=0`)는 계속된다.
- 호스트 단위 시험 `test_trial_menu`: 서버가 한가할 때 29개 전부 통과(20초). 서버 부하가 9~10이던 때의 13·19개 오류는 모두 20초 시간 초과였다.
- `install.sh`와 `uninstall.sh`는 USB 마운트 행이 없어도 진행한다(메뉴 3·5는 거부, 메뉴 2는 상태만 표시).

## 확인하지 못한 것

물리 USB·FAT 동작, 실제 재부팅·PID 1, 플래시·전원 차단, 실제 SM·AA·VBS·LDS 프로세스(서비스 줄은 합성 `/proc`), 실제 센서 기록, **service 계정의 파일 소유권**(`proot`가
chown을 유지하지 못함), CMU 실제 소요 시간, 설치기 소스의 "Persistent … preload found" 오류 경로(실행하지 않음), 셸 진입(MP3) 단계.
이 기록은 설계 변경이나 live ASSIST 활성화, 차량 시험 승인을 뜻하지 않는다.
