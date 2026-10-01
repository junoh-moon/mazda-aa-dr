# USB에서 한 줄 설치

현재 안내는 [USB 설치 방법](../packaging/USB_START_KO.md)을 따르십시오.
내용물 전체를 FAT32 USB 최상위에 복사하여 `trial`, `install.sh`, `mp3/`, `js/`가
바로 보이게 하십시오. USB 곡 재생으로 진단 셸이 열린 뒤 실행하십시오.

```sh
sh /tmp/mnt/sda1/trial
```

실제 USB가 sdb1 등으로 잡히면 해당 경로 글자만 바꾸십시오. Shift가 필요한
문자는 없습니다. 메뉴에서 `1`과 Enter로 설치하고, 다음 정상 부팅 후 같은
메뉴에서 `2`로 상태 확인, 도착 후 주차 상태에서 `3`으로 종료·회수하십시오.
회수 대상은 실행한 USB이며 경로를 다시 입력하지 않습니다. 제거는 `4`,
메뉴 종료는 `0`입니다. 설치기는 전체 묶음과
대상 펌웨어를 검사하고, 필요한 마운트를 일시적으로 쓰기 가능하게 바꾼 뒤
복원합니다. 별도 해시 도구 설치나 root 계정 이름은 요구하지 않습니다.
설치기는 순정 SM 설정을 보존하고, 다음 한 번의 기동을 예약합니다.

일반 빌드 기본값은 OBSERVE이며 통합 시험용 ZIP은 SHADOW로 명시하여
만듭니다. SHADOW는 계산 결과를 기록하고 순정 송신을 유지합니다.
ASSIST는 비활성 상태입니다. 설치 성공 후 정상적인 전원 종료·기동이 필요합니다.

[v0.3.3-shadow.1](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.3-shadow.1)의
공개 파일은 `mazda-aa-dr-v0.3.3-shadow.1.zip`이며 기본 모드는 SHADOW입니다.
공개 파일 재다운로드까지 확인한 [검증 결과](../validation/RELEASE_V033_2026-10-01.md)와
[설치 수정 범위](../validation/USB_INSTALL_2026-09-29.md)를 확인하십시오.
기존 v0.3.0-shadow.1 ZIP에는 이 수정이 없습니다.

숫자 메뉴는 v0.3.3 공개 ZIP과 이전 `a29f1b8` 후보에 없습니다.
메뉴를 포함한 후속 후보는 [주차 중 확인·회수 절차](FIELD_TRIAL_KO.md)의
고정 파일을 사용하십시오. 기존 [저장 공간·계산 진단 검증](../validation/TRIAL_PREPARATION_2026-10-01.md)은
해당 커밋의 기록이며 새 메뉴의 실행 결과와 구분합니다.
