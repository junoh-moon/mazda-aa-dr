# NA 74.00.324A 센서 수신 계약

2026-09-29 제공 펌웨어의 정적 분석 결과다. OEM 바이너리를 실행하거나 차량에서 수신을 확인한 결과는 아니다. 이 문서는 `src/sensors/vim_source.cpp`와 `vim_tap.cpp`의 근거이며, 물리 센서 보정·실제 지연·내비 앱 수용을 보증하지 않는다.

2026-10-01에 같은 업데이트의 VIP 생산자를 추가 조사했습니다.
[선택한 원본 명령어의 해석 실행과 MODEL 수정](../validation/VIP_ACCUMULATOR_2026-10-01.md)은
아래 최초 정적 분석과 구분하십시오. MCU 전체나 실제 CAN을 실행한 결과는 아닙니다.

## 선택한 경로

`VIM의 SPI 수신 → 기존 IPC 전송 → VBS의 기존 VIMC 콜백 → 관찰 사본 → 비차단 로컬 데이터그램 → AA SHADOW 계산`

VIMC의 **기존 VBS 등록을 감싸서** 데이터를 복사한다. 새 IPC 구독자를 등록하거나 TCP 7035에 접속하지 않는다. 순정 콜백에는 같은 포인터와 인자를 정확히 한 번 전달하며, 원본 반환값과 `errno`를 보존한다. 순정 센서 데이터와 AA 위치 송신은 수정하지 않는다.

독립 VIMC 구독도 API상 가능하지만 채택하지 않았다. 원본 IPC 서버는 클라이언트 큐를 `O_WRONLY`로 열고 타임아웃 없는 `mq_send`를 호출한다. 추가 수집기가 죽으면 남은 큐가 차서 VIM의 전송 스레드가 막힐 수 있다. 이 때문에 신규 구독 구현은 제거하고, 이미 존재하는 VBS 수신 경계만 관찰한다.

## 콜백 ABI

ARM32에서 다음 구조체는 12바이트다.

```c
struct VimMessage {
    uint32_t id;            /* +0 */
    uint32_t length;        /* +4 */
    const uint8_t *data;   /* +8 */
};
typedef void (*VimCallback)(uint64_t client, uint64_t server,
                            const struct VimMessage *message);
struct VimCallbacks { VimCallback app_event; };
int VIMC_AddClient(uint64_t client, uint64_t server,
                  const struct VimCallbacks *callbacks);
```

`VIMC_AddClient`의 세 번째 C 인자(ARM 레지스터 배치상 스택에 놓이는 다섯 번째 워드)는 **함수 포인터를 담은 테이블의 포인터**다. 함수 포인터 자체가 아니다. 원본 API의 성공 상태는 100이고, IPC 실패를 102로 변환하는 경로가 있다. 후크가 원본을 찾지 못해 102를 반환하는 경우는 후크의 등록 실패이며, 원본 호출 결과로 가장하지 않는다.

VBS 초기화는 `IPCAPI_MakeApiHandle(0x80000068, 0)`을 호출한다. 이 함수는 `(uint64_t(key) << 32) | local_handle`을 반환한다. `VIMC_Open`은 고정 서버 키 `0x80000067`에 대해 `IPCAPI_MakeApiHandle(key, 0)`으로 출력 핸들을 만든다. 따라서 이 펌웨어의 VBS 등록에는 client=`0x8000006800000000`, server=`0x8000006700000000`을 검사한다. 다른 호출이나 소유자는 관찰하지 않고 그대로 전달한다.

## 원본 필드와 시간

VIM은 원 SPI 헤더의 16비트 ID와 8비트 길이를 읽고, SPI 버퍼의 `+3`부터 payload를 복사한다. 아래 오프셋은 **콜백 `data` 기준**이며 VDT의 public 이벤트 구조체와 다르다.

| VIM ID | 필드 | 최소 payload 길이 |
|---|---|---:|
| `0x100` | 네 바퀴 raw 값: little-endian u16, `+1/+3/+5/+7` | 9 |
| `0x116` | yaw 누적 raw: little-endian u16 `+1`, count: u8 `+3` | 4 |
| `0x118` | 후진등 raw 상태: u8 `+1` | 2 |

이 VIMC 채널은 12바이트 헤더를 포함해 최대 28바이트를 설정하므로 파서는 payload 최대 16바이트를 허용한다. count=0, raw sentinel, 미확인 후진 enum은 원본 그대로 보존한다. 파서 성공을 센서 유효 판정으로 취급하지 않는다. VDT의 다른 이벤트 번호 `0x118`을 후진으로 해석해서는 안 된다.

2026-10-10 진단 부채널(로그 전용, [기록](../validation/VIM_CHANNEL_CAPTURE_2026-10-10.md)): 위 세 종류의 모션 이벤트와 receive_seq는 그대로 두고, 같은 콜백이 받은 `0x116` 전체 payload(+4 Qf, +5..+6 종가속도, +7..+8 브레이크 압력, +9 Qf)와 `0x169`(+1 Qf, +2..+3 횡가속도), `0x15B`(+1..+2 차속, +3..+4 엔진 회전수, +11 상태)를 해석하지 않고 복사해 별도 비차단 소켓(`<모션 채널 이름>.ch`)으로 묶어 보낸다. 이 값은 persistent 로그의 `chan_digest` 원시 통계에만 쓰이며 MODEL·BETA·어댑터 입력이 아니다. 단위·부호·영점은 미확인이다.

