# LDS preload 반복 종료와 정적 TLS 축소 — 2026-10-02

## 판정과 배포 상태

NA 74.00.324A 대상 공개 `v0.3.10-shadow.2` 설치 ZIP을 원본 펌웨어의
격리된 부분 SM 그래프에 넣으면 LDS preload 설치 뒤 원본 `jciLDS`가
반복 종료됩니다. 동일 시험에서 LDS preload만 생략하면 LDS가 유지됩니다.
따라서 `.2`를 설치 후보에서 제외하고 릴리즈 설명에 사용 중단 경고를
게시했습니다. `v0.3.10-shadow.1`의 LDS DSO도 `.2`와 바이트 단위로
같아 함께 사용 중단했습니다. `.1` 전체 기동을 별도로 재현한 것은 아닙니다.

공개 사용 중단 표시는 [`.1` 릴리즈](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.10-shadow.1)와
[`.2` 릴리즈](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.10-shadow.2)의
제목·설명 맨 앞에서 다시 확인했습니다. GitHub API의 마지막 수정 시각은
각각 2026-10-01 22:26:27 UTC, 22:18:01 UTC입니다. 두 항목은 계속
pre-release이며 안정 릴리스용 `releases/latest`는 404였습니다.

게시된 `.2` ZIP SHA-256은
`76d6049db04631352ee7b37cb5f4f0294ad9b36e123dc8ad05b062a1a59d4148`입니다.
두 버전의 LDS DSO SHA-256은
`29a2cbeb7a9f29eb83a9e6ac05a91ec3d0b73fad1dea579e016db5710b79b73f`입니다.
LDS DSO의 ELF `PT_TLS`는 7,288바이트였고, 그중 AA adapter의
`tls` (`ThreadState`와 `frames[8]` 포함) 단일 TLS 심볼의 `st_size`가
7,240바이트였습니다. LDS는
AA prediction 후보를 소유하지 않으므로 이 큰 AA 상태가 필요하지 않습니다.

수정 후보는 LDS 링크에서 AA adapter를 제외하고 LDS bus wrapper가
prediction invalidator 없이 자체 연결 수명·revision만 기록하게 합니다.
AA의 기존 단일 인자 경로는 여전히 `invalidate()`를 주입합니다. 고정
GCC 4.9.1로 빌드한 수정 LDS DSO의 SHA-256은
`2353666d85e301f54341b21e70cf740877f5dcf818d560263a00081db290b01a`,
`PT_TLS`는 48바이트, 정렬은 4바이트입니다. 최종 링크는
`--no-undefined`를 사용하며 미해결 `mx5::` 동적 심볼은 없었습니다.
기존·수정 LDS DSO의 `DT_NEEDED` 여섯 항목은 동일합니다.
`libmx5dr-vimtap.so`에는 `PT_TLS`가 없습니다. 빌드 검사는 정렬
패딩까지 포함한 LDS DSO 자체 TLS가 512바이트를
넘으면 거부합니다. 이 수치는 동일한 큰 배열이 다시 링크되는 회귀를
막는 상한이지 전체 프로세스의 스택 여유를 증명한 수치가 아닙니다.

## 원본 런타임 격리 대조

순정 커널과 rootfs의 해시는 각각
`57fe0cda7c1f994a6ca6b3cce7d9cf247f3b5607ebf1ddc12f02598662d5e240`,
`61bbcea608cc915f45a1775d4f49fb1603e4d92a0163311ced15220d03cf9d44`입니다.
QEMU `sabrelite` 위의 진단 PID 1에서 원본 SM·서비스 바이너리를 사용하되,
19개 서비스·21개 내부 의존성·2개 연결만 포함한 **명시적 부분 그래프**를
실행했습니다. 외부 장치와 전체 OEM 부팅을 재현하지 않았습니다. 실제
`install.sh`는 실행했지만, 재부팅 대신 시험용 이전 boot ID 표식을 만들어
일회성 guard 선택만 검사했습니다. 이는 실제 재부팅·지속성·guard 성공
증거가 아닙니다. 각 실행은 시간 제한으로 종료했으며 timeout 종료 코드
124를 제품 PASS로 취급하지 않습니다.

