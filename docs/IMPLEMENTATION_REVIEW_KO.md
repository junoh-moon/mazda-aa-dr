> **2026-09-28: installation on hold.** Historical 0.1 analysis/instructions follow. [Current status](STATUS_KO.md) and [review corrections](REVIEW_2026-09-28_KO.md) supersede installation GO statements. No vehicle or phone validation has been performed.

# MX-5 v74 AA DR 구현 리뷰 자료 — 0.1

작성일: 2026-09-27. 이 문서와 함께 제공된 소스가 검토 대상이다.
이전 상세 설계는 목표 설계이며, 여기서 명시한 구현 상태가 현재 사실이다.

## 1. 목표와 현재 판정

대상은 2019 MX-5 ND2, 6MT, NA 74.00.324A의 1세대 MZD Connect이다.
사용자는 OEM Android Auto touch preload를 사용하고 있으며, Galaxy S25와 무선
AA 동글을 통해 네이버 지도 등에서 터널 내 차량 속도·회전 정보를 반영하기 원한다.
하단 USB 포트는 고장이고 정상 포트는 하나다. 운전 중 CMU 조작을 요구하지 않는다.

**현재 GO:** 해시가 일치하는 펌웨어에서 cold-load 관측 후크, 자동 로그,
mode=0 stale speed/bearing 제거 실험, 오프라인 DR 계산·송신 경계 검증.

**현재 NO-GO:** 실제 차량 센서를 입력으로 ASSIST 좌표 송신 활성화.
SMDB 값에는 신뢰할 수 있는 원래 측정 시각과 품질이 부족하다. 로그를 읽은 시각은
측정 시각을 대신할 수 없다. yaw 축·부호·bias와 센서 활성 조건도 실차에서 미확인이다.
설정 파일에서 ASSIST를 선택하거나 SHADOW를 선택해 이 조건을 우회할 수 없다.

지도 앱 수용 역시 미확정이다. 차량 송신 함수의 성공 반환은 폰 위치 융합 또는
네이버 지도·TMAP·카카오맵의 사용을 증명하지 않는다. 이전 사용자 후기나 다른
차량의 성공 사례가 이 조합에서의 성공으로 승격되지 않는다.

## 2. 이 패키지로 먼저 가를 수 있는 원인

| 관측 | 해석과 다음 경로 |
| --- | --- |
| NNG가 LDS data 이름을 소유하고 터널에서도 mode=3·속도가 변함 | 순정 DR 경로가 이미 존재. 자체 DR을 켜기 전에 AA 송신과 폰 수용을 조사 |
| LDS 소유, 터널에서 mode=0, 송신 속도·방향이 고정 | SCRUB 비교 실험 후보. 좌표 캐시는 여전히 남으므로 개선을 보장하지 않음 |
| SMDB yaw 값이 없거나 stale 여부를 구별할 수 없음 | 현재 SMDB 기반 ASSIST는 종료. 다른 검증 가능한 생산자 입력을 확보해야 함 |
| 입력·송신이 정상인데 폰 앱에서 반영되지 않음 | CMU 좌표 생성만으로 목표 달성 불가. 폰/AA/앱 경계의 별도 증거가 필요 |
| 해시·GOT·prologue·cold-start 조건 불일치 | 후크를 설치하지 않음. 다른 라이브러리를 임의로 건너뛰지 않음 |

GetNameOwner 뒤에 해당 unique name의 GetConnectionUnixProcessID와 `/proc/PID/comm`을
기록한다. 이것은 **관측 시점의 전역 소유자**다. 특정 AA 요청의 정확한 생산자 증명은
아니므로 ASSIST의 `Provenance`로 재사용하지 않는다. `GetSelectedGPS_sync` 역시
receiver 종류와 위치 알고리즘 소유자를 혼동하지 않는다.

SD로 NNG가 실제 활성화되는 경로는 가장 먼저 비교할 순정 대안이다. 이 구현은 NNG를
강제 실행하거나 프로파일의 DR 우선순위를 임의로 올리지 않는다.

