# v0.3.12-shadow.2 릴리즈 기록 — 2026-10-04

- 태그 `v0.3.12-shadow.2`(pre-release), 소스 커밋 `fba30806752d5655d2a46ccb0c8e167a30841296`, 기본 모드 SHADOW, 셸 전용 묶음.
- 설치 ZIP `mazda-aa-dr-v0.3.12-shadow.2.zip`, SHA-256 `49559ac4858815ed69a5cf3cd154d1eaca62d2e1d205239108872c5222bb354e`.
  게시 뒤 다시 받아 `.sha256` 확인, 빌드한 ZIP과 `cmp` 동일, `unzip -t`, 압축을 푼 `SHA256SUMS` 전부 일치, `source_modified=false`, `mp3`·`js` 없음.
- 제품 바이너리 여섯 개는 [v0.3.12-shadow.1](RELEASE_V0312_2026-10-04.md)과 해시가 같다(새 클론에서 다시 빌드). 변경은 `packaging/trial`(메뉴 2 판정 블록), 설치 안내, 시험이다.

## 변경

메뉴 2의 출력이 43줄·최대 286자였고 판정 항목 열 개가 28줄에 흩어져 작은 CMU 화면에서 읽기 어려웠다. 줄당 40자 이내(실측 최대 29자)의 `---- GO / NO-GO ----` 블록
(`BOOT GUARD ONCE MODE STOP DATA POLL AAPA LDS VBS`를 `ok`/`NO`/`wait`로 요약, 마지막 줄 `GO`·`NO-GO`·`WAIT 60 s, then run 2 again`)을 상세 출력 뒤에 놓고 메뉴 안내를 다시 찍지
않아 화면 아래에 남도록 했다. 같은 블록이 `startup-result.txt` 끝에 저장된다. 판정은 `NO`가 하나라도 있으면 `NO-GO`, 없고 `wait`가 있으면 `WAIT`, 모두 `ok`면 `GO`이다.
순정 BusyBox 1.19.2의 awk와 호스트 awk의 출력이 같음을 확인했다.

## 검사

- ARM 전체: PASS 312, SKIP 0, 종료 0, `release_verified=true`(시작·끝), 원본 LDS 설치 검사 포함.
- 패키징 전체(`MX5DR_STOCK_ROOT`, 최종 ZIP의 `MX5DR_RELEASE_BUNDLE` 지정): 303개 통과, 생략 1(호스트 collector 빌드 필요).
- 호스트 `make -k test`: PASS 줄 298, FAILED 0, **D-Bus 개발 파일이 없어 호스트 collector 세 대상 빌드 불가**(같은 시험은 ARM 통과).
- 순정 BusyBox: 최종 ZIP으로 하네스 `main()` 전체 통과, v0.3.9-shadow.2·shadow.3 위 덮어쓰기 설치 4경우 통과, 메뉴 2 화면 맨 아래에 판정 블록이 나옴을 확인.

## 정정

[v0.3.12-shadow.1](RELEASE_V0312_2026-10-04.md)의 "호스트 FAILED 0"은 `make test-packaging`이 D-Bus 헤더로 막혀 **패키징 테스트가 돌지 않았다는 사실을 빠뜨렸다.** 그 사이 `test_storage`가
기록 상한 상향(`1d53dca`)을 따라가지 못해 실패하고 있었고 이 릴리즈에서 고쳤다. `shadow.1` 릴리즈 노트에 같은 정정을 덧붙였다. 제품 바이너리에는 영향이 없다.

## 한계

실차 기동·센서·복구, 폰/지도 수용, service 계정 파일 소유권(`proot` 한계)은 미검증이다. 서비스 줄은 실제 CMU에서 처음 읽는 값이다. 이 기록은 차량 시험 승인이나 live ASSIST
활성화를 뜻하지 않는다.
