# Android Auto 17.9.664004 정적 역공학: 차량 LOCATION 변환·전달·위성·구독 (독립 2건, 2026-10-12)

소유자가 `AGENTS.md`에 허용한 범위(Android Auto 앱만, Google Play 서비스는 범위 밖이라 분석하지 않음). Opus와 Fable이 독립 분석했고 결론이 일치합니다. 클래스·메서드 이름, 상수, 문자열만 적었고 APK·코드·덤프는 두지 않았습니다. 정적 분석이며 폰 실행은 없습니다.

**버전 주의**: 분석한 것은 17.9.664004(arm64 번들)인데 **폰에 설치된 것은 17.7.663654**(`DHU_NAVER_EXP12_RESULT_2026-10-12.md`)입니다. 아래 결론은 설치 버전에서 재검증이 필요합니다.

## 일치한 결과 (FACT)
- **와이어 LOCATION**: 필드 2 lat e7, 3 lon e7(둘 다 필수), 4 accuracy e3, 5 altitude **e2**, 6 speed e3, 7 bearing e6. **필드 1(timestamp)은 파서에 없고 무시**됩니다. 존재 비트 1/2/4/8/16/32 = lat/lon/acc/alt/speed/bearing.
- **변환**(`uej.R` → `CarSensorEvent(10, SystemClock.elapsedRealtimeNanos(), …)`): 정확도 ×0.001, 고도 ×0.01, 속도 ×0.001, 방위 ×1e-6. **이벤트 시각 = 폰이 메시지를 디코드한 시각**(반올림, 중복 제거, 기본값 없음). 존재 비트가 없으면 `setAccuracy/setSpeed/setBearing/setAltitude`를 호출하지 않고 `has*=false` 그대로 전달.
- **앱용 Location**(`kdb.d`, 로그 태그 `CarApp.H.Har`): 메시지마다 `new Location("Car-GPS")`, `setElapsedRealtimeNanos(디코드 시각)`, `setTime(currentTimeMillis − 경과)`(디코드 벽시계). 같은 내용이 연속으로 와도 걸러내지 않고 **매번 새 시각**. 정확도·속도·위성 필터 없음.
- **전달 경로는 Car App Library의 `CarHardwareLocation` 하나뿐**(조건: 폰 위치 모드 켜짐, 앱 `ACCESS_FINE_LOCATION`, 허용 목록 플래그 기본값 전체 허용, 차가 LOCATION 광고, 앱 FASTEST → 내부 rate 0). **폰 위치 스택(Fused Location) 주입 코드(`rbw`)는 있으나 리스너로 등록하는 호출이 없어 이 빌드에서 도달 불가**(INFERENCE, 높음). 주입 전처리(정확도 ≤ 1 또는 ≥ 500 → 5.67 m로 대체, 속도 ≥ 70 비활성, 1.8 s stale, 정지 시 7.89 m 재주입)도 같이 비활성.
- **차 대 폰 우선순위와 전환 타임아웃**: 앱 수준 선택 로직 없음. 커뮤니티의 "1~5 s 침묵 후 폰 전환"에 대응하는 코드는 gearhead에 없음(주입 경로의 stale 처리뿐이고 비활성).
- **GPS_SATELLITE(type 21)와 위치 특성값(256)**: 변환기는 있으나 **gms type 17을 등록(구독)하는 코드가 전무** → 폰은 SensorRequest를 보내지 않고, 요청 없이 들어온 이벤트는 "sensor event while no listener"로 폐기. 특성값은 저장과 덤프 문자열("locationCharacterization=")에만 쓰이고 비교·분기가 없음. DR/GYRO/ACCEL은 AA 자체가 요청하지 않고 Car App 앱이 구독할 때만 구독.
- **속도 교차검증 없음**: LOCATION 속도와 CAR_SPEED를 비교하는 코드는 이 빌드에서 찾지 못했습니다(aa-proxy-rs 주석의 "characterization을 낮추면 교차검증 회피" 주장은 근거 없음). 차 LOCATION의 속도는 주행/정차 판정에만 쓰임(임계 플래그 1.5/0.5 m/s, 5 s 만료).
- **구독**: 첫 리스너가 등록될 때만 센서 요청을 보내고 마지막 해제 때 −1. 접속 시 LOCATION rate 0, SPEED 3 등을 등록. 전달 단 throttle 플래그(SPEED, LOCATION, 토큰 버킷 rate/초, rate 0이면 없음)는 1~2 Hz에는 무영향.

## 함의
- **네이버가 받는 시각**: `getTime()`은 폰이 메시지를 디코드한 벽시계(ms), `elapsedRealtimeNanos`는 디코드 시각, 와이어 timestamp 0은 무관. 따라서 네이버의 "같은 수신 시각 → INVALID_TIME_DIFF"는 차가 같은 메시지를 보내서가 아니라 **네이버의 1 Hz 틱이 1 Hz 차 스트림의 지터로 같은 Location을 두 번 샘플링할 때** 생깁니다(INFERENCE).
- **beta.12 arm**: A20의 20 m는 그대로 `hasAccuracy=true, 20.0`으로 네이버에 도달(상·하한·대체 없음). 효과는 전적으로 네이버 판정에 달림. **S5는 안드로이드 오토 수준에서 효과 없음**(구독 없음) → 네이버 앱도 위성 수를 읽지 않으므로 사실상 CONTROL과 같음.
- **우리 와이어 필드**: accuracy/speed/bearing 존재 비트는 1:1로 `has*()`에 반영되고 없으면 네이버가 −1/무효로 봄. **순정 mode-0 재송신은 새 시각 + 정확도 없음으로 도착**해 네이버에서 `INVALID_ACCURACY`로 집계. 고도는 e2(cm)로 해석되므로 우리가 보내는 고도 스케일 확인 필요. 위성 0과 특성값 0x100은 AA 동작에 영향 없음.
- **헤드유닛 쪽 변경 후보**: (1) BETA 치환 중 정확도·속도·방위 항상 포함(이미 설계, 위험 없음), (2) 송신 2 Hz(≥ 1.5 Hz)는 틱마다 새 시각을 보장하는 유일한 수단이나 "같은 호출을 치환"하는 계약을 벗어나 추가 송신이 필요하고 CMU 부하·RMC 검증 미비, 별도 arm으로만, (3) S5 폐기 가능, (4) 위성·특성값 조작 계열은 AA 코드상 무의미.

## 미해결
`lmo.c().s()` 본문(접속 시 LOCATION 요청의 권한 조건), 소유자 폰이 레거시(`rbe`)와 신(`irz`) 스택 중 어느 쪽인지, 네이버가 CarSensors를 NORMAL/FASTEST 중 어느 rate로 등록하는지, Mazda가 요청 없이 type 21을 보내는지, fused provider 내부(Play 서비스, 범위 밖), **설치 버전(17.7.663654)과의 차이**.
