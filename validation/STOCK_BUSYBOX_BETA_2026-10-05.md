# 순정 BusyBox에서의 BETA 묶음 메뉴·상태 판정 — 2026-10-05

master `c008d00`의 릴리즈 ARM 빌드([ARM_FULL_2026-10-05](ARM_FULL_2026-10-05.md))로 `tools/make_usb_zip.py --default-mode BETA`(그리고 비교용
`--default-mode SHADOW`) ZIP을 로컬에서만 만들었다. `--shell-only` 없이 만들어 `mp3/a-d.mp3`, `js/run.js`, `USB_ENTRY_NOTICE.md`가 들어 있고,
`build-info.json`은 `source_commit=c008d00…`, `source_modified=false`, `default_mode=BETA`, 압축 해제 뒤 `SHA256SUMS` 불일치 0이다. 게시하지 않았다.

## 방법

[2026-10-04 기록](STOCK_BUSYBOX_MENU_2026-10-04.md)과 같은 `proot -0` + `qemu-arm` 8.2.2 하네스 사본(`make_root`, 같은 7개 DEVIATION)에서
ARM BusyBox 1.19.2 순정 루트로 `trial`을 키 입력으로 구동했다. 재부팅은 witness 스텁, `/proc`의 서비스 세 개는 실제 CMU 형태(comm `L_<svc>`,
argv `/jci/sm/sm_svclauncher -l <svc> …`)로 만든 합성 항목이며 각자 해당 preload가 maps에 있다. collector 저널(시작+poll 2줄)은 합성이다.

BETA 저널은 `tests/runtime/test_worker_beta.cpp`의 `main`, `nofix_rearm` 시나리오를 호스트에서 실행해 **제품 저널 작성기가 쓴** `trace.0.jsonl`을
보존한 것이다(시험 사본은 마지막 삭제 전에 파일만 복사, 저장소에 넣지 않음). boot_id를 게스트 부팅 ID로 바꾸고 `/proc/uptime`을 행 시각에 맞췄다.
`nofix_rearm`에는 NO_FIX/SPEED_ENGAGED 전이 10행, choice 4 overlay send 39행(결과 0이 아닌 것 1행)이 있다.

SHADOW 확인에는 shadow.5 실차 내보내기(모드 4, 비공개, 저장소에 복사하지 않음)의 `logs/`, guard 표식, `mx5dr.conf`를 게스트에만 넣었다.

## 결과(자동 확인 92개 전부 통과)

- **(a) 첫 화면·설치**: 첫 화면에 `This USB installs mode BETA: BETA = SHADOW capture + dead-reckoned LOCATION only while GPS is lost`와
  `1 Install BETA for the next boot`. 메뉴 1 rc=0, `mx5dr.conf` 첫 줄 `mode=BETA`, `installed.txt` `mode=BETA`, BETA 안내
  (`ok BETA armed, ok HOOK and ok FENCE`) 출력, `guard/arm` 생성. `guard/normal.trial`·`wcp.trial`에 VBS tap preload 줄이 있고, 메뉴 5 뒤
  실제 ARM guard `select`(exit 0)가 고른 설정에 VBS·LDS·AA preload가 모두 있다.
- **(b) BETA 상태/판정**(메뉴 2, 판정 줄 모두 40자 이하, 가장 긴 줄 40자):
  - B1 현재 부팅, capture_end 뒤: `ok MODE BETA`, `ok BETA armed`, `ok HOOK installed`, `ok FENCE declined`, `ok HLTH`, `BETA NO_FIX: 10 state rows`,
    접힌 줄 `BETA: engaged 0 times, replaced 0 sends,` / ` speed overlay sends 39, nonzero 1,` / ` hold 1,` / ` last state DISABLED (capture_stop),` /
    ` scope current_boot`, `GO`. 상세 줄 `beta_speed_engaged=3 beta_speed_overlay_sends=39 beta_speed_overlay_nonzero=1`.
  - B2 같은 저널, capture_end 직전 시각: 위와 같고 `last state ARMED (gps_returned)`, `capture_active=observed`, status_exit=0, `GO`.
  - B3 같은 부팅의 워커 재시작(`main` → `nofix_rearm`): 합계 `engaged 2 times, replaced 9 sends, speed overlay sends 39, nonzero 1, hold 2`.
  - B4 다음 부팅: `wait BETA/HOOK/FENCE not started`, `scope previous_boot`, `NO-GO`.
- **shadow.5 실차 저널**(SHADOW 묶음): 주행 중 t=600 s 시점은 `ok BOOT/GUARD/ONCE/MODE SHADOW/STOP/DATA`, `ok POLL polling 0s ago`, `ok HLTH`,
  `ok AAPA/LDS/VBS preload yes`, BETA 줄 없음, `GO`. 내보낸 그대로(t=860 s, stop 표식 있음)는 `NO GUARD capture_stop_requested`,
  `wait POLL last poll 4s ago`, `NO-GO`. 다음 부팅은 `wait POLL no poll yet`, `NO-GO`. **이전의 `wait POLL unavailable` 거짓 음성은 어느 화면에도 없다.**
  `guard_config_binding=matched`(묶음의 `mx5dr-sha256` 사용, 순정에 `sha256sum` 없음).
- 같은 실차 저널을 BETA 묶음으로 보면 `NO MODE SHADOW not BETA`, `NO-GO`(모드 혼동 거부).
- 모든 화면에 `awk:`/`syntax error`/`not found` 같은 BusyBox 이식성 오류가 없다.

## 관찰(수정하지 않음)

- B1·B3처럼 capture_end 뒤에는 상세 상태가 `capture_active=unavailable`, status_exit=1인데 짧은 판정은 `GO`다. 짧은 판정은 주행 전 시작 확인용으로
  capture_active를 보지 않는다. 정상 절차(설치→재부팅→메뉴 2→AA)에서는 이 시점에 capture가 살아 있으므로 문제 되지 않지만, 주행 뒤 메뉴 2의
  `GO`를 주행 결과로 읽으면 안 된다.
- 상태 이름이 24자를 넘으면 잘린다(예: `NO GUARD guard_selection_unconfir`). 전체 값은 `startup-result.txt`에 있다.

## 확인하지 못한 것

차량·실제 CMU, 물리 USB/FAT, 실제 재부팅·SM·AA·VBS·LDS 프로세스(서비스는 합성 `/proc`), 휴대폰(Naver 지도/AA 앱)의 BETA 위치·속도 수용, DHU,
실제 GPS 상실 상황, service 계정 파일 소유권(`proot` 한계), CMU 실제 소요 시간. BETA 저널은 호스트(x86) 시험 fixture가 만든 것이고 ARM 실차 저널이
아니다. 이 기록은 BETA 차량 시험, live ASSIST 활성화 또는 릴리즈 게시를 승인하지 않는다.
