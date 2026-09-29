# USB 설치

대상은 1세대 Mazda Connect **NA 74.00.324A**입니다.

1. ZIP을 푼 **내용물 전체**를 깨끗한 FAT32 USB 최상위에 복사하십시오.
   USB를 열었을 때 `install.sh`, `mp3/`, `js/`가 바로 보여야 합니다.
2. 주차 상태에서 USB와 키보드를 연결하십시오. Entertainment → USB에서
   곡을 재생하고 약 15초 기다리십시오. 진단 셸이 열립니다.
   다른 USB가 먼저 연결되어 있으면 다음 곡도 재생하십시오.
3. 셸에서 해당 USB 디렉터리로 이동한 뒤 실행하십시오.

   ```sh
   cd /tmp/mnt/sda1
   sh install.sh
   ```

   USB가 `sdb1`, `sdc1`, `sdd1`로 잡히면 `cd` 경로만 바꾸십시오.
   별도 `sha256sum`, `chmod`, `chown`, `mount` 명령은 필요하지 않습니다.
4. 명령이 성공으로 끝나면 정상적으로 전원을 껐다 켜십시오.
   다음 한 번의 부팅에서 묶음에 지정된 모드로 자동 수집합니다.
   SHADOW 묶음은 위치 계산을 로그로만 기록하며 폰으로 전송하지 않습니다.

기존 AA 터치와 km/L 패치는 유지됩니다. 이 묶음의 MP3/JS는 셸만 열며
기존 터치 패치를 다시 설치하지 않습니다. 설치 중에는 USB를 빼지 마십시오.

AA 연결 후 주차 상태에서 수집 상태를 확인하십시오.

```sh
sh /data_persist/mx5-aa-dr/tools/trial_status.sh
```

운전 중에는 명령을 입력하지 마십시오. 도착 후 주차 상태에서 로그를
저장하십시오. 마지막 경로는 실제 USB 경로로 바꾸십시오.

```sh
sh /data_persist/mx5-aa-dr/tools/finish_capture.sh
sh /data_persist/mx5-aa-dr/tools/export_logs.sh /tmp/mnt/sda1
```

제거할 때는 USB에서 `sh uninstall.sh`를 실행하십시오. USB가 없어도
`sh /data_persist/mx5-aa-dr/tools/uninstall.sh`로 제거할 수 있습니다.
다음 정상 부팅부터 적용되며 기존 터치 설정과 수집 로그는 보존합니다.

이 패키지는 설치 오류를 수정한 시험 후보입니다. 실제 CMU에서의 기동,
센서 수신 및 폰/지도 앱 수용은 아직 검증하지 않았으며 ASSIST는 꺼져 있습니다.
