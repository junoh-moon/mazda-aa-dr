# v0.3.12-shadow.3 릴리즈 기록 — 2026-10-04

- 태그 `v0.3.12-shadow.3`(pre-release), 소스 커밋 `6c0305fecf69163a6f1b0a9b14099a7fd609ab5a`, 기본 모드 SHADOW.
- 설치 ZIP `mazda-aa-dr-v0.3.12-shadow.3.zip`, SHA-256 `6b6aa9960905dbff11db7c3f617dc2e2cee2986f7d1dc3ade3571415f7f3cdbc`.
  게시 뒤 다시 받아 `.sha256` 확인, 빌드한 ZIP과 `cmp` 동일, `unzip -t`, 압축을 푼 `SHA256SUMS` 전부 일치, `source_modified=false`.
- **진입 파일 포함**: `mp3/a~d.mp3`, `js/run.js`, `USB_ENTRY_NOTICE.md`를 `v0.3.9-shadow.3`의 것과 바이트가 같게 포함한다(`make_usb_zip.py`를 `--shell-only` 없이 실행).
  소유자 지시에 따라 릴리즈 규칙을 "ZIP은 그 자체로 완결적이어야 한다"로 바꿨다([릴리즈 절차](../docs/RELEASING_KO.md)). `v0.3.11-shadow.2`, `v0.3.12-shadow.1`·`.2`는 진입 파일이 없는 셸 전용 ZIP이라
  ZIP만으로 설치를 시작할 수 없었고, 두 `v0.3.12` 릴리즈 노트에 이 사실과 이 릴리즈로의 안내를 덧붙였다.
- 여섯 제품 바이너리(`libmx5dr.so` `ec46bf08…`, `libmx5dr-vimtap.so` `41805e10…`, `libmx5dr-ldstap.so` `7f7a6580…`, `mx5dr-collector` `2304eff0…`, `mx5dr-guard` `a4b893dd…`, `mx5dr-sha256` `358f5d8e…`)는
  `v0.3.12-shadow.1`·`.2`와 같고 이 커밋의 새 클론에서 다시 빌드했다.

## 검사

- ARM 전체: PASS 312, SKIP 0, 종료 0, `release_verified=true`(시작·끝), 원본 LDS 설치 검사 포함.
- 패키징 전체(`MX5DR_STOCK_ROOT`, 최종 ZIP의 `MX5DR_RELEASE_BUNDLE` 지정): 303개 통과, 생략 1(호스트 collector 빌드 필요).
- 호스트 `make -k test`: PASS 줄 298, FAILED 0. D-Bus 개발 파일이 없어 호스트 collector 세 대상 빌드 불가(같은 시험은 ARM 통과).
- 순정 BusyBox 1.19.2: 진입 파일이 포함된 최종 ZIP으로 하네스 `main()` 전체 통과, v0.3.9-shadow.2·shadow.3 위 덮어쓰기 설치 4경우 통과, 메뉴 2 화면 맨 아래에 판정 블록. 기록은 [순정 BusyBox 메뉴 기록](STOCK_BUSYBOX_MENU_2026-10-04.md).

## 한계

MP3로 셸을 여는 단계는 에뮬레이션하지 못했고 이전 릴리즈에서 사용자가 검증한 방식에 의존한다. 실차 기동·센서·복구, 폰/지도 수용, service 계정 파일 소유권(`proot` 한계)은 미검증이다.
서비스 줄은 실제 CMU에서 처음 읽는 값이다. 이 기록은 차량 시험 승인이나 live ASSIST 활성화를 뜻하지 않는다.
