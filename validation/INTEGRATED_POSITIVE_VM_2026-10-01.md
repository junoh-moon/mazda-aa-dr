# 통합 SHADOW 후보의 원본 LDS·센서 양성 입력 VM — 2026-10-01

공개 v0.3.8 이후의 로컬 통합 후보를 원본 NA 74.00.324A userspace의
LDS→AA 위치 요청·응답과 VIM→VBS 센서 경로에 연결했습니다. 격리된 QEMU
보드에 작성한 NMEA와 휠·yaw·후진 입력을 넣었으며, 제품 위치 callback에
합성 기준점을 직접 넣지 않았습니다. **정차 후보 적용, 이동 GPS 기준점,
약 5초의 mode 0 구간에서 MODEL 계산, 이동 GPS 복귀 뒤 철회**를 한 실행에서
확인했습니다. 이는 합성 입력의 계산 경로 검증이며 차량 정확도나 live ASSIST
검증이 아닙니다. 공개 `v0.3.8-shadow.1` ZIP도 변경하지 않았습니다.

## 실행과 입력 식별

시험 제품은 `d3c6fa1e81fa16046ebbd1f200c22ffb3c3eda6a`에서 만든
비공개 통합 SHADOW ZIP입니다. ZIP SHA-256은
`4560926ace8b60187db448d9c5f8667fd5ad25b5f5896c69dc3ee43621d4ec31`,
`libmx5dr.so`는
`4ef64da1fae9c18c975833495c69fda9a9b41ceaf0c3d78e07fc9eeeb911b441`입니다.
압축 파일의 36개 항목을 VM bundle manifest와 각각 대조했습니다.
원본 rootfs와 BLM·VIM API·AA 서비스 파일의 해시는 비공개 이미지
sidecar에서 고정하고 이미지 안의 바이트와 재대조했습니다. 커널 해시는
VM runner 기록에 따로 남겼습니다. 고정 ARM
도구체인으로 만든 별도 진단 호출기는 제품 adapter를 링크하지 않았습니다.
r7 진단 initramfs SHA-256은
`df45790fbf9e8c362569fafef8309fb249d784a71ceb9a5740535a492bdc1877`입니다.
실행 후 합성 NMEA 공급기 소스
`df62742cf8864486078ee084bab5605205bcc91384ee86af237107e2cd42703a`를
같은 고정 컴파일러로 재빌드해 시험 이미지에 기록된 공급기 바이너리 해시
`beff64e9ad20197ebb58869a826f79530af622f7a3234ec666c5ab1d340921ce`와
일치함을 확인했습니다. 이는 공급기 바이트 출처의 사후 대조이지 VM runner가
별도 이미지의 빌드 출처를 자동 인증했다는 뜻은 아닙니다.

진단용 PID 1이 원본 LDS·VBS와 관련 의존 서비스를 포함한 10개 서비스의
부분 SM 그래프, 원본 VIM, AA 서비스를 시작했습니다. 진단용 GPIO 되읽기
모델은 QEMU의 미구현 수신기 선택 경계에만 적용했습니다. NMEA는 PTY로
원본 LDS에, 센서는 원본 VIM API에 넣었습니다. 원본 AA manager가 LDS에
요청하고 원본 callback·위치 worker·송신 경로를 수행했습니다. 물리 GPS/CAN,
전체 순정 init, 차량 장치, 호스트 네트워크, 폰은 없었습니다. initramfs,
원본 console과 상세 trace는 배포하지 않는 `evidence/integrated-positive-20261001/`
안에 보관합니다. VM runner는 이 별도 initramfs를 `external image`로 표시하므로
runner의 출처 필드는 비어 있습니다. 이미지 sidecar·실제 아카이브·runner의
SHA-256을 따로 연결했으며 QEMU 종료 코드만으로 시험을 통과 처리하지 않았습니다.

초기 입력 없는 실행과 중간 진단 실패를 성공으로 합치지 않았습니다.
정차 NMEA만 준 r4에서는 원본 mode 1 응답이 12건 있었지만 속도 0으로
`BAD_FIX`였고 보정은 적용되지 않았습니다. 이동 입력 r5b는 보정을 적용했으나
가상 VIM의 합성 callback 호출이 응답 없이 멈춘 뒤 센서 입력이 끊겼습니다.
따라서 그 실행의 mode 0에서 `MISSING_SENSOR`가 발생했고 MODEL 성공으로
세지 않았습니다. r6은 짧은 무효 구간의 경로 점검이며, 아래 수치는 무효
구간과 거리 검사를 늘려 다시 실행한 **r7 단일 실행**의 결과입니다.

## r7에서 확인한 경로

VM 안의 `sh install.sh`와 일회성 guard 선택은 성공했습니다. 제품 `boot`는
`install=ok`, `shadow_boot`는 MODEL 수집 활성, `assist_ready=false`를
기록했습니다. 원본 LDS는 수신 시작 상태에 도달했고 작성한 정차→북진→
no-fix→북진 복구 NMEA를 해석했습니다. 제품 trace의 위치 응답 43건은
mode 0이 28건, mode 1이 15건입니다. 첫 위치는 bus 관측 경계보다 먼저
요청되어 MODEL에서 `request_before_bus_boundary`로 거부됐습니다. 이를
유효 계산 입력으로 세지 않았습니다.

