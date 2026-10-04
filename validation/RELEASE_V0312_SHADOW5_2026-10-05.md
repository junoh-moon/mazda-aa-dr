# v0.3.12-shadow.5 릴리즈 기록 — 2026-10-05

- 태그 `v0.3.12-shadow.5`(pre-release), 소스 커밋 `429e7dbe3f3ab5b65002fc9569bb789b7a7872e1`, 기본 모드 SHADOW, 진입 파일(`mp3/`, `js/run.js`, `USB_ENTRY_NOTICE.md`) 포함.
- 설치 ZIP `mazda-aa-dr-v0.3.12-shadow.5.zip`, SHA-256 `cf77702d3b655f55ae244d83feb9f839cbfe9e74f88e4cba5e2968f97a688445`. 게시 뒤 다시 받아 `.sha256` 확인, 빌드한 ZIP과 `cmp` 동일, `unzip -t`, `SHA256SUMS` 전부 일치, `source_modified=false`.
- 바이너리: `libmx5dr.so` `5d21814d…`(후킹 수정, shadow.4에서 변경), `libmx5dr-vimtap.so` `41805e10…`, `libmx5dr-ldstap.so` `db5fa6f3…`, `mx5dr-collector` `252c2efc…`, `mx5dr-guard` `a4b893dd…`, `mx5dr-sha256` `358f5d8e…`(나머지는 shadow.4와 같음).
- 변경: AA 위치 후킹이 oem-aa-mod `libpatch`와 공존(`f97d13f`), 회수 도구가 `oem-aa-mod` 해시와 `libpatch.conf` 수집(`429e7db`). 근거와 시험은 [후킹 검증 기록](AA_HOOK_LIBPATCH_2026-10-05.md).

## 검사

- ARM 전체: PASS 315(AA 설치 시험 3 포함), SKIP 0, 종료 0, `release_verified=true`(시작·끝), 원본 LDS 설치 검사와 AA 설치 시험 모두 입력을 지정해 생략 없이 실행.
- 호스트 `make -k test`: 종료 0, FAILED 0, make 오류 0, Python 629개(호스트 collector 포함).
- 패키징 전체(최종 ZIP 경로, 호스트 collector 지정): 311개 통과, 생략 0.
- 순정 BusyBox 1.19.2: 최종 ZIP으로 하네스 전체와 이전 버전 위 덮어쓰기 설치 4경우 통과.

## 한계

AA 후킹이 실제 차량에서 설치되는 것은 처음이다. `libpatch` 기능(터치·HUD·km/L)과의 실제 공존, 재부팅 수정(shadow.4)의 효과, 실차 센서·복구, 폰/지도 수용, service 계정 파일 소유권(`proot` 한계)은 미검증이다.
이 기록은 차량 시험 승인이나 live ASSIST 활성화를 뜻하지 않는다.