## 3. 펌웨어 정적 근거

원본은 사용자 제공 NA 74.00.324A다. 코드 오프셋은 로드 베이스에 대한 RVA다.
전체 업데이트 이미지의 SHA-256은
`ffd04e2c8cfaf77388aacde0f9c1cddc17cb6b7f02d7caa2fe6ad39c0f40e787`이다.
배포 ZIP에는 원본 업데이트와 OEM 실행 파일을 포함하지 않는다.

| 항목 | 값 |
| --- | --- |
| BLM SHA-256 | `10e7235bfce075b44c1a8ffc99bbc9b63af85d9262df868ca1874ec36d8d3b71` |
| AA interface SHA-256 | `e9eb5e0d42719c98efc5ef86a270b4b3c86467aced55d4c8158bd99e06bbd436` |
| RequestSendPosition | BLM `0xC7460` |
| aap_send_vehicle_data GOT | BLM `0xF88BC` |
| aap_send_vehicle_data 구현 | interface `0x1A538` |
| BLM 첫 16바이트 | `10482de908b08de21cd04de200449fe5` |
| interface 첫 16바이트 | `70402de9ac429fe5acc29fe504408fe0` |

AA getter의 callback에서 Worker 큐를 거친 후 RequestSendPosition이 실행된다.
이 함수에서 OrderSendVehicleData와 하위 send 호출까지는 해당 바이너리에서 동기
호출이다. 따라서 **그 구간에만** TLS로 입력과 출력을 연결한다. 이름에 Order가
들어간다는 이유로 비동기라고 가정하지 않았고, 반대로 callback 이전까지 TLS가
연결된다고 가정하지 않는다. 다른 펌웨어에는 이 판단을 적용하지 않는다.

NNG의 GetPosition은 센서 융합 결과를 반환하는 mode=3 경로를 이미 가진다.
AA util과 BLM에도 mode=3 통과 경로가 있다. 따라서 “mode=3 허용 패치”만으로
해결된다고 볼 수 없다. DISTANCE_GYRO_2 eligibility의 해당 함수는 좌표 커버리지가
아닌 센서 유효성과 시간 간격을 검사한다. 다만 그보다 상위의 초기화 조건은 미확인이다.
`jci_ublox_m8.ini`의 `distance_gyro_2=0`은 해당 프로파일이 활성인 경우 전략을
등록하지 않는 효과가 있으므로 무작정 변경하지 않는다. 추가 근거는
`native_dr_followup.md`와 `research/native_dr_followup/`에 있다.

## 4. 로드와 후크

1. `jciAAPA` 서비스의 LD_PRELOAD에 이 라이브러리를 기존 touch 라이브러리 앞에 추가한다.
2. `dlopen` interposer는 정확한 `/jci/aapa/blmjciaapa.so` 경로만 대상으로 삼는다.
3. NOLOAD 조회에서 이미 로드된 것으로 확인되면 실행 중 코드 패치를 하지 않는다.
4. 최초 실제 로드는 RTLD_NOW로 GOT를 먼저 resolve한다.
5. dlopen 반환 직후, launcher가 GetServiceInterfaces를 쓰기 전에 패치한다.
6. 파일 해시, ELF ABI, module mapping, RX/RW 구간, 두 함수의 실제 바이트,
   GOT의 기존 목적지까지 일치해야 한다.
7. RequestSendPosition의 최초 8바이트를 별도 ARM trampoline에 보존한다.
   상위 veneer와 하위 GOT는 이 서비스 주소 공간에만 설치한다.

BLM의 `.init_array`는 정적 객체 초기화이며 조사된 생성자에는 서비스 생산자 스레드
시작이 없다. GetServiceInterfaces도 로그와 테이블 반환이다. 이 근거를 이용해
해당 최초 로드 경계에서만 코드를 쓰며 hot patch는 제공하지 않는다.
trampoline은 RW로 작성한 뒤 RX로 바꾸고 I-cache를 비운다. OEM 코드 페이지도
패치 순간 RW/NX이고, 반환 전에 반드시 RX를 복원한다.

