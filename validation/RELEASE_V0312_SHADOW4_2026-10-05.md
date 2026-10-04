# v0.3.12-shadow.4 릴리즈 기록 — 2026-10-05

- 태그 `v0.3.12-shadow.4`(pre-release), 소스 커밋 `4748f090dbc76534df8582500372dee66059f4bd`, 기본 모드 SHADOW, 진입 파일(`mp3/`, `js/run.js`, `USB_ENTRY_NOTICE.md`) 포함.
- 설치 ZIP `mazda-aa-dr-v0.3.12-shadow.4.zip`, SHA-256 `13f94eb93e9b6a2172c42866a5ff21f2241b21dae651d3e54ac6a983185fb821`. 게시 뒤 다시 받아 `.sha256` 확인, 빌드한 ZIP과 `cmp` 동일, `unzip -t`, 압축을 푼 `SHA256SUMS` 전부 일치, `source_modified=false`.
- 바이너리: `libmx5dr.so` `ec46bf08…`, `libmx5dr-vimtap.so` `41805e10…`, `mx5dr-guard` `a4b893dd…`, `mx5dr-sha256` `358f5d8e…`은 shadow.3과 같고, `libmx5dr-ldstap.so` `db5fa6f3…`(노출 심볼 숨김)와 `mx5dr-collector` `252c2efc…`(SMDB 접근 제거)가 바뀌었다.
- 변경과 근거는 [실차 시험 분석](TRIP_RESET_2026-10-04.md)에 있다.

## 검사

- ARM 전체: PASS 312, SKIP 0, 종료 0, `release_verified=true`(시작·끝), 원본 LDS 설치 검사 포함.
- 호스트 `make -k test`: 종료 0, FAILED 0, make 오류 0, Python 628개 실행. 처음으로 호스트 collector 대상까지 생략 없이 통과(D-Bus 개발 헤더를 root 없이 `apt-get download`+`dpkg -x`로 풀어 `HOST_DBUS_FLAGS`/`HOST_DBUS_LIBS`로 지정).
- 패키징 전체(`MX5DR_STOCK_ROOT`, 최종 ZIP의 `MX5DR_RELEASE_BUNDLE`, 호스트 collector 지정): 310개 통과, 생략 0.
- 순정 BusyBox 1.19.2: 최종 ZIP으로 하네스 `main()` 전체 통과, v0.3.9-shadow.2·shadow.3 위 덮어쓰기 설치 4경우 통과([순정 BusyBox 메뉴 기록](STOCK_BUSYBOX_MENU_2026-10-04.md)).

## 한계

AA 위치 후킹은 이 릴리즈에서도 설치되지 않는다(서드파티 `libpatch`가 세션 함수를 가로채 안전하게 거부; 수정은 다음 릴리즈). 재부팅 수정의 효과, 실차 센서·복구, 폰/지도 수용, service 계정 파일 소유권(`proot` 한계)은 미검증이다.
이 기록은 차량 시험 승인이나 live ASSIST 활성화를 뜻하지 않는다.
