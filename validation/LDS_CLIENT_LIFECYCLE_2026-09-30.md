# 원본 LDS API의 연결 종료·지연·AA util 응답 — 2026-09-30

[앞선 다중 요청·장애 시험](LDS_CLIENT_API_2026-09-30.md)의 r3 뒤에 Codex가
직접 수행한 r4–r6입니다. 실패한 시도도 보존했습니다. **전체 통과 기록이 아닙니다.**
동일 connection 객체·동일 bus 이름의 재연결은 실패했으며 원인을 아직 분리하지
못했습니다. 제품 코드·공개 ZIP·ASSIST 허용은 바꾸지 않았습니다.

Claude의 별도 [LDS 내부 관찰](LDS_ASYNC_2026-09-30.md)을 읽고 공개 함수의
장애 응답과 연결 수명 시험을 보강했습니다. 이 실행은 userspace debugger나
내부 객체 읽기 없이 원본 API와 직접 작성한 callback을 검사했습니다.
method·pending의 실제 해제, memory leak 부재, BLM worker 큐의 수명 증거가 아닙니다.

## 실행과 실제 결과

환경은 앞선 시험과 같은 NA 74.00.324A 원본 rootfs·Linux 3.0.35,
QEMU 7.2.22/Sabrelite, 고정 GCC 4.9.1·ARMv7 softfp입니다. 기존 kernel-entry
machine ID 조정 뒤 userspace 전에 detach했습니다. NIC·호스트 장치·공유
디렉터리는 없으며, 원본 서비스 함수·응답·반환값을 대체하지 않았습니다.

| 경우 | 실제 결과와 범위 |
| --- | --- |
| 응답 전 free → 새 connection 생성 | r4·r5·r6에서 성공. 이전 두 요청의 userdata callback은 0회, 새 두 요청은 각각 1회. 모두 같은 connection 주소 재사용을 관찰. 마지막 disconnect/free도 복귀하고 fixture 종료 0 |
| 응답 전 disconnect → dispatch → 같은 객체·이름으로 connect | r4는 disconnect 뒤 dispatch가 -1일 것이라는 fixture 가정에서 실패. 실제 반환 0. r5는 다음 connect 반환 0에서 종료 1 |
| 응답 전 disconnect → 같은 객체·이름으로 바로 connect | r6에서 중간 dispatch를 제거해 대조했으나 connect=0, fixture 종료 1. free/recreate 대조 case는 같은 실행에서 성공 |
| LDS 제공자를 멈춘 상태에서 timeout 기대 | r4는 두 요청 제출 뒤 45초 fixture deadline으로 종료 124. 정상 timeout callback 완료를 확인하지 못함 |
| 멈춘 제공자에 대한 timeout 대조와 재개 | r5에서 명시적 1,500ms timeout의 원본 dbus-send는 NoReply로 종료 1. 동시에 대기 중인 LDS client의 두 요청은 아래 45초 관측 시점까지 callback 0회. 제공자 재개 뒤 두 요청 모두 원래 userdata로 정상 callback 1회, fixture 종료 0 |
| AA용 util API, 제공자 실행 중 | r5에서 원본 LDS_DATA_GetPosition_AA 두 호출이 0 반환, 각 userdata로 callback 1회. error NULL, mode·UTC·좌표 등 아홉 값 모두 0, fixture 종료 0 |
| 제공자 종료 후 data client와 AA용 util 비교 | 원본 launcher 종료 137과 NameHasNoOwner를 확인한 뒤 차례로 호출. data client의 두 callback은 ServiceUnknown. 별도 AA용 util fixture의 두 callback은 error NULL이고 아홉 값 모두 0. 두 fixture 종료 0 |

