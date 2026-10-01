# 최신 통합 제품 DSO의 원본 LDS·센서 VM 재실행 — 2026-10-01

[이전 양성 입력 VM](INTEGRATED_POSITIVE_VM_2026-10-01.md)은
`d3c6fa1`의 제품 DSO를 사용했습니다. 후속 ASSIST 큐 폐기 경계 수정과
LDS 출처 코어를 합친 `c62b313` 소스에서 제품을 다시 빌드하고, 같은
원본 NA 74.00.324A LDS→AA·VIM→VBS 입력 경계를 r8·r9 두 번 실행했습니다.
두 실행 모두 합성 no-fix 동안 MODEL 위치 계산과 이동 GPS 복귀 뒤 철회를
관측했습니다. **두 VM runner는 guest 종료 표식 뒤에도 제한 시간에 걸렸으므로
전체 실행 PASS나 차량 검증으로 판정하지 않습니다.** live ASSIST는 비활성입니다.

## 정확한 제품과 격리 조건

고정 GCC 4.9.1 도구체인(`61ec0343`)으로 다섯 ARM 파일을 새 디렉터리에
빌드했습니다. 제품 `libmx5dr.so` SHA-256은
`fda002045a8bdf474012502a973729e4eb15200d1611ff828f369b4f3519f7ed`로,
[ASSIST 큐 수정의 독립 ARM 제품 검증](ASSIST_QUEUE_CUTOFF_2026-10-01.md)과
일치합니다. VIM tap은
`9fff417181991d692360af542a540da31cd0b8e5c719660306dfd67f22fafa4c`입니다.
비공개 SHADOW 설치 ZIP SHA-256은
`97e6c534646a430d34ec132dd3086f5476867bb2b88971d1a6048d1f27b4adc8`입니다.
공개 v0.3.8 ZIP을 바꾸거나 이 후보를 발행하지 않았습니다. 제품에 연결되지
않은 새 LDS 필드 출처 코어를 이 DSO의 동작으로 세지 않습니다.

진단 호출기는 새 DSO의 실제 ELF에서 `request_hook_health`와 boot 결과
심볼의 위치를 다시 얻고 제품 해시를 검사했습니다. 진단 호출기는 제품
adapter를 링크하지 않았습니다. 원본 rootfs·커널, 원본 LDS·VIM·VBS·AA,
10개 서비스의 부분 SM, 진단용 GPIO 되읽기 모델, 합성 NMEA PTY와
휠·yaw·후진 입력을 사용했습니다. 원본 kernel SHA-256은
`57fe0cda7c1f994a6ca6b3cce7d9cf247f3b5607ebf1ddc12f02598662d5e240`,
공통 r8/r9 initramfs는
`6a6230643dfeee9c11a2d6f6ab57fc64693f82daee00668276646c1f975b89ce`입니다.
비공개 이미지 sidecar와 runner 입력 해시를 대조했습니다. 물리 GPS/CAN,
전체 순정 init, 호스트 네트워크·차량 장치 통과, 폰은 없었습니다.
순정 `aap_service`는 별도 프로세스로 기동했고, 제품 DSO는 진단 호출기에
preload하여 그 프로세스에서 원본 BLM manager 코드를 적재·호출했습니다.
따라서 순정 AA 프로세스에 제품이 cold preload된 수명 검증은 아닙니다.

## 두 실행의 실제 관측

두 VM 모두 `sh install.sh`·일회성 guard 선택 뒤 `boot install=ok`,
`shadow_boot active=true`, `assist_ready=false`를 기록했습니다. 원본 LDS는
수신 시작과 mode 1→0→1 응답에 도달했습니다. 각 guest의 합성 공급기와
진단 호출기는 종료값 0이고 `SHADOW_RESULT=PASS`를 남겼습니다. 이 표식은
아래의 **제한된 계산 조건**만 뜻합니다.

