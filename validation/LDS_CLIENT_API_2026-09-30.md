# 원본 LDS client의 다중 요청·오류 응답 — 2026-09-30

원본 LDS data client와 JCIDBUS의 공개 함수를 격리 ARM VM에서 호출했습니다.
두 요청, 64개 동시 대기 요청, 버스 부재, 제공자 부재의 네 경우에서 직접
작성한 callback의 결과와 fixture 종료를 검사했습니다. 제품 코드·설치물·
공개 ZIP은 변경하지 않았으며, ASSIST 연결이나 차량 검증의 완료가 아닙니다.

Claude가 별도로 수행한 [LDS 비동기 내부 관찰](LDS_ASYNC_2026-09-30.md)의
`88ea87d`와 범위를 보정한 `e5d87c1`을 확인했습니다. 이 기록은 Codex가 직접
수행한 별도 실행입니다. 아래 시험은 userspace debugger 없이 API와 작성한
callback만 관찰하므로 method·reply sender·정리 경계에 대한 새로운 증거로
세지 않습니다. 두 조사 모두 data client를 직접 호출하며, 제품 BLM이 거치는
`libjcilds-util`의 AA용 요청·수신기 cache·추가 callback 계층은 실행하지 않았습니다.

## 구성과 판정

- NA 74.00.324A 원본 rootfs와 Linux 3.0.35, QEMU 7.2.22/Sabrelite를
  사용했습니다. NIC·호스트 장치·공유 디렉터리는 없습니다.
- 기존 kernel-entry r1 machine ID 조정만 수행하고 userspace 이전에 detach했습니다.
  나머지 register packet byte와 원본 커널 코드는 보존했습니다.
- 진단 PID1이 원본 service/HMI 버스와 standalone LDS launcher를 시작했습니다.
  제품 preload·collector·SM·VBS·AA 세션은 시작하지 않았습니다.
- 고정 GCC 4.9.1·ARMv7 softfp로 fixture를 빌드했습니다. 원본 세 라이브러리의
  전체 파일 해시를 확인한 뒤 NOW로 로드하고 원본 API를 호출했습니다.
- 요청의 userdata는 fixture가 소유한 서로 다른 고정 storage이며 프로세스가
  끝날 때까지 유지했습니다. callback은 connection·userdata·호출 횟수·canary와
  작성한 main thread에서의 실행을 검사했습니다. 다른 스레드였다면 즉시 실패합니다.
  다중 스레드 경쟁 시험은 아닙니다.
- 오류가 있으면 빌린 오류 문자열을 callback 안에서 복사했습니다. 오류 응답의
  숫자 인자는 읽어 위치 값으로 해석하지 않았습니다.
- fixture별 deadline은 30초입니다. 실패·deadline·누락된 case·0이 아닌 종료는
  통과로 세지 않습니다. 별도 검사기가 console 해시와 네 case의 실제 제출·결과·
  완료·종료 marker를 다시 대조했습니다.

## r3 결과

| 경우 | 실제 호출과 결과 |
| --- | --- |
| 정상 제공자, 두 요청 | connect=1, 제출 두 건 모두 0, 제출 완료 시 callback 0건. dispatch 뒤 각 userdata에 정상 callback 1회, 종료 0 |
| 정상 제공자, 64개 동시 대기 | 64건 모두 0으로 제출되고 그때까지 callback 0건. dispatch 중 정확한 userdata에 각각 1회, 총 64회 수신. 관찰 중 중복·canary 변경 없음, 종료 0 |
| 존재하지 않는 버스 경로 | 해당 fixture 프로세스의 service-bus 주소만 존재하지 않는 경로로 지정. connect=0, 요청 제출·callback·disconnect callback 모두 0건, 종료 0 |
| LDS 제공자 부재 | 이 VM에서 시작한 launcher의 PID와 실행 파일을 대조한 뒤 SIGKILL. 종료 137과 bus의 `NameHasNoOwner` 응답을 확인. 새 연결은 1, 제출 두 건 모두 0. 각 userdata에 `org.freedesktop.DBus.Error.ServiceUnknown` 1회, 종료 0 |

정상 응답 66개의 mode·UTC·좌표를 포함한 아홉 값은 모두 0입니다. 유효하거나
새로운 측정값이 아닙니다. 제공자 SIGKILL은 폐기할 VM의 의도적인 장애이며
정상 차량 기동·종료·복구 시험으로 세지 않습니다.

