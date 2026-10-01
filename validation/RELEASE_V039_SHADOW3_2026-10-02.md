# v0.3.9-shadow.3 설치·재부팅·회수 검증 — 2026-10-02

[공개 릴리즈](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.9-shadow.3)를
발행하고 설치 ZIP·체크섬을 다시 받아 검증했습니다. 고정 커밋은 master에
반영했으며 기존 태그나 공개 ZIP을 교체하지 않았습니다.

## 배포 대상

- 소스: `0ae08ccfae3504bd54dd286baa6c682c3e75b059`
- 파일: `mazda-aa-dr-v0.3.9-shadow.3.zip`
- SHA-256: `186cd6cad2965e8c76921db8efcae0eed6547638a6c10e1c36734ddb52af03bc`
- 크기: 1,458,990 bytes, 최상위 ZIP 항목 38개
- 대상: NA 74.00.324A / 2019 MX-5 ND2 6MT
- 기본 모드: SHADOW, `source_modified=false`, live ASSIST 비활성

이번 변경은 [설치·기동 진단 수정](STARTUP_RECOVERY_2026-10-02.md)입니다.
숫자 `5`의 순정 CMU 재부팅 요청, USB의 이전 boot ID와 숫자 `2`의 새 부팅
대조, 설치 폴더 전체와 현재 autostart·SM·부팅 진단의 USB 회수를 포함합니다.
ACC·엔진이 꺼진 ON·실제 엔진 가동을 구분하고 한 USB 포트의
`1 → 5 → 2 → 0 → AA 동글 교체` 절차를 제공합니다.
첫 빈 기록의 원인을 CMU 재부팅 누락 하나로 확정한 것은 아닙니다.

다섯 제품 바이너리와 63개 빌드 입력은 공개 `.2`와 동일합니다. 이 릴리즈에
새 LDS 입력 공급부나 ASSIST 활성화가 포함된 것으로 세지 않습니다.

## 전체 실행과 최종 커밋의 동일성

`706ab264aa39bb0e345ca040c8b429d9ca6d12bf`의 서로 다른 깨끗한 checkout에서
새 제품을 빌드하고 다음 전체 검사를 직접 실행했습니다.

| 검사 | 실제 결과 |
| --- | --- |
| `make test` | Python 482개와 C/C++ 검사 통과, exit 0, skip 0 |
| `tests/run_arm_all.sh` | Python 81개와 standalone ARM 검사 통과, exit 0, skip 0 |
| 실제 제품 DSO | 8개 suite, 145개 사례 통과 |
| 검사 전후 입력 | 각 checkout의 추적 파일 355개 동일 |

고정 GCC 4.9.1 도구체인 `61ec0343de84f6fc7c46840056df1d600d44be8a`를
사용했습니다. ARM 시작·끝 입력 기록의 `release_verified=true`, 다섯 산출물,
도구체인·sysroot를 대조했습니다. ARM ELF, softfp, 동적 의존성·GLIBC와
TEXTREL 검사도 빌더에서 실행했습니다. 서로 다른 C/C++ assertion 수를
하나의 테스트 수로 합산하지 않았습니다.

첫 최종 ZIP 검사에서 새 archive 검사기가 없는 proc 파일의 실제 `missing`
표기를 `unavailable`로 잘못 기대하여 중단됐습니다. 제품은 누락을 수집
성공으로 표시하지 않았습니다. 올바른 누락 archive가 거부되는 실패를 먼저
재현하고, 수정 뒤 정상 1개와 손상 11개를 각각 수락·거부했습니다.

최종 `0ae08cc`는 이 검사기의 한 줄만 바꿉니다. 새 checkout과 새 ARM build
디렉터리에서 다시 빌드했으며, 나머지 추적 파일 354개·전체 host/ARM 검사 입력·
제품 입력 63개·다섯 바이너리·도구체인의 동일성을 확인했습니다.
전체 host/ARM 실행은 위 `706ab264`의 결과이며 새 커밋에서 반복 실행했다고
표현하지 않습니다. 최종 ZIP의 원본 BusyBox 검사는 아래처럼 새로 실행했습니다.
ZIP의 36개 실제 payload는 이전 후보와 같으며 소스 pin을 담는
`build-info.json`과 그 파일의 `SHA256SUMS` 항목만 달라집니다.

## 최종 설치 ZIP 실행

