# AA 작업 스레드의 스택 한도 — 2026-10-03

차 시험 전 감사(G3)에서 **설치된 AA preload의 작업 스레드가 순정 CMU의 스택 한도보다 훨씬 큰 프레임을 요구한다**는
결함을 찾았다. 서브에이전트의 정적 분석을 레포 쪽에서 소스·릴리즈 바이너리·순정 `init_cmu`로 다시 확인했고, 호스트에서
재현했으며, 수정과 회귀 검사를 추가했다. 차량에서 실행한 것은 없다.

## 확인한 것

| 항목 | 근거 |
| --- | --- |
| 작업 스레드는 기본 속성으로 만들어졌다 | 수정 전 `src/runtime/runtime.cpp`의 `pthread_create(&thread, 0, worker, 0)`, `pthread_attr_*` 호출 없음. 제품 코드의 스레드 생성은 이 한 곳뿐 |
| 작업 함수의 프레임 | 릴리즈 v0.3.11-shadow.2 `libmx5dr.so`의 `run_worker_association`(0x1e100) 프롤로그 `sub sp,sp,#0xa5000` 뒤에 `sub sp,sp,#0xde0`: 약 679 KB. v0.3.9의 `run_worker`도 약 469 KB(서브에이전트 확인) |
| 순정 스택 한도 | 순정 `/sbin/init_cmu`의 한도 표(0x277a8)에 `RLIMIT_STACK` 소프트 `0x20000` = **128 KiB**, 하드 무제한. 코어 무제한, NOFILE 4096, MSGQUEUE 100 MB도 같은 표 |
| 기본 스레드 스택 | 순정 libc는 glibc 2.11.1이고 nptl은 `RLIMIT_STACK` 소프트 값을 기본 스택으로 쓴다. SM이 이를 바꾸지 않는다는 점은 서브에이전트의 추정이다(`sm_stack_size` 등이 −1이라 `setrlimit`을 건너뜀) |
| 호스트 재현 | `ulimit -s 128`에서 워커를 쓰는 시나리오 19개가 전부 분할 오류(139). 기본 8 MB에서는 모두 통과 |

따라서 설치본이 jciAAPA 안에서 작업 스레드를 시작하는 순간 128 KiB 스택에 약 679 KB 프레임을 잡으려 하고, 첫 호출이 스택 밖을
건드려 `boot` 행을 쓰기 전에 죽을 가능성이 높다. jciAAPA는 `reset_board="yes"`, `retry_count="0"`이라 CMU 재시작으로 이어질 수
있다. 이전 VM·QEMU 시험은 호스트의 8 MB 한도에서 돌았기 때문에 이를 보지 못했다.

**이것이 2026-10-02 야간 부팅의 관찰과 맞는다**: 일회성 가드가 소비됐고(`consumed` v2, `last-boot` 불일치), collector 기록은
약 27 KB(대략 30~90초)이며, AA의 `boot` 행과 위치 기록은 없었다. 가드가 시험 설정을 SM에 넘기고 collector를 시작한 부팅이
jciAAPA 기동 무렵에 끝났다면 그 모양이다. 다만 **원인이라고 확정하지 않는다.** 그 부팅이 왜 끝났는지는 기록이 없고, v0.3.9의
첫 주행 시험(가드가 소비되지 않음)은 다른 이유로 실행되지 않았을 가능성이 높다.

## 수정

- `src/runtime/worker_thread.h`: 스택 크기(4 MiB, 가상 예약만 하고 건드린 쪽만 실제 메모리를 쓴다)와 detach 여부를 지정하는
  `create_thread`. 제품은 이를 통해서만 스레드를 만든다. 생성에 실패하면 기존처럼 OBSERVE로 내리고 무효화한다.
- 큰 지역 변수를 힙으로 옮기는 변경은 하지 않았다(300줄 함수를 건드리는 큰 변경). 프레임 자체를 줄이는 일은 열린 개선 후보다.

## 검사 (`make test-runtime` 통과, 종료 0)

- `tests/runtime/test_worker_thread.cpp`: 자기 자신을 128 KiB 한도로 다시 실행한 뒤, 기본 속성 스레드가 700 KB 프레임에서 죽음(전제)과
  헬퍼 스레드가 같은 프레임을 실행함(joinable과 detached), 잘못된 스택 크기를 거부함을 확인한다.
- `tests/build/test_thread_policy.py`: 제품 코드가 헬퍼 밖에서 `pthread_create`나 `std::thread`를 쓰면 실패한다. 옛 코드에서 실패함을
  확인했다.
- 두 고충실도 테스트(`test_runtime_assist`, `test_runtime_lds_association`)는 워커 스레드를 같은 헬퍼로 만들도록 바꿨다. 이 테스트들의
  `main()` 프레임은 128 KiB보다 커서(gdb로 `main`에서의 분할 오류 확인) 128 KiB 한도로는 돌릴 수 없고, 256 KiB 이상에서 통과한다.

## 같은 감사에서 나온 나머지 위험 (미해결)

1. **AA 워커가 VBS·LDS 수집의 단일 수신자다.** 모션과 LDS 소켓 채널은 jciAAPA의 워커 안에서만 열린다. 워커가 시작하지 못하거나
   죽으면 VBS와 LDS의 모든 데이터가 조용히 버려지고, 남는 흔적은 각 tap의 stderr뿐이다. 문서의 "AA가 실패해도 수집은 유지된다"는
   표현은 MODEL·감사 오류에만 맞는다.
2. `bootstrap`의 `RESTORE_FAILED_FATAL` 경로는 `disable-next-start`를 쓰고 `_exit(126)`한다. 의도된 동작이지만 jciAAPA에서는
   CMU 재시작이다. 일회성 가드의 복귀가 이 시점의 재시작을 덮는지는 확인이 필요하다.
3. 순정 스레드들의 실제 스택 크기를 모른다. `libjci.so`와 `libjcicommon.so`가 `pthread_attr_setstacksize`를 가져오고, hook이 그 스레드들에
   프레임 약 1.5~1.6 KB씩 더한다(`position_enter` 1520 B, `send_vehicle_data` 1608 B). 가장 작은 크기를 정적으로 확인하고 그 크기에서
   `small_stack_test`를 다시 해야 한다.
4. 세 서비스가 모두 root로 도는지가 추정이다(모션 수신기가 송신자 uid를 AA의 `getuid()`와 비교한다). 아니라면 모든 데이터그램이
   거부되고 `shadow_input_reset` 행이 쌓인다.
5. 이 설치본으로 **주차한 채 확인할 수 있는 것**: `/proc/<jciAAPA pid>/limits`의 스택 한도(131072 예상), `/proc/<pid>/maps`(preload
   적재), `/proc/<pid>/status`의 `Uid:`. 현재 상태 메뉴와 회수 도구는 이를 수집하지 않는다.

## 한계

수정 뒤의 ARM 빌드에서 워커 프레임과 호출 깊이를 다시 재지 않았고, 실제 CMU에서 확인한 것은 없다. 4 MiB는 측정한 필요량이 아니라
679 KB 프레임에 여유를 둔 값이다. 이 기록은 설계 변경이나 live ASSIST 활성화를 승인하지 않는다.