RX 복원이 실패하면 계속 실행할 수 없으므로 writable 로그 디렉터리에
`disable-next-start` 표식을 저장하고 서비스 프로세스를 종료한다. 표식이 남아 있으면
다음 시작에서 후크를 건너뛴다. 원래 서비스 설정의 reset_board 동작으로 CMU가
재시작될 수 있다. 저장장치 오류로 표식까지 실패하면 자동 복구를 보장하지 않는다.
이 경로는 무조건적인 crash-loop 복구 체계를 의미하지 않는다.

## 5. 정확한 데이터 계약

Request 입력에서 확인한 필드는 mode `+0`, UTC seconds `+8`, latitude `+16`,
longitude `+24`, altitude int32 `+32`, heading double `+40`, velocity km/h `+48`,
horizontal `+56`, vertical `+64`이다. ARM32 VehicleData wrapper는
type uint32, pointer, length uint32의 12바이트다. send의 첫 인자는 session handle
자체가 아니라 handle을 저장한 영역을 가리키는 원래 포인터이며 그대로 전달한다.

LOCATION type=1의 payload 48바이트:

| Offset | 필드 |
| --- | --- |
| 0 | UTC ns uint64 |
| 8 / 12 | 위도·경도 int32 × 1e7 |
| 16 / 20 | accuracy 유무 byte / 값 × 1e3 |
| 24 / 28 | altitude 유무 byte / m × 1e2 |
| 32 / 36 | speed 유무 byte / m/s × 1e3 |
| 40 / 44 | bearing 유무 byte / degree × 1e6 |

SCRUB은 정확히 상관된 원래 mode=0, type=1, length=48에만 적용한다.
byte32와 byte40 및 36–39, 44–47을 0으로 만들고 나머지는 보존한다.
원본 메모리는 고치지 않는다. native mode 1·2·3, 상관 없는 호출, 중첩 호출,
잘못된 길이는 원래 OEM 호출을 한 번 실행한다. 같은 입력 안의 추가 LOCATION은
변경을 중단시키는 결함으로 취급한다. 반환 레지스터와 errno를 보존한다.

mode=0에서 오래된 좌표도 남아 있다. wire encoder의 timestamp=0 문제를 이 버전에서
수정하지 않는다. 이 두 잔여 문제 때문에 SCRUB 자체가 해결책이라고 결론 낼 수 없다.

## 6. DR 코어와 배포 차단

코어는 외부 I/O·메모리 할당·스레드가 없는 C99 상태 기계다. 검증된 GNSS anchor에서
시작해 속도와 yaw로 정확한 원호를 적분하고 WGS84 반경을 사용한다. 방향과 이동
bearing을 구별하여 후진을 처리한다. 불확실한 저속 상태를 0 이동으로 숨기지 않고
정지 확인과 오차 증가를 구분한다.

기본 개발 한계는 60초, 1,500m, 추정 오차 100m다. 이 오차는 실험 프로파일 값이며
안전성이나 실제 정확도를 보증하지 않는다. 이전 설계의 50m를 고정으로 계승하지
않았다. 장터널 완주나 지도 매칭을 구현하지 않으며, 현재 범위는 짧은 단절이다.
5분 터널을 이 코어만으로 해결한다고 주장하지 않는다.

**speed-only 직진 DR은 허용하지 않는다.** yaw가 없거나 fresh/valid임을 확인할 수
없으면 원래 출력으로 돌아간다. yaw 4094·4095, count=0, UNKNOWN 품질, 단순 polling,
lease 만료, epoch/generation 불일치, NaN, 시간 역행, 큰 적분 간격을 거부한다.
원본 yaw 평균 구간과 측정 시각은 상위 정규화 계층이 증명해야 한다.

