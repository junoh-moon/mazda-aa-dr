# v0.3.9 회수 실패와 핫픽스 검증 — 2026-10-02

실차에서 v0.3.9-shadow.1 메뉴3이 `/proc/mounts`의 정상 심볼릭 링크를
거부하여 회수에 실패했습니다. 일반 설정 파일의 링크 금지 검사를 커널의
mount-table 별칭에도 적용한 제품 결함입니다. 시험 환경도 해당 경로를 일반
파일로 만들어 이 결함을 놓쳤습니다. 순정 BusyBox를 썼다는 사실만으로
파일 구조까지 실제 환경과 같다고 판단한 검증의 누락입니다.

## 사용자 자료에서 확인한 범위

제공된 USB 파일 36개는 공개 v0.3.9-shadow.1 ZIP의 파일과 모두 바이트
동일합니다. 파일 혼용이나 복사 손상으로 설명할 수 없습니다. 사진에는
메뉴1 설치 완료, 메뉴3의 `Not a regular non-symlink file: /proc/mounts`,
뒤이은 메뉴4 제거 완료가 있습니다. 원문 사진과 공유 주소는 공개하지 않습니다.

메뉴3은 상태 조회·종료 요청·export·결과 보고서 작성 전에 중단됐습니다.
제공 자료에는 주행 journal이나 회수 archive가 없으므로 AA 기동, 센서 수집,
SHADOW 계산 및 폰 수용의 성공·실패는 아직 판정할 수 없습니다. 제거 코드는
로그를 보존하며 현재 설정만 OFF로 바꿉니다. 이후 회수 archive의 현재 OFF
설정을 과거 시험의 모드로 읽지 않고 해당 boot 기록을 확인해야 합니다.

## 수정과 재현

`packaging/trial`은 커널 mount table의 읽기에서 정상 링크를 따릅니다.
일반 파일의 기존 검사와 실행한 USB가 실제 mount 목록에 있는지 확인하는
조건은 그대로입니다. 메뉴3은 제거 후에도 재설치·재무장 없이 남은 자료를
회수하며 현재 부팅 종료 확인과 파일 복사의 결과를 구분합니다.

호스트 메뉴 및 순정 BusyBox chroot fixture에 `/proc/mounts -> self/mounts`를
반영했습니다. 모의 remount는 링크의 backing 파일을 갱신하며 USB 분리·복귀와
guest 명령 전후에 링크가 유지되는지 확인합니다. 기존판에서 동일 오류를
재현하고 수정판에서 제거 후 원시 trace·collector 바이트, archive SHA-256,
OFF·미무장 상태 및 OEM 설정 보존을 대조했습니다.

실제 Linux procfs도 순정 ARM BusyBox와 기존 QEMU로 직접 읽었습니다.
`-f`, `-r`, `-L` 모두 참이고 링크 대상은 `self/mounts`였습니다. 그 procfs에서
기존판은 같은 링크 거부로, 수정판은 실제 USB가 없는 조건의 mount 검사로
진행했습니다. 이 직접 검사는 물리 차량 USB의 정상 회수 시험이 아닙니다.

집중 호스트 회귀는 메뉴 17개·종료 8개 통과입니다. 상태 42개 중 1개는
해당 집중 환경에 collector가 없어 생략했습니다. 이 결과와 새 빌드의 전체
검사는 구분합니다. 최초 잘못된 discovery 패턴의 0-test 실행은 성공 검사로
세지 않습니다.

## 배포 고정

- 소스: `bdb37e0f8cf5c8acb95166fba7f02fce860e9e21`.
- ZIP: `mazda-aa-dr-v0.3.9-shadow.2.zip`, 1,445,783바이트.
- SHA-256: `4f6633ac95a20aa6c9907c2938843dff2fa769e6fd4c14dbc4fc243d7bf30699`.
- 새 detached checkout과 고정 GCC 4.9.1로 다섯 ARM 바이너리를 빌드했습니다.
- 기존 v0.3.9-shadow.1과 다섯 바이너리 및 63개 빌드 입력이 모두 동일합니다.
- ZIP 36파일·내부 manifest 35파일·Git 소스 89파일을 대조했습니다.
  `source_modified=false`, 기본 SHADOW이며 ASSIST는 비활성입니다.

ZIP 변경은 `trial`, 설치 안내, manifest, build-info 네 파일입니다.
검증과 회수를 위한 수정이며 항법 계산·차량 기동 성공을 추가하지 않습니다.
기존 실차 기록을 회수할 때는 USB 파일을 교체하고 기존 한 줄과 `3`만
사용합니다. 다시 설치하거나 주행할 필요가 없습니다.

## 최종 검사와 게시 확인

[v0.3.9-shadow.2](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.9-shadow.2)를
2026-10-01 15:22:09 UTC에 prerelease로 게시했습니다. 해당 소스는 master에도
반영했습니다. 새 host·ARM 검사는 별도 clean checkout에서 실행했습니다.

| 검사 | 결과 |
| --- | --- |
| 전체 host `make test` | Python 426개와 C/C++ 통과, 종료 0, 생략 0 |
| 고정 ARM/QEMU 전체 | Python 81개와 C/C++ 통과, 종료 0, 생략 0 |
| 실제 배포 DSO 8개 suite | 145개 통과 |
| ARM 시작·종료 입력 | 두 기록 동일, `release_verified=true` |
| 최종 ZIP·순정 BusyBox | 전체 설치/회수, 계정, 메뉴 실패 세 검사 종료 0, 생략 0 |
| 제거 후 메뉴3 | 원시 trace·collector 바이트와 archive SHA 일치, OFF·미무장·OEM 설정 보존 |

host Python 합계는 41+135+28+1+10+177+34=426, ARM Python은 12+18+51=81입니다.
검사 전후 tracked 입력이 같고 checkout은 clean입니다. 작성된 로그·USB mount·
boot 조건은 실제 차량 입력과 구분하며 전체 하드웨어 에뮬레이션을 주장하지 않습니다.
최초 검증 도구의 Git 소유권 preflight 실패는 제품 실행 전 실패로 보존하고,
정확한 checkout 경로를 지정한 후의 실행 결과만 위 표에 포함했습니다.

공개 ZIP과 외부 체크섬을 다시 내려받아 후보와 실제 바이트가 같음을 확인했습니다.
ZIP CRC·전체 manifest·89개 소스의 Git blob·다섯 산출물·태그의 고정 커밋도
대조했습니다. 과거 공개 ZIP은 변경하지 않았으며 해당 릴리즈 안내에 회수 결함과
핫픽스 링크를 덧붙였습니다.

임시 binfmt·helper·QEMU 복사본과 작업용 컨테이너를 제거했습니다. 컨테이너의
추가/갱신 패키지 79행과 도구체인 2,124개 blob도 제거됐습니다. 최초 호스트
패키지 2,963개·이미지 11개·컨테이너 0개·binfmt 디렉터리 7항목과 상세 목록
8종이 모두 같습니다. 원본 펌웨어·실차 원문·검사 결과는 비공개로 보존했습니다.
