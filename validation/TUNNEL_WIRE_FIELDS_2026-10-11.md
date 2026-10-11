# 터널 중 폰이 받는 LOCATION 필드 조사 (G1, 2026-10-11)

두 조사자(Opus, Codex)가 독립으로 같은 질문을 조사했다: 실차 터널에서 BETA 치환 중 폰이 받는 메시지가 평소와 정확히 무엇이 다른가. OEM 바이너리, 역어셈블, 지도는 복사하지 않았고 함수 이름과 오프셋만 적는다. 표시: FIRMWARE FACT, LOG FACT, PUBLIC DOC, INFERENCE.

## 관찰 (소유자 기억, 첫 beta.10 주행)
터널 1(111초): 회색 "GPS 안 잡힘" + 속도 고정. 터널 2(60초): 회색, 속도는 반영. 터널 3(14초)과 일부 구간: 정상(검정)으로 잘 움직임. 소유자 기억이며 로그에는 폰 화면이 없다.

## 일치한 사실
- FIRMWARE FACT: 인코더 경로는 `aap_service!AAPSensorSource::send_sensor_data@0x4DB30` → `send_location_data@0x50274` → `SensorSource::reportLocationData@0x17F6A4` → `populateLocationData@0x17F5F0`. 48바이트 입력의 바이트 0~7(UTC ns)은 **wire에 쓰이지 않고** timestamp는 정상 GPS와 터널 모두 **항상 명시적 0**이다. 바이트 8~15는 위도·경도(e7), 16/20은 accuracy(e3), 24/28 고도, 32/36 속도(e3), **40/44 방위(e6)**이며 "moving 플래그"로 불렀던 바이트 40은 **has_bearing**이다. LOCATION에는 mode, fix 유형, elapsedRealtime 필드가 없다. 공개 aasdk GPSLocationData.proto와 번호·형식이 일치한다.
- FIRMWARE FACT: `mode`는 LOCATION 필드가 아니다. `blmjciaapa!RequestSendPosition@0xC7460`에서 mode 0은 이전 fix를 복사하고 accuracy를 지우며(캐시가 없으면 송신 안 함), mode 1/2는 새 위치와 accuracy=2xHDOP를 보내고 캐시를 갱신한다. 별도로 `MakeGpsSatellite@0xC8190` → `fillGpsSatellites@0x181F90`가 GPS_SATELLITE(type 21)의 `number_in_use`를 mode 1이면 5, mode 2이면 3, mode 0/3이면 0으로 정한다(실측 위성 수가 아님). 실제 송신 여부(구독 조건)는 미확정.
- LOG FACT: 터널 구간의 BETA 치환 송신은 매초 위치·속도·방위를 갱신하고(속도 3.9~17 m/s로 변함), 정확도는 30에서 40 m 상한으로 오르며 111/60/14초 중 약 105/56/6초를 40 m 상한에 머문다. 각 구간의 첫 원본 송신은 accuracy 없이 나간다. payload의 UTC 필드는 터널 내내 얼어 있지만 wire에 가지 않는다.

## 결론
- 시각 정체 가설(고정된 시각이 폰에서 오래돼 회색이 됨)은 **배제**한다: wire timestamp는 평소에도 0이다. 시각을 전진시키는 beta는 만들지 않는다.
- 남은 후보(INFERENCE): 위성 수 0(회색 표시는 설명하나 터널 1/2의 차이는 못 설명), 40 m 상한 유지 시간(105/56/6초가 관찰 순서와 같음, DHU는 40 m를 30초까지만 시험), 횡방향 오차 증가(119/52/25 m가 관찰 구분과 가장 잘 맞음), 구간 첫 무정확도 메시지가 상태 전환을 촉발할 가능성(세 터널 공통이라 길이 의존성은 못 설명), 마지막 수용 위치의 나이. **터널 1만 20 km/h 미만 구간이 있어 길이와 속도가 혼동된다.** "속도 고정"은 네이버가 우리 fix를 버리고 마지막 속도로 외삽한 모양이다(DHU 실험 3 C1/C5와 같은 패턴).
- 네이버의 표시·동결 임계값은 공개 근거로 확인되지 않았다.

## 다음
`validation/DHU_NAVER_EXP11_PLAN_2026-10-11/`: DHU에서 같은 속도 곡선으로 위성 0/5, 40 m 유지 대 20 m, 횡오프셋 0 대 120 m, 실차 조합 재현(양성 대조군)을 가른다. DHU는 timestamp와 mode를 제어하지 못하지만 wire timestamp가 상수 0이므로 필요하지 않다. 위성 값은 DHU 공식 `gps_satellite` 명령으로 보낸다. DHU에서 재현되지 않으면 차량 로깅 빌드로 간다.