VIM이 만드는 `CLOCK_MONOTONIC` 밀리초 타임스탬프는 TCP 전송용 지역 구조체에만 들어간다. 이 IPC 경로의 원 SPI payload에는 추가되지 않는다. 따라서 관찰 이벤트는 실제 **콜백 수신 시각**만 기록하고 `source_mono_ms=0`을 유지한다. 새로 붙인 sequence도 관찰 순번이며 센서의 생산 순번이 아니다.

후진등의 TCP 알림과 `/tmp/RvrseLmpReq` 캐시는 변경 시에만 갱신되지만, IPC 큐 호출은 그 뒤 공통 경로에 있다. 값이 같아 TCP 알림을 생략해도 **실제로 수신한 후진 SPI 이벤트는 IPC로 전달**한다. 이 사실은 SPI 자체의 주기나 6MT 실차 enum을 증명하지는 않는다. 캐시 파일은 사용하지 않는다.

VIP의 조사한 요레이트 생산 경로는 12비트 표본을 16비트 합계와 8비트 count에
누적한 뒤 10바이트 payload로 보냅니다. 표본 `4094`는 건너뛰고 `4095`는
누적합니다. count는 생산 순번·시각이 아니며 합계와 count 모두 넘칠 수 있습니다.
MODEL은 count가 1–255 범위 밖이거나 `합계 + 65536 <= count × 4095`이면
서로 다른 합계를 구별할 수 없으므로 계산을 초기화합니다. 통과한 값도 count
넘침·오류 표본 혼합·신선도까지 입증한 것은 아닙니다. raw 사본과 기존 파서의
복사 계약은 보존하며, 이 검사를 ASSIST 자격으로 사용하지 마십시오.

## 한정 조건과 실행 보호

- 유효한 `SHADOW` 설정과 비활성화 표식 부재, ARM32, 일치하는 원본 해시가 필요하다.
- VIM API 등록 함수와 CAN 콜백의 실제 로드된 파일 해시·주소, 등록 테이블 주소 및 핸들을 검사한다.
- 다른 preload가 등록 함수를 제공하면 그 호출 체인을 그대로 통과시키고 관찰하지 않는다. 이미 로드된 라이브러리 조회는 `RTLD_NOLOAD | RTLD_LAZY`를 사용한다.
- 로더 조회·설정 읽기·해시 확인은 등록 mutex 밖에서 수행한다. 초기화 중 재진입하거나 다른 스레드가 등록하면 기다리지 않고 원래 테이블을 전달한다.
- 최대 8개 불변 콜백 경로를 유지한다. 같은 등록은 재사용하며, 한도 초과는 순정 테이블을 그대로 전달한다. 이전에 큐에 들어간 콜백의 원래 소유자를 덮어쓰지 않는다.
- 콜백 안에서 대기·재시도·힙 할당·파일 쓰기를 하지 않는다. 미리 연 데이터그램 소켓에 비차단 전송하며, 유실은 관찰 sequence의 불연속으로 드러난다.
- 별도 `jciVBS` 서비스 안에 로드되므로 해당 서비스에도 제한된 시험 부팅과 다음 부팅 복구 보호가 필요하다. AA 프로세스의 기존 보호만으로 충분하다고 간주하지 않는다.

## 정적 분석 식별자

주소는 아래 해시의 ELF 가상주소이며 실행 시 load bias를 더해야 한다.

| 파일 | SHA-256 | 관련 경계 |
|---|---|---|
| `vim_app` | `1d3657cfe451dcdc02920b682bee3196fefaa431c3516f774d0ba603cf118605` | `iuc_Rx_Process_Request` 공통 enqueue `0xdd5c`; `iuc_queueMessage` `0xf0b0` |
| `libjcivim_api.so` | `c9a8409743e304fc096336f90f9b262a93bc996f3e56a3a0bf5012672da0256b` | `VIMC_AddClient` `0x1070`; client event adapter `0x173c`; server fanout wrapper `0x1d00` |
| `libjcimod_can.so` | `bc478f2409e473ef3da8084ebce3b699643b5ee355485122521099d47c78ef22` | 원본 콜백 `0xa8b0`; 테이블 `0x4625c`; 기존 등록 호출 `0xb6d8` |
| `libjcipcapi.so` | `2304e20be64eec57e5a2b69af92b421484d7a4be1bbb666e43ee10a3fca7e161` | fanout `0x546c`; 서버가 클라이언트 큐를 여는 경계 `0x2588` |
| `libjcipc.so` | `b5b59f7270b372672b27292834027d2099542311f72a4cbf0550eeb423735e82` | 송신 큐 플래그 선택 `0x15fc–0x1604`; `mqSendXid` `0x2044` |

`tests/sensors`는 합성 데이터로 payload 해석, 등록·반복·한도·NULL 처리, 원본 포인터와 반환/errno 보존, 변경 전 복사, 관찰 유실을 검사한다. 테스트 전용 소유권·전송 주입은 테스트 소스의 `MX5_VIM_TAP_TESTING`에만 있고 생산 빌드에는 없다. 호스트 및 고정 ARM 툴체인/QEMU 통과는 실제 CMU 로딩·수신·복구 시험을 대체하지 않는다.