| 구간 | 원본·제품 기록 | 판단 |
| --- | --- | --- |
| 정차 후 북진 | 정차 yaw 후보 2050 생성, 북진 원본 mode 1·약 35 km/h 수신, 새 기준점에서 `calibration_version=1`·`active_zero=2050` | 후보가 이동 중 임의 적용된 것이 아니라 새 GPS 기준점에 적용됨 |
| 약 5초 no-fix | 연속된 원본 mode 0 응답 5건, MODEL 유효 snapshot 47건, frontier 4.844초 전진 | GPS 좌표 36°N/136°E를 예측 위치로 복사하지 않고 휠·yaw로 북진 계산 |
| 복귀 | 원본 mode 1이 다시 도착하고 다음 응답에 이동 속도 약 35 km/h가 실림, 그 뒤 MODEL 유효 출력 철회 | 이 작성한 복귀 경계에서 계산이 남지 않음 |

위 유효 구간의 모든 snapshot은 동일한 MODEL session epoch 2·session
revision 1·bus revision 5, `calibration_version=1`, `resets=22`,
`assist_ready=false`를 유지했습니다.
22회 `MISSING_SENSOR` reset은 합성 센서를 공급하기 **전**에 발생했고
유효 구간 안에서 증가하지 않았습니다. 첫·끝 유효 frontier 사이의 제품
기록에는 휠·yaw·후진 각 91건의 연속 수신 입력이 있으며 최대 수신
간격은 각각 약 78·103·102 ms였습니다.
이 시각은 callback 수신 시각이지 물리 측정 시각이 아닙니다.

진단 호출기는 no-fix 중 4.051초에 북쪽 40.646 m, 동쪽 0 m와
MODEL 유효 지속을 검사했습니다. 별도로 전체 유효 frontier의 처음·끝
위도를 WGS84 자오선 곡률로 독립 계산하여 4.844초에 48.440 m,
약 10.000 m/s를 얻었습니다. 이는 작성한 raw 휠 값 13600의
`(13600×0.01−100)/3.6 = 10 m/s`와 수치적으로 일치합니다.
유효 GPS fix가 없는 동안 나온 합성 입력의 산술 일치이며 차량 위치 오차
수치가 아닙니다. 유효 snapshot 47건 모두 위치 preview를 만들었지만
ASSIST 송신으로 승격되지 않았습니다.

기록된 48바이트 LOCATION 입력·출력 쌍 20건은 모두 바이트가 같고
하위 호출 반환값 0이었습니다. 이는 제품 journal의 원본 전달 관측이며
폰 수신이나 별도 하위 송신 횟수 계측은 아닙니다. 완료 표식은 비공개
console의 `INTEGRATED_CALLER_RC=0`과 `SHADOW_RESULT=PASS`를 함께 확인했습니다.
VM runner 자체의 판정은 `observation_only`입니다.

`tools/analyze_logs.py --json`의 전체 판정은 종료 2, `inconclusive`입니다.
초기 세션·버스 미관측과 센서 공급 전 reset을 지우지 않았고, holdout은
실제 mode 0에서 중단됐습니다. 작성 GPS와 비교한 0.278 m 한 건은
완료된 holdout 창이 아니며 참값 정확도도 아닙니다. collector 생명주기,
전체 순정 재부팅과 다음 부팅 복귀, 같은 실행 SM 재시도, 실제 센서 주기·
생산 시각·품질, 폰/지도 앱 수용은 이 실행으로 확인하지 않았습니다.
live ASSIST는 계속 비활성입니다.

동일한 소스에서 Linux 컨테이너의 원본 identity fixture와 통합 번들을
지정해 `make test`를 clean build로 실행했습니다. C/C++ 검사와 Python
400건이 모두 통과했고 생략은 0건입니다. macOS에서 직접 실행한 첫 시도는
Linux 전용 헤더 때문에 컴파일되지 않았으며, 앞선 컨테이너 실행에서는
fixture 경로 누락으로 패키징 검사가 생략돼 이 조건으로 다시 검사했습니다.
호스트 회귀 통과를 OEM·차량 검증으로 세지 않습니다.

검토 과정에서 Claude Code에 진단 fixture의 인과관계·시간 조건을
읽기 전용으로 비판 검토시켰습니다. 짧은 no-fix와 빈약한 거리 단언을
지적받아 r7에서 지속·거리·이동 복귀 조건을 보강했습니다. Codex 독립
검토도 trace의 47개 연속 유효 행과 5개 원본 mode 0 응답을 재대조했습니다.
이 검토들은 실제 차량 실행이나 폰 시험을 대신하지 않습니다.

기존 [입력 없는 통합 VM 기록](INTEGRATED_VM_2026-10-01.md)은 별도 이전
실행으로 보존합니다. 이후 단계와 차량 시험 범위는
[현재 상태](../docs/STATUS_KO.md), [통합 현장 계획](../docs/FIELD_TRIAL_KO.md),
[v1.0 완료 조건](../docs/V1_READINESS_KO.md)을 따르십시오.