원본 ARM BusyBox와 순정 공유 runtime으로 최종 ZIP을 풀어 실행했습니다.
Linux 모양의 `/proc/mounts -> self/mounts` 링크를 유지했으며 mount 동작과
부팅 상태는 작성한 모델입니다. QEMU user가 CMU 커널·플래시·전원·SM 전체를
에뮬레이션한 것으로 세지 않습니다.

설치·일회성 예약·collector 실행·숫자 메뉴·다음 부팅의 두 회수 경로·제거 뒤
회수·재설치를 통과했습니다. 네 실제 생성 tar에서 전체 설치 폴더와 현재
autostart·두 SM 설정의 bytes·UID/GID·mode, proc alias의 내용, 순정 명령의
출력·종료값을 대조했습니다. 작성하지 않은 proc 정보는 누락으로 남기며
`ps` 헤더를 실제 OEM 프로세스 실행 증거로 세지 않습니다.

별도 계정 검사는 6개 계정 구성, 두 계정 소실 회수, collector 소유권 전환과
활성 lock·경로 형식을 순정 NSS/BusyBox와 실제 제품으로 통과했습니다.
차량 bus·SMDB나 OEM 서비스 실행은 아닙니다.

메뉴 실패 검사에서는 실제 커널 파일 크기 제한으로 `printf` 쓰기 오류를
발생시켜 이전 보고서와 원시 자료 보존, 정확한 실패 표시, 임시 파일 제거를
확인했습니다. 현재 수집 종료를 확인하지 못해도 raw archive·체크섬을 회수했고,
마운트되지 않은 USB는 수집 중지나 archive 생성 전에 거부했습니다.
첫 보조 주입은 호스트의 SIGXFSZ ignore가 guest 셸까지 유지되지 않아 목표
오류 반환 전에 signal로 끝났습니다. 제품은 실패를 표시했습니다. guest에서
ignore를 명시한 뒤 해당 메뉴 검사만 다시 실행하여 exit 0을 확인했습니다.
이를 제품 수정이나 전체 검사 재실행으로 세지 않습니다.

각 전용 binfmt·보조 컨테이너·QEMU 복사본은 제거했고 cleanup exit 0을
확인했습니다. 호스트 package·image·container·binfmt와 등록 상세 목록은
검사 전후 같으며 이번 릴리즈 작업의 추가 패키지 설치는 0개입니다.

별도의 원본 재부팅 helper 여섯 사례는 최종 소스와 같은 helper/common
바이트로 실행했습니다. 정상 경로는 실제 순정 `sync` 두 번과 작성 remount
완료 뒤 BusyBox의 `kill(1, SIGTERM)` 한 번이며, 격리한 작성 PID 1만 대상입니다.
원본 init_cmu의 이후 reset 경로는 정적 분석입니다. 진단 미확보·실제 signal
거절·sync 실패·ro 복원 실패·USB 부재를 구분했으며 차량의 실제 재부팅 완료는
검사하지 않았습니다.

## 게시 파일과 남은 범위

2026-10-02 02:41 KST에 pre-release로 발행했습니다. 원격 태그의 실제
커밋을 대조하고, GitHub에서 다시 받은 두 파일을 검사했습니다.
다운로드 ZIP과 검증한 로컬 ZIP은 바이트 단위로 같으며 SHA-256·ZIP CRC·
37개 manifest 파일·38개 항목·소스 pin·SHADOW·`source_modified=false`가
일치합니다. 추가 `unzip`/`file` 설치 없이 Python zipfile/hashlib로 CRC와
manifest를 확인했고 ELF는 고정 readelf를 사용했습니다.

실차의 새 부팅·자동 수집·물리 복구·센서 단위와 생산 시각·폰/지도 앱 수용은
아직 입증되지 않았습니다. 빈 첫 회수는 계속 미판정이며 정상 archive 생성이
주행 데이터 존재나 계산 성공을 뜻하지 않습니다. v1.0의 목표인 관성항법
위치의 실제 AA 적용은 미완료입니다. 이 시험판은 새 차량 방문을 요청하지 않습니다.

이전부터 사용한 주 검증 컨테이너와 도구체인은 후속 LDS 구현·검증을 위해
아직 사용 중입니다. 해당 작업 종료 때 최종 제거 결과를 별도로 기록하며
OEM 파일·원시 차량 자료·개인 공유 링크는 공개하지 않습니다.