64건의 dispatch 호출은 이번 실행에서 676회였으며, callback 수가 증가하지
않고 0을 반환한 호출도 있었습니다. **dispatch 성공은 새 위치 응답의 도착을
뜻하지 않습니다.** 이 횟수와 진단 출력에 영향을 받는 경과 시간은 성능·cadence
보증으로 쓰지 않습니다. 64는 이 fixture의 시험 크기이며 OEM 동시성 한계가 아닙니다.

네 fixture는 모두 종료 0, guest 집계는 failed cases=0이었습니다. guest의
`poweroff` 뒤 바깥 runner는 180.020초 제한으로 종료 124, QEMU는 종료 0,
`timed_out=true`였습니다. 성공 판정은 fixture 결과를 근거로 하며 VM 종료값이나
timeout을 통과 근거로 쓰지 않습니다.

## 앞선 시도와 남은 범위

- r1은 작성한 fixture가 connect 성공값을 0으로 가정하여 실제 반환 1에서
  요청 제출 전 종료 1이었습니다. 원본 BLM 호출부를 정적으로 대조해 nonzero가
  성공임을 확인한 뒤 fixture를 수정했습니다. 제품의 연결 코드를 수정한 것은 아닙니다.
- r2는 수정한 두 요청 시험을 먼저 실행하여 각각 callback 1회와 종료 0을
  확인했습니다. r3에서 위 네 경우로 확장했으며 이전 결과를 소급 변경하지 않았습니다.
- 명시적 disconnect/free, pending 취소, timeout, 재연결, 할당 실패와 응답 순서
  역전은 이 실행 범위에 없습니다. 관찰 종료 뒤의 중복까지 없다는 보장도 아닙니다.
- 같은 0 값으로 아홉 필드의 개별 numeric ABI를 모두 입증할 수 없습니다.
  BLM worker 큐·AA 송신·수신기와 세션의 자격·물리 센서·폰 수용도 남습니다.
- 제품 소스를 변경하지 않아 전체 host/ARM 제품 검사는 재실행하지 않았습니다.
  fixture ARM 빌드와 위 실제 원본 API 실행이 이번 추가 검사입니다.
- 이 fixture에 대한 Claude 텍스트 리뷰 호출은 Anthropic API의 `[cyber]`
  자동 심사로 거절되어 의견을 받지 못했습니다. 앞서 명시한 Claude의 독립
  실행 기록과 별개의 호출이며, 이 fixture의 독립 리뷰 완료로 세지 않습니다.

## 고정 입력과 비공개 산출물

| 항목 | SHA-256 |
| --- | --- |
| 순정 커널 | `57fe0cda7c1f994a6ca6b3cce7d9cf247f3b5607ebf1ddc12f02598662d5e240` |
| 원본 rootfs archive | `61bbcea608cc915f45a1775d4f49fb1603e4d92a0163311ced15220d03cf9d44` |
| 원본 common-util | `c44304ad0e493eb172e0e298ba5ce6d35ee5d021563dae97af5877852dc98e58` |
| 원본 JCIDBUS | `b44b2f462c09376747a380ee3010501e952f557759898fad01a0fdcd573d375f` |
| 원본 LDS data client | `bd4039da18c4039ba8d901ae49e917c9e7d22357d42ea930b43253eb55b8fbea` |
| r3 작성한 C++ source | `071aeaafa8eda317e43607eaf7cef73e377f2e1807b3834d039ecfd1b08f7f28` |
| r3 ARM fixture | `13250a92df71ae5993e5b2912e14ffcf56a4ff7c7c65c6a42e851726dcba96a0` |
| r3 진단 init | `5080a28ffb112609433f37d3a8586b5319214653a44fe3dc377d1e49a18848e5` |
| r3 initrd | `cf283aa2218c6c3c6761bfdd261977f4e5a59e19643998899150e4101bc2a75e` |
| r3 console | `7d366c374f7e81bf16144e74bb2c804712c2d34c5f56240b43b244cfeb0aa078` |

컴파일·이미지·실행 metadata와 각 시도의 소스를 비공개로 보존했습니다.
원본 파일·디스어셈블리·전체 console은 게시하지 않습니다.