`core_bridge`는 코어 snapshot을 adapter로 옮기는 순수 함수다. 64비트 세대 값의
32비트 축소 overflow를 거부하며 profile/input quality와 유효 기간을 명시적으로
요구한다. 미래 오차 한계까지 검증됐다고 자동으로 꾸미지 않는다.

현재 worker는 raw SMDB polling을 기록만 한다. 그 값을 DR의 VALID 입력으로 만들지
않으며 core→bridge→send의 live 연결은 활성화하지 않았다. 연결은 합성 통합시험에서
검증된다. `allow_assist=false`와 항상 false인 exact provenance callback이 런타임에
고정되어 있어 설정만으로 ASSIST를 켤 수 없다.

## 7. 관측·실패·복구

OEM 호출 안에서는 입력·출력 값을 POD로 복사하고 bounded queue에 trylock으로 넣는다.
파일·D-Bus·SMDB·해시는 hook 안에서 실행하지 않는다. 기록 worker가 JSONL을 작성한다.
로그 준비와 flush 성공 후에만 SCRUB을 켜며, 저장 실패나 queue 유실은 OBSERVE로
복귀시킨다. 이미 끝난 송신의 관측을 잃을 가능성은 있으므로 유실이 있으면 전체
추적이 완전하다고 주장하지 않는다.

worker는 고정된 read-only SMDB 필드 세 개와 D-Bus getter를 호출한다. 자식 명령에는
LD_PRELOAD를 전달하지 않고 출력 크기와 대기 시간을 제한한다. D-Bus getter는
auto_start=false다. GPS tty를 열거나 다른 서비스를 강제 실행하지 않는다.
로그 숫자는 worker 전용 C numeric locale로 출력한다.

설치 스크립트는 SHA-256 4개, 버전·지역·패치를 확인한다. sm.conf의 대상 서비스만
편집하며 sm_WCP.conf는 명시 옵션으로 선택한다. 임시 파일 검증 뒤 atomic rename을
사용한다. 두 파일 간 완전한 원자성은 없으므로 pending marker로 중단을 식별한다.
제거 스크립트는 자기 token만 삭제하고 과거 전체 백업을 덮어쓰지 않는다.

## 8. 검증의 경계와 다음 판정

수행한 빌드·테스트의 정확한 결과는 `VALIDATION.md`가 기준이다. 합성 DR 테스트나
QEMU의 ARM 실행을 실차 검증으로 바꾸어 표현하면 안 된다. 첫 차량 관측에서 필요한
것은 자동으로 남는 후크 설치 결과, receiver, owner/PID, 입력 mode, AA payload,
raw sensor 존재 여부다. SD 유무 등의 조건 변경은 주차 중에만 한다.

ASSIST 후속 구현의 선행 조건은 다음과 같다.

1. 실제 SD 없는 구성에서 필요한 센서가 생산되는지 확인.
2. 생산자 timestamp/sequence와 원래 품질을 보존하는 읽기 경계 확보.
3. yaw 부호·bias, 속도·후진·정지 신뢰도와 실제 지연을 검증.
4. 각 AA 요청의 LDS 소유/receiver와 session epoch를 정확히 연결.
5. 폰이 차량 LOCATION을 사용하고 목표 앱의 동작에 반영되는지 확인.

조건 1·2가 성립하지 않으면 현 SMDB 소스 경로를 폐기한다. NNG의 native DR이
이미 충분하면 자체 ASSIST 구현을 우선하지 않는다. 조건 5가 성립하지 않으면
CMU에서 더 정교한 좌표를 만드는 작업만으로 목표를 달성할 수 없다.

독립 검토는 Astra 하위 에이전트가 코어·후크·설치·런타임을 분담해 수행했다.
이번 구현에 Claude를 사용했다고 주장하지 않는다. 이 환경에서 Claude 호출 경로는
확보되지 않았다.