| 조건 | LDS tap 설치 | SM의 LDS signal 11 | SM LDS 재시도 | 늦은 LDS 상태 |
| --- | ---: | ---: | ---: | --- |
| 공개 `.2`, 추적 켬 | 7회 | 7회 | 6회 | STOPPED |
| 공개 `.2`, 추적 끔 | 7회 | 명시 기록 4회 | 6회 | STOPPED |
| 공개 `.2`, LDS preload만 생략 | 0회 | 0회 | 0회 | RUNNING |
| 전체 수정 진단 묶음, 추적 켬/끔 | 각 1회 | 각 0회 | 각 0회 | RUNNING |
| 공개 `.2` 묶음에서 LDS DSO만 수정본으로 교체, 추적 켬 2회·끔 1회 | 각 1회 | 각 0회 | 각 0회 | RUNNING |

마지막 대조 묶음은 공개 ZIP의 파일 중 LDS DSO, LDS `.sha256`,
`SHA256SUMS`만
바뀌었습니다. 동일한 원본 kernel/rootfs·진단 init·부분 SM 설정을 사용하며,
설치 manifest 검사를 통과했습니다. 이 대조도 사적 실험용이며 공개
릴리즈 ZIP이 아닙니다. 추적을 끈 공개판의 설치 횟수 7회는 제품의 설치
진단 행을 센 값입니다. 그중 signal 11 명시 기록은 4회뿐이며 나머지
종료 원인은 이 로그에서 분류되지 않았습니다. 공개판의 첫 signal 11은
guest 약 29초였고 수정본의 늦은 조회는 guest 100초를 넘었습니다.
수정본의 단일 LDS PID가 늦은 스냅샷까지 남고
원본 `GetPosition` 호출은 응답했지만, 값은 mode 0·UTC/좌표 0이며
`READ_NOT_READY`였습니다. 유효 GPS나 실제 수신기 입력의 성공을
뜻하지 않습니다. 원본 VBS의 별도 중단·reset-board 현상은 순정 대조에도
있고 여전히 남아 있어 AA는 이 부분 그래프에서 기동하지 못했습니다.

이 결과는 큰 정적 TLS에 의한 작은 스레드 스택 침범과 강하게
일치합니다. 하지만 LDS 스레드의 실제 스택 크기·잔여 여유를 직접
측정하지 않았고, 수정은 불필요한 AA invalidator 호출도 제거하므로
TLS만이 유일한 원인이라고 확정하지 않습니다. 반복 대조는 LDS preload
문제를 국소화하지만 물리 CMU 복구나 차량 동작을 검증하지 않습니다.

## 별도 AA 위험과 미완료 항목

수정 후에도 AA DSO의 `PT_TLS`는 7,284바이트, 정렬 8바이트입니다.
고정 도구체인·원본 glibc의 별도 격리 QEMU-user 시험에서 작성한
16KB 스레드와 1KB 로컬 프레임은 AA preload가 있을 때 signal 11로
끝났고, preload 없음과 수정 LDS preload는 정상 종료됐습니다. 24KB에서는
AA preload도 정상 종료됐습니다. 작성한 실제 AA
`position_enter → send_vehicle_data → position_leave` 경로도 16KB에서
종료 코드 139, 24KB에서 정상 종료 코드 0이었습니다. **원본 AAPA의 실제 최소
스레드 스택이 16KB라는 증거는 없습니다.** 따라서 이 별도 시험은
적용 가능한 경계 위험이지 차량 장애 판정이 아닙니다. AA의 실제 원본
기동·callback 경로와 스택 여유는 후속 오프라인 과제로 남습니다.