free/recreate 시험은 원본이 만든 connection을 원본 free 함수로 해제하고 다시
원본 create 함수를 호출했습니다. free 뒤 객체를 읽지 않았습니다. userdata 네 개는
시험 프로세스가 끝날 때까지 살아 있으며, 이전 두 개와 새 두 개를 구분해 호출 횟수·
canary를 검사했습니다. 관찰 종료 뒤의 모든 callback, 경쟁 상태 또는 내부 자원
정리까지 보장하는 결과는 아닙니다. disconnect callback은 이 실행에서 0회였습니다.

## 지연 시험에서 확인한 것

r4의 deadline 기록만으로 callback이 전혀 없었다고 단정하지 않습니다.
이전 fixture는 callback 진입을 바로 출력하지 않았으므로, 정확한 결과는
**45초 내 시험이 완료되지 않았다**는 것입니다.

r5는 callback 진입마다 즉시 기록하고, timeout signal의 callback count도
signal-safe 출력으로 보강했습니다. guest는 다음 순서로 별도 fixture를 실행했습니다.

1. 직접 시작한 LDS launcher의 PID·실행 파일을 대조하고 SIGSTOP 뒤 상태 T를 확인했습니다.
2. fixture의 두 요청이 모두 0으로 제출되고 callback이 0인 marker를 기다렸습니다.
3. 같은 제공자에 원본 dbus-send로 1,500ms 제한의 GetPosition을 보냈습니다.
   외부 5초 deadline에 도달하지 않고 실제 NoReply 오류와 종료 1을 확인했습니다.
4. 추가 45초를 기다린 뒤, fixture가 살아 있고 callback 진입 기록이 0개임을 확인했습니다.
5. 제공자를 SIGCONT로 재개했습니다. 같은 fixture와 connection에서 재제출 없이
   원래 두 userdata의 callback이 각각 1회 돌아왔고 전체 검사 뒤 종료 0이었습니다.

이 45초는 guest의 진단 대기 구간이며 차량 응답 지연 상한이나 정밀 성능 측정이
아닙니다. 원본 LDS data client의 timeout 인자 -1이 pending을 거쳐 libdbus까지
변경 없이 전달되는 정적 호출·인자 흐름도 대조했습니다. 해당 fixture 경로의
timeout 처리 원인은 미분리이며, -1을 무조건 무한 대기 또는 특정 초 수의
정상 timeout이라고 단정하지 않습니다.

늦은 요청을 경과 시간만으로 소멸했다고 판단해서는 안 된다는 실제 대조가 됐습니다.
[요청 관측 컴포넌트](REQUEST_TRACE_2026-09-30.md)의 실제 정리 경계 요구를
timeout 추정으로 대체하지 않습니다.

## AA용 util의 오류 전달

r5는 원본 LDS control client와 util까지 해시 확인 후 NOW 로드하고,
BLM이 import하는 공개 함수 LDS_DATA_GetPosition_AA를 직접 호출했습니다.
작성한 callback의 ABI는 앞선 data client와 같으며 connection·userdata 대응을
검사했습니다. 생산자의 위치 응답을 덮어쓰거나 수신기 cache를 변경하지 않았습니다.

제공자 실행 중과 종료 후의 두 AA용 API 결과가 모두 error NULL·mode 0·0 값으로
보였습니다. 이는 기존 정적 감사에서 확인한 util의 오류 인자 평탄화와 맞습니다.
동일 wire 요청을 data client와 util 양쪽에서 추적한 결과는 아니며, 원래 error의
관측 연결은 여전히 별도 구현 대상입니다. 상위 callback의 NULL error만으로
하위 요청 성공을 판정할 수 없습니다.

이번 fixture는 LDS_UTIL_Init/Uninit, 원본 BLM callback, Worker::PostWorker,
doWork, AA 송신·세션·폰 API를 호출하지 않았습니다. 원본 util의 공개 요청 함수까지
실행한 결과이며 제품 전체 초기화·수신기 전환·queue 경로를 실행한 것으로 확대하지
않습니다. 정상 위치 값과 물리 센서는 이번에도 없었습니다.

## 실패 보존과 판정

