# 제품 후크 없는 원본 AA 세션 정리 비교 — 2026-09-30

앞선 [AA 자동 처리 시험](AA_PIPELINE_2026-09-30.md)의 mutex 정리 오류를
**제품 adapter·preload·후크 없이도 재현했습니다.** 세 조건에서 세션을 각각
세 번 생성·송신·파괴했고, 총 아홉 주기 중 일곱 주기에 원본 mutex destroy
오류 16이 기록됐습니다. 따라서 이 오류의 발생에 제품 후크가 필요한 것은
아닙니다. 원본 또는 진단 환경의 정확한 원인·영향까지 해결한 결과는 아닙니다.

## 방법과 범위

Codex가 NA 74.00.324A의 순정 rootfs·Linux 3.0.35와 원본 aap_service를
격리 QEMU 7.2.22/Sabrelite에서 실행했습니다. NIC·호스트 장치·공유 폴더는
없습니다. 기존 kernel-entry machine ID 조정 뒤 userspace 전에 debugger를
분리했으며 userspace debugger·strace는 사용하지 않았습니다.

고정 GCC 4.9.1·ARMv7 softfp로 별도 fixture를 빌드했습니다. 링크 입력은
작성한 fixture와 파일 검사용 sha256.cpp뿐입니다. 컴파일 명령과 ELF 정의
심볼을 대조하여 adapter·veneer·install_v74·request_trace가 없는 것을
확인했습니다. guest와 fixture에서 LD_PRELOAD도 비어 있는지 검사했습니다.

원본 common-util·BLM을 NOW로 로드하고 원본 파일 해시·실제 BLM mapping·
함수 진입 바이트를 검사했습니다. singleton/getter가 돌려준 원본 객체에
RaceAap::Init(true), VehicleDataManager::OrderSendVehicleData, RaceAap::UnInit을
호출했습니다. 폰 연결이나 AA 세션 시작은 수행하지 않았습니다. manager 내부
활성 상태나 세션 포인터를 수동으로 구성하지 않았고 원본 반환을 대체하지 않았습니다.

세 조건은 같은 VM·같은 aap_service를 사용하되 각각 새 fixture 프로세스로
실행했습니다. 각 프로세스 안에서는 같은 원본 객체로 세 주기를 반복했습니다.
원본 큐의 실제 std::function callback으로 pthread_self를 얻어 마지막에
큐 stop=100·join=0을 확인했습니다. 위치 자동 요청·LDS는 이번 실행 범위가 아닙니다.

## 실제 결과

| 조건 | 파괴 중 mutex 오류 16 | StopSession 반환 | fixture 종료 |
| --- | --- | --- | --- |
| 생성·송신 후 바로 파괴 | 3/3 주기 | 호출하지 않음 | 0 |
| 생성·송신 후 500ms 대기하고 파괴 | 2/3 주기 | 호출하지 않음 | 0 |
| 생성·송신 후 원본 StopSession을 호출하고 파괴 | 2/3 주기 | 세 번 모두 264(0x108), 오류 | 0 |

모든 주기에서 실제 send는 생성 전 256, 생성 후 0, 파괴 후 256이었습니다.
호출자의 48바이트 입력도 유지됐습니다. `/proc/self/task`는 각 프로세스에서
기준 task 2개, 생성 후 4개, 파괴 후 같은 기준 task 2개로 돌아왔습니다.
이후 같은 객체의 다음 생성도 성공한 로컬 send 경로로 진행됐습니다.

이 결과는 아홉 주기의 실제 반환·원본 재생성·추가 task 소멸·정상 프로세스
종료의 검사입니다. 모든 객체·mutex의 해제나 누수 부재를 증명하지 않습니다.
fixture의 PASS는 위 기능 검사에 한정하며, 수집한 원본 정리 오류는 그대로
실패 진단으로 남겼습니다. StopSession의 264도 성공으로 처리하지 않았습니다.