| 관측 | r8 | r9 |
| --- | ---: | ---: |
| 원본 위치 응답 | 47건: mode 0 30, mode 1 17 | 41건: mode 0 28, mode 1 13 |
| 연속 no-fix 응답 | 6건, 첫·끝 간격 5.037초 | 5건, 첫·끝 간격 4.005초 |
| 연속 MODEL 유효 snapshot | 55건 | 47건 |
| 유효 frontier 전진 | 5.812초 | 4.795초 |
| 별도 WGS84 자오선 계산 | 북쪽 58.115m, 동쪽 변화 0 | 북쪽 47.946m, 동쪽 변화 0 |
| frontier 사이 휠·yaw·후진 수신 | 각각 108건 | 각각 92건 |
| 기록된 48바이트 LOCATION 동일 쌍 | 23건 | 18건 |

거리/시간은 각각 약 10.000m/s로, 작성한 raw 휠 값 13600의
`(13600×0.01−100)/3.6 = 10m/s`와 수치적으로 일치했습니다. MODEL 유효
snapshot은 각 실행에서 중간 무효 행 없이 연속됐고, 보정 version 1,
같은 MODEL session·bus revision, `assist_ready=false`를 유지했습니다.
유효 구간 안의 `MISSING_SENSOR` reset 증가는 0건입니다. 센서 수신
최대 간격은 r8 약 101ms, r9 약 67ms였으며 이는 callback **수신** 시각입니다.
유효 GPS fix가 없는 동안에도 no-fix NMEA 문장은 계속 공급됐습니다.
이 수치는 작성한 VM 입력의 산술 일치이지 물리 차량의 위치 정확도가 아닙니다.

전체 journal 분석은 r8·r9 모두 종료 2·`inconclusive`입니다. 첫 위치의
bus 경계 이전 요청은 거부됐고, 입력 전 `MISSING_SENSOR` reset을 지우지
않았습니다. 두 실행 모두 완료된 GPS holdout 창은 0건입니다. r8의 비교
한 건도 뒤에 중단된 창에 속하며 참값 정확도가 아닙니다. 송신 선택은
r8 524건, r9 455건 모두 `ORIGINAL`이고 하위 호출 반환값은 0으로
기록됐습니다. LOCATION 동일성은 제품 journal의 입력·출력 쌍에 한정하며
독립 폰 수신 계측은 아닙니다. collector 생명주기·전체 순정 재부팅·
같은 실행 SM 재시도·실제 생산 시각과 센서 품질도 이 실행으로 자격화하지
않았습니다.

r8 runner는 300초, r9는 360초에 각각 **종료 124·`timed_out=true`**였습니다.
두 guest console에는 `LDS_PATH_DONE`과 `System halted`가 있지만 QEMU가
제한 내 종료되지 않았습니다. runner JSON의 `exit_code=0`은 시간
제한 처리 뒤의 QEMU 자식 종료 상태이므로 완료 증거로 사용하지 않습니다.
이 필드만으로 자발적 종료와 제한 뒤 종료 신호의 처리를 구분할 수 없습니다.
별도 추출한 제품 JSONL은 r8 1,584행·r9 1,417행으로 유효 JSON이고
끝 표식이 있지만,
runner의 `observation_only`와 위 전체 분석 판정을 덮지 않습니다.

동일 소스의 별도 Linux 빌드 디렉터리에서 `make test`는 종료 0,
Python 묶음 400건과 단독 재생 1건·C/C++ 검사 통과, 생략 0건입니다.
새 ZIP을 원본 identity fixture로 설치·제거하는 패키징 검사 20건도
별도로 통과했습니다. macOS 공유 볼륨의 도구체인은 파일명 대소문자
충돌로 해시 검사에서 먼저 멈췄고, 위 ARM 산출물은 컨테이너 내부의
검증된 도구체인으로 만들었습니다.

비공개 이미지·원본 console·상세 trace는 무시된
`evidence/integrated-positive-20261001/`에 보관합니다. 이 기록은 최신
DSO에서 이전 합성 MODEL 경로가 다시 동작한다는 근거입니다. 제품 LDS
출처 코어의 실제 cache mutex·수명·프로세스 간 요청 연결과 물리 센서
자격, live ASSIST 송신·폰/지도 앱 수용은 여전히 남았습니다.
[한 번의 현장 시험 계획](../docs/FIELD_TRIAL_KO.md)은 현재 공개 v0.3.8
ZIP에 고정되어 있습니다. 이 비공개 후보를 현장 파일로 쓰려면 고정 제품과
설치·회수 절차의 해시 및 문서를 함께 다시 맞춰야 합니다.
