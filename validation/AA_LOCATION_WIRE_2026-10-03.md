# 원본 aap_service의 LOCATION 직렬화 — 2026-10-03

DHU 실험([2차](DHU_NAVER_EXP2_2026-10-03.md), [3차](DHU_NAVER_EXP3_2026-10-03.md))에서 네이버는 **정확도가 없는
fix를 무효로** 보았다. 실차의 Mazda가 GPS 손실 중 정확도 표시를 지운 채 재송신할 때 wire에
실제로 무엇이 실리는지, 그리고 정확도를 담으려면 무엇을 써야 하는지를 **차량 없이 펌웨어에서**
확인했다. 서브에이전트가 NA 74.00.324A 원본 `aap_service`·`libaap_interface.so`·
`blmjciaapa.so`(SHA-256 `10e7235b…3b71` 일치)를 정적 분석하고, 직렬화 코드를 에뮬레이션했다.
원본 바이너리와 디스어셈블 덤프는 싣지 않는다. 주소는 기존 문서와 같은 방식의 해석 기록이다.

## 확인한 것 (정적 분석)

- 송신 체인: BLM이 48바이트 native LOCATION을 만들어 `aap_send_vehicle_data`로 넘기고,
  `aap_service`의 `AAPSensorSource::send_sensor_data`(type 1, 길이 정확히 0x30 필요) →
  `send_location_data`(0x50274) → `SensorSource::reportLocationData` →
  `populateLocationData`(0x17F5F0)가 protobuf `LocationData`를 채운다.
- `populateLocationData`는 **timestamp를 항상 0으로** 설정한다(인자로 받은 구조체의
  timestamp는 읽지 않는다). 위도·경도는 항상 설정한다. **accuracy, altitude, speed, bearing은
  각 has 플래그가 0이 아닐 때만** 설정한다. 직렬화(`SerializeWithCachedSizes`)는 has-bit이
  선 필드만 쓴다. 필드 번호는 timestamp 1, latitude_e7 2, longitude_e7 3, accuracy_e3 4,
  altitude_e2 5, speed_e3 6, bearing_e6 7이다.
- 따라서 `hasAccuracy=0`이면 **accuracy(필드 4)는 wire에서 완전히 빠진다.** 0도, 원래 값도
  아니다. 같은 변환 없이 값이 그대로 전달되므로 정확도 N m는 `hasAccuracy=1`,
  `accuracy = N × 1000`(예: 20 m → 20000)이다. 범위 검사·클램프는 없다.
- 모드별 처리는 `RequestSendPosition`(0xC7460)에 있다. `MakeLocation`은 모드 로직 없이 항상
  `hasAccuracy=1`, `accuracy = 2×HDOP×1000`(HDOP는 위치의 horizontal 필드)으로 채운다.
  - 모드 0, 캐시 있음: 캐시를 복사한 뒤 hasAccuracy=0, accuracy=0으로 지우고 송신한다.
    좌표·속도·방위·timestamp 원본 값은 그대로이다.
  - 모드 3(DR): MakeLocation 뒤 hasAccuracy=0으로 지우고 송신한다. 순정 DR 출력도 정확도가
    wire에 실리지 않는다.
  - 그 밖의 모드(1, 2): 정확도를 담아 송신한 뒤 캐시에 복사한다.
- 훅 지점부터 wire 사이에서 값이 바뀌거나 버려지는 곳은 없다(BLM `SendVehicleData` →
  `aap_send_vehicle_data`는 12바이트 wrapper를 통과시키고, 길이 0xF97 초과만 거부). 메시지가
  통째로 버려지는 경우는 type≠1, 길이≠48, IPC/handle 실패, `aap_service`의 "센서 시작" 게이트
  (`helper+0x85`)뿐이다.

## 에뮬레이션 확인 (Unicorn)

원본 `aap_service` ARM 코드를 Unicorn으로 실행했다(libc·할당자·helper/SensorSource 객체는
대체). `sendSensorBatch` 진입 시점의 batch를 캡처했다.

- 순정 모드 0 형태(hasAccuracy=0, accuracy=9999): 필드 4가 없고, timestamp는 0, 고도·속도·방위는
  존재했다.
- 같은 구조체에 hasAccuracy=1, accuracy=20000: `20 a0 9c 01`(필드 4 = 20000)이 추가됐다.
- hasAccuracy=2도 1과 같게 동작했고, accuracy=0xFFFFFFFF는 4바이트 최댓값 varint로 나갔다.

실행하지 않은 것: batch 경로, MessageRouter 프레이밍·암호화, 폰까지의 전송, 실제 BLM/IPC 프로세스.
QEMU 전체 VM은 필요하지 않았다.

## DHU 결과와의 연결

- 순정은 정확도를 **생략**한다. DHU 콘솔의 `help location`은 "Pass in NAN to skip optional
  parameters"라고 하므로 DHU의 `NAN`도 생략으로 보는 것이 자연스럽다(DHU wire는 TLS라 직접
  확인하지 못했다). 그러면 DHU 2·3차의 "정확도 없음" 시험이 순정 재송신과 같은 조건이다.
- 순정의 wire timestamp는 항상 0이다. 평소 GPS가 정상일 때(모드 1/2)도 같으므로, 평소 네이버가
  차량 위치를 정상 표시한다면 timestamp 0은 수용된다고 추정한다. 이 추정은 사용자의 평소 사용
  경험에 기대며, 이 기록은 폰의 timestamp 처리를 입증하지 않는다.

## 설계 함의

- 주입 좌표는 native 48바이트에서 `hasAccuracy=1`, `accuracy = N×1000`으로 쓴다. timestamp는
  native에서 바꿔도 wire에서 0이 되므로 바꿀 수 없고 바꿀 필요도 없다.
- **in-place 수정 대신 복사본을 쓴다.** 모드 1/2에서는 캐시 복사가 송신 뒤에 일어나므로 VDM+8을
  직접 고치면 모드 0 캐시로 샌다. wrapper가 우리 버퍼를 가리키게 한다.
- **정확도는 우리 DR 좌표에만 담는다.** 순정 모드 0 캐시 재송신에 정확도를 붙이면 낡은 fix가 유효로
  보이게 된다. 이는 안전과 설계의 결정이며 펌웨어가 정하지 않는다.
- 폰·Android Auto·네이버가 accuracy 0, 아주 작은 값, 반복된 동일 fix, 음수 속도를 어떻게 다루는지는
  오프라인으로 정할 수 없다. 실제 터널의 LDS 모드 순서와 HDOP 값도 마찬가지다.

이 기록은 설계 변경이나 live ASSIST 활성화를 승인하지 않는다.