부분 SM의 VBS 실패, 원본 AA 미기동, 유효 센서·producer 시각·provider/
session/receiver 자격, 물리 재부팅·복구와 Galaxy S25/지도 앱 수용은
여전히 미검증입니다. live ASSIST는 비활성이고 v1.0 완료 조건을
충족하지 않습니다. 추가 차량 방문을 이 기록으로 요청하지 않습니다.

## 검사와 독립 검토 범위

새 호스트 LDS bus fixture는 AA adapter 없이 연결 생성·연결·해제·
signal·close callback을 실제 호출하여 원본 반환값, `errno`, source
수명과 예외 전달을 검사했습니다. 공개 `.2` LDS DSO는 새 TLS 회귀
검사에서 거부됐고 수정 DSO는 통과했습니다. 합성 ELF 경계 회귀 30개
build Python 사례도 통과했습니다. Linux/amd64·Python 3.11의 한 차례
`make test`에서는 Python 581개 중 **541개 통과, 40개 생략**됐고,
C/C++ 실행도 통과했습니다. 생략은 원본 fixture·현재 여섯 산출물 묶음이
해당 실행에 설정되지 않았기 때문입니다. Darwin의 exit 77 허용은 이
Linux 실행에 적용되지 않았습니다. 생략을 릴리즈 통과로 세지 않습니다.
이후 원본 rootfs와 아래 여섯 산출물 묶음을 지정한 별도
`make test-packaging`은 **293개 통과, 생략 0개**였습니다. 앞선 생략을
과거 전체 실행의 성공으로 소급하지 않습니다.

이후 고정 소스 커밋 `bcb70b9e325abde38cf9bcd3b71dba9213ba4d34`에서
ARM 여섯 산출물을 새로 빌드했습니다. LDS DSO 해시는 위 VM 진단에
사용한 값과 **동일**하며, ZIP manifest/CRC를 확인했고 ZIP의
`source_modified=false`입니다. 새 빌드의 전체 ARM runner가 완료돼
Python 109개, 실제 AA 제품 DSO 8 suite·145개, 원본 LDS 설치기
71개를 실패·생략 없이 통과했습니다. `ARM_TEST_INPUTS`의 처음과 끝은
같은 여섯 해시와 `release_verified=true`를 보였습니다. 원본 설치기
fixture는 정상 ServiceInit이나 제품 DSO 자동 기동을 실행하지 않습니다.
수정 후보의 순정 ARM BusyBox chroot 설치·회수와 작성한 재부팅 경계도
통과했지만 실제 PID 1/AA/VBS 기동·물리 재부팅은 실행하지 않았습니다.
첫 ARM 전체 시도는 시험 중 소스 변경으로 build record가 무효화돼
중단됐으며, 위 재실행으로만 전체 결과를 판단합니다.

`-fstack-usage`로 별도 재컴파일한 LDS 코드에서 선택된 정적 함수 프레임은
`mx5_lds_path` 856바이트, sideband `try_send` 1,376바이트,
설치 전 파일 해시 경로 8,336바이트였습니다. 이 값은 호출 사슬의 합계나
OEM 스레드의 잔여 스택 측정이 아닙니다. 유효 GPS 경로는 실행되지
않았고, 수정 VM의 LDS 설치 진단 1회가 모든 hook 호출 횟수를 뜻하지
않습니다.

Codex 독립 리뷰 네 건과 Claude 읽기 전용 적대적 리뷰 두 건을 수행했습니다.
LDS bus 원본 전달 시험 공백·주석 오류·TLS 정렬/회귀 한계는 수정하거나
위에 명시했습니다. Claude의 최종 리뷰가 지적한 소스/산출물 결속은
깨끗한 커밋 재빌드와 동일 SHA로 확인했습니다. AA TLS, 스택 여유,
유효 GPS와 전체 기동은 해결하지 못한 릴리즈 차단 항목으로 남깁니다.
검토자는 소스를 수정하지 않았습니다. 원본 firmware/VM 로그와 임시
실험 파일은 비공개 evidence에만 두었으며 이 기록에는 포함하지 않습니다.
