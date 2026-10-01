# USB에서 `sh install.sh` 설치

현재 공개 파일은 NA 74.00.324A 전용
[v0.3.8-shadow.1 설치 ZIP](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.8-shadow.1)입니다.
파일명은 `mazda-aa-dr-v0.3.8-shadow.1.zip`이고, SHA-256은
`19edb8b72faba813e169543ff6e1697a945954f13f76d5ee1346803b81168686`입니다.
GitHub의 자동 `Source code (zip)` 대신 이 설치 ZIP을 받으십시오.

ZIP의 **내용물 전체**를 깨끗한 FAT32 USB 최상위에 풀어 복사하십시오.
USB에서 `install.sh`, `mp3/`, `js/`, `trial`이 바로 보여야 합니다.
주차 중 Entertainment → USB에서 포함된 곡을 재생하고 진단 셸이 열리면
다음처럼 실행하십시오. 실제 USB가 sdb1 등으로 잡히면 경로만 바꾸십시오.

```sh
cd /tmp/mnt/sda1
sh install.sh
```

설치 명령이 성공하면 정상적으로 전원을 끄고 다시 켜십시오. 다음 **한 번의
부팅**에서 SHADOW 관측·계산·기록이 자동 실행됩니다. ASSIST 송신은
비활성입니다. 별도 `sha256sum`, 계정 이름 `root`, 수동 remount 명령은
필요하지 않습니다. 설치기는 기존 터치·km/L 설정을 보존합니다.

주차 중 상태 확인·수집 종료·같은 USB로 회수·제거에는 다음 숫자 메뉴를
사용하십시오. `2`는 상태, `3`은 종료·회수, `4`는 제거입니다.

```sh
sh /tmp/mnt/sda1/trial
```

[통합 시험 절차](FIELD_TRIAL_KO.md)와
[최종 ZIP 검증](../validation/RELEASE_V038_2026-10-01.md)을 따르십시오.
실제 차량 기동·센서·복구·휴대폰 수용은 아직 검증하지 않았습니다.