- r4는 8개 case 중 2개 실패입니다. disconnect 이후 dispatch에 대한 fixture
  반환값 가정을 원본 코드와 대조했습니다. wrapper는 내부 처리 뒤 0을 반환하고
  내부 반환값을 전달하지 않습니다. watcher 조건을 연결 상태 검사로 오해한 가정이었습니다.
- r5는 9개 case 중 동일 객체 재연결 1개 실패입니다. timeout 기대를 통과로
  바꾸지 않고, 별도의 제공자 지연·재개 실험으로 원래 요청의 뒤늦은 완료를 확인했습니다.
- r6는 직접 재연결과 free/recreate의 두 case 중 직접 재연결 1개 실패입니다.
  같은 객체 재연결 실패를 중간 dispatch만으로 설명할 수 없었습니다. 이름·연결·
  watcher 상태 등의 정확한 원인은 아직 분리하지 않았습니다.
- 별도 결과 검사기는 console 해시, case 순서, 각 종료·실패·callback·최종 집계를
  대조했습니다. 검사기가 기록과 일치함을 확인한 것과 전체 시험 통과를 구분했습니다.
- 각 guest 완료 뒤 바깥 runner는 r4 180.036초, r5 180.038초, r6 120.020초
  제한으로 종료 124, QEMU는 종료 0이었습니다. 세 경우 모두 timed_out=true입니다.
  VM 종료값이나 경과 시간은 API 시험의 통과 근거가 아닙니다.
- 제품 소스 변경이 없어 전체 host/ARM 제품 검사는 재실행하지 않았습니다.
  이번 추가 검사는 세 고정 ARM fixture 빌드와 위 VM 실행·정적 계약 대조입니다.
  r4–r6의 독립 리뷰는 수행하지 않았습니다.

## 비공개 입력·산출물 식별자

커널·rootfs와 세 기본 라이브러리 해시는 [r3 기록](LDS_CLIENT_API_2026-09-30.md)과
같습니다. 추가 원본 control client는
`26103b7e98bbe241704054552c4f9a8ea4106b923cb527a32f61a3a752d1eade`,
util은 `f1315887634917af604943e828a3ac752c5c375a0ec3b05e59c867e38293135c`입니다.

| 시도 | ARM fixture SHA-256 | initrd SHA-256 |
| --- | --- | --- |
| r4 | `3bd7731919af4fc625a24974ea672fc6bc8ecfbd861609f22c0d06289c0435c0` | `c6908e8aeb1b73cf3a63b06106e63b1bd41015faf0474ced727c44cfdb5ebe62` |
| r5 | `e6a3c5afa29b3e2f4fd89bcc841ae5679de1f581633fb27ee2ff3b09dbb3d9fc` | `d162db43b4d3d864544ef68c5acc3914a6487482cb223090be23b1444f9e34a6` |
| r6 | `0f25c16d533352988d01a80ddc528e174c7737e74b0312120dbd7454f62715a6` | `abe239626a26aac18c40866a0ffa897dc5c3447e6eaeca588e0ca08ae315ef50` |

| 시도 | console SHA-256 |
| --- | --- |
| r4 | `23c0e5fda4602133559f5fcec2ebc1fff609f1a42d5bc6806eafed23b7a7d0d5` |
| r5 | `076b367105e6e1df10c95a356b916b27b9adeb0bbee8aacebbc4750cb42fcb0b` |
| r6 | `bffae81421d680b88609f4cd75091de96dcc14439fa778f440d38d0be610d680` |

각 소스·진단 init·컴파일·이미지·실행·검사기 기록을 비공개로 보존했습니다.
원본 바이너리·디스어셈블리·전체 console은 게시하지 않습니다. 실제 요청 출처의
제품 연결, 센서·시간·보정 자격과 BLM 이후 경로는 아직 미구현 또는 미검증이며
[v1.0 완료 조건](../docs/V1_READINESS_KO.md)을 완료 처리하지 않습니다.