500ms는 비교를 위한 진단 대기입니다. 이 대기 뒤에도 두 주기에서 오류가
나왔으므로 정리 문제의 해결책이나 필요한 차량 대기 시간으로 채택하지 않습니다.
StopSession을 먼저 호출한 조건도 정리 오류를 없애지 못했고 해당 호출 자체가
실패했습니다. 미시작 세션에 대한 이 결과를 정상 연결 세션의 stop 계약으로
일반화하지 않습니다.

## 원본 종료 경로와 해석

해당 BLM의 정적 호출을 별도로 대조했습니다. ServiceTerm은 AapProc::UnInit을
호출하고, 그 함수의 정리 경로에는 connection manager·Bluetooth·RaceAap 등
다른 구성 요소와 Dbus의 UnInit이 있습니다. AapProc::Stop의 큐 동작은
stopPop이며 큐 thread join이 아닙니다. RaceAap::UnInit은 실제
aap_destroy_session과 sem_destroy를 호출합니다. RaceAap::StopSession은
실제 aap_stop_session의 반환을 돌려주고 260(0x104)인 동안 재시도합니다.

이번 fixture는 이 전체 SM/애플리케이션 종료 순서를 실행하지 않았습니다.
앞선 OBSERVE/SCRUB 실행과 지금의 baseline은 같은 원본 로컬 세션 API를
사용하지만 부하·요청 수·초기화 구성까지 완전히 같은 A/B 실험은 아닙니다.
그러므로 제품 후크의 모든 시간·경합 영향을 배제하거나 정상 차량 정리를
확인했다고 주장하지 않습니다. 지금 확보한 결론은 **원본 mutex 오류가
제품 후크가 없는 이 진단 조건에서도 발생한다**는 것입니다.

## 검증과 남은 작업

결과 검사기는 입력/빌드/initrd/console 해시, 제품 후크의 링크 부재, VM 격리,
세 case·아홉 주기의 실제 반환, task identity 복귀와 정상 종료를 대조했습니다.
기능 assertion 평가는 조건별 526/526/516회이며 입력 바이트 반복 검사를
포함합니다. 독립 시험 개수로 해석하지 않습니다.

guest 완료 뒤 외부 runner 제한으로 180.034초에 종료했습니다. runner=124,
QEMU=0, timed_out=true는 통과 기준이 아니며 각 fixture의 종료 0을 따로
확인했습니다. 원본 서비스 로그는 buffering으로 마지막 일부가 없을 수 있으며,
파괴 오류 집계는 각 fixture의 파괴 진입/복귀 표식 사이 출력에 한정합니다.

제품 소스·설치 ZIP은 변경하지 않았습니다. 전체 make test·제품 ARM 회귀는
재실행하지 않았고 이번 검사는 새 ARM 빌드 한 번·VM 한 번의 세 case와 결과
검사입니다. Codex가 직접 수행했으며 새 독립 리뷰는 수행하지 않았습니다.
Claude의 추가 커밋도 확인했지만 `e5d87c1` 이후 새 조사 기록은 없었습니다.

정상 전체 AA 초기화·정리, 정리 오류의 정확한 원인과 영향, 실제 요청 identity의
제품 연결, 센서 자격·폰 수용은 남아 있습니다. ASSIST와 배포 기본값은 변경하지
않았고 [v1.0 조건](../docs/V1_READINESS_KO.md)을 완료 처리하지 않습니다.

소스 기준은 `8ff2911523320a035e2b20a388eb1be04109afe5`입니다. 원본 입력은
앞선 AA 세션 기록과 같으며 작성한 source·compiler/ELF·init·build/run/검사
기록은 비공개 evidence에 보존했습니다. OEM 바이너리·전체 로그·정적 덤프는
게시하지 않습니다.

| 산출물 | SHA-256 |
| --- | --- |
| ARM fixture | `322aaa29c15f09d01a4f743acbcd7e1afa9cc109215bc5312bf035441c6bcb7f` |
| initrd | `2c07e8de6cc3f4f03bf30c09fc2ae077c8faa414833aec772daba30cddeec163` |
| console | `a240de4fc9650c1b7dd5e74f13826c770914245d2466854e0a64bf02f9f54b38` |
