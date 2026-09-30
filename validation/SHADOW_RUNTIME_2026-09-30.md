# 원본 센서 경로부터 제품 MODEL 계산까지 — 2026-09-30

원본 VIM/VBS callback에서 실제 제품 AA worker까지 연속 합성 입력을 전달하여
정차 영점 적용, 직진·회전, yaw 단절과 새 GPS 기준점 후 복구를 실행했습니다.
비차단 센서 전송의 `EAGAIN`과 순번 누락을 발견해 수신 대기를 개선했습니다.
**최종 r6은 통과했지만 같은 최종 제품의 r5에서는 요청 관측 잠금 경합이
발생했습니다. 전체 통합 안정성 또는 v1.0 완료 판정이 아닙니다.**

기준 소스는 `2b959ff`입니다. 공개 ZIP은 기존 `v0.3.1-shadow.1`이며 이번
변경을 포함하지 않습니다. ASSIST·qualified provenance는 비활성입니다.

## 실행 범위

정확한 NA 74.00.324A 순정 커널·rootfs와 QEMU 7.2.22의 기존 CMU 구성을
사용했습니다. kernel-entry machine ID 조정 외 커널 바이트는 그대로이며,
네트워크·호스트 장치·공유 폴더·userspace debugger를 사용하지 않았습니다.

진단 PID1이 원본 VIM, settings, VBS, LDS, aap_service를 시작했습니다. 원본
VIM API로 합성 이벤트를 전달하고 기존 등록·VBS callback·제품 tap을 거쳤습니다.
새 VIM 구독자나 차량 제어 메시지는 없습니다. 외부 caller는 실제 제품 DSO를
preload하고 원본 BLM을 처음 로드해 자동 설치와 원본 queue/manager를 실행했습니다.
adapter를 caller에 복제하거나 서비스 응답을 성공 stub으로 바꾸지 않았습니다.

GPS 네 건은 명시적인 합성 원본 callback 호출이며 요청 관측은
`request_id=0/not_observed`입니다. 실제 LDS 응답은 계속 mode 0·UTC/좌표 0이며
별도로 관측했습니다. 원본 등록 성공·제품 SHADOW worker·원본 CAN ready-handler
로그 뒤에 시작했지만, handler 진입은 물리 CAN 준비 완료가 아닙니다.
standalone 구성의 CAN peer timeout·폰 없는 세션 한계는 남습니다.

## 전송 실패와 변경

기존 제품 r1에서는 yaw 순번 24·372·594가 누락됐고 회전 검사가 실패했습니다.
r2는 같은 제품으로 완료됐으므로 간헐적인 문제였습니다. r3에는 **우리 센서
datagram의 sendto 결과만** 기록하는 비공개 진단을 추가했습니다. 순번 237의
`sendto=-1, errno=11(EAGAIN)`과 AA의 동일 순번 누락을 확인했습니다. 순정
`max_dgram_qlen`은 10입니다. 실패 후 caller 종료에 따른 `ECONNREFUSED`는
그 앞선 누락 원인과 구분했습니다.

`MotionReceiver`에 bounded `poll` 대기를 추가했습니다. AA worker는 입력이
도착하면 수신하며, 대기 오류·비capture·로그 실패에서는 기존 50ms sleep으로
돌아갑니다. sender의 비차단·무재시도, 순번/신선도 검사, 원본 callback·errno
계약은 그대로입니다. 큐 크기나 커널 설정도 바꾸지 않았습니다.

독립 리뷰는 poll마다 MODEL 계산까지 수행하는 초기 변경의 회귀를 찾았습니다.
receipt 기반 yaw 150ms, wheel 10/20ms에서 unseeded watermark가 먼저 전진해
아직 닫히지 않은 yaw 창을 LATE로 거부했습니다. 단순 최소 50ms gate도 wheel
20ms에서는 계산 시각을 0·60·120ms로 밀었습니다.

최종 `WorkerTick`은 계산 deadline을 50ms로 유지하고 poll timeout을 잔여시간으로
줄입니다. 실제 Pipeline/GpsHoldout 회귀는 두 입력에서 기존과 같은 31개 계산
시각과 LATE/reset 0을 확인합니다. 잘못된 두 스케줄은 음성 대조로 실패합니다.
일반적인 100ms holdback 한계나 모든 센서 cadence를 해결했다는 주장은 아닙니다.

## 최종 r6 결과

r6은 sendto 진단 interposer를 제외했습니다. 최종 제품 ELF와 원본 경로로
caller 내부 검사 10,957회·정상 종료 0과 현재 boot의 durable capture를 확인했습니다.

| 항목 | 실제 기록 |
| --- | --- |
| 합성 VIM API → 제품 raw | 916건, 순번 연속, sensor별 원시값·수신 시각 대응 일치 |
| 정차 영점 | 2050 후보 학습, 적용 전 active 2047/version 0, 새 기준점 뒤 2050/version 1 |
| 직진·회전 | speed 10m/s MODEL 계산, 유효 snapshot 67개 |
| yaw 단절 | 계산·preview 중단. 센서만 재개해도 이전 기준점이 되살아나지 않음 |
| 새 기준점 | 새 GPS 쌍 이후 MODEL 계산 복구 |
| journal | 667행: position 14, send 96, motion batch 341, shadow 172 등 |
| 종료 | drop·audit fault·요청 관측 손실·남은 요청/worker 0, capture 종료 확인 |

48바이트 MODEL preview의 좌표 반올림, 속도와 합성 anchor에서 유도된 UTC도
대조했습니다. 독립 검증기는 기록된 frontier 시각과 일정 속도·각속도의
직선/원호식을 사용하고, 좌표 차이를 WGS84 국소 곡률로 미터에 환산했습니다.

| 비교 구간 | 시간 | 계산 이동 거리 | 계산 회전각 |
| --- | ---: | ---: | ---: |
| 직진 | 2.388038954s | 23.88038954m | 0rad |
| 회전 | 2.043659908s | 20.43659908m | 0.313614521rad |
| 재기준점 후 직진 | 1.801946774s | 18.01946774m | 0rad |

위치 식의 잔차 0.005m, heading 잔차 1e-7rad 이내였습니다. 동일 합성 모델의
계산 일치이며 **차량 위치 오차나 물리 정확도 수치가 아닙니다.** raw 한 건 삭제,
계산 좌표 변경, capture_end 삭제의 세 음성 입력은 모두 거부됐습니다.

이번 fixture는 GPS callback을 manager 시작 전에 공급하므로 native LOCATION
송신은 0건입니다. 다른 타입의 send 96건만 기록됐고 모두 원본 선택입니다.
LOCATION 바이트 보존이나 폰 수용의 추가 증거가 아닙니다. 이전 실제 위치
송신 검사는 [별도 AA pipeline 기록](AA_PIPELINE_2026-09-30.md)에 남아 있습니다.

일반 분석기는 의도한 yaw 단절/reset, 실제 GPS 단절에 따른 holdout abort,
LOCATION payload 부재로 exit 2/inconclusive입니다. 그 판정을 변경하지 않았습니다.
별도 시나리오 검증 통과를 완전한 차량 세션 판정으로 바꾸지 않습니다.

## 실패 보존과 남은 요청 관측 경합

| 실행 | 센서 전달/계산 | 최종 결과 |
| --- | --- | --- |
| r1 기존 제품 | raw 732, 순번 세 건 누락 | 회전 검사 실패 |
| r2 기존 제품 + send 진단 | raw 945, 계산·복구 완료 | 종료 0, 수치 검증 통과 |
| r3 r2와 같은 이미지 | raw 664, 순번 237 EAGAIN | 영점 적용 검사 실패 |
| r4 초기 poll 변경 | raw 685, 기록 구간 순번 누락 없음 | 요청 관측 손실 검사 실패 |
| r5 최종 deadline 제품 + send 진단 | raw 739, 기록 구간 순번 누락 없음 | 요청 관측 손실 검사 실패, 마지막 불완전 journal 행 1개 |
| r6 r5와 같은 제품, send 진단 제외 | raw 916, 계산·복구 완료 | 종료 0, 수치 검증 통과 |

r4/r5의 health에는 `loss_reasons=1(LOSS_CONTENTION)`, epoch 2가 남았습니다.
r5의 최초 실제 mode 0 위치는 요청 연결이 없고, 뒤의 요청은 새 epoch에서
관측됐습니다. 종료 때 requests/workers가 0이어도 이 손실은 해소된 것이 아닙니다.
후속 caller는 비동기 배출을 최대 5초 기다렸지만 손실 검사는 계속 실패했습니다.
대기시간 문제로 해결 처리하거나 실패 검사를 제거하지 않았습니다.

어느 Ledger 경계끼리 경합했는지는 아직 분리하지 못했습니다. 제품의 단일
trylock 정책은 그대로이며, 수신 개선은 요청 관측 무손실을 보장하지 않습니다.
다음 작업은 이 경합의 작성 코드 재현·원인 분리입니다. 동일 최종 ELF의
r6 성공으로 r5 실패를 닫지 않습니다.

## 회귀와 독립 리뷰

최종 `make test`는 Python 270개와 전체 C/C++ 검사를 생략 없이 통과했습니다.
고정 GCC 4.9.1의 전체 authored ARM 검사도 생략 없이 통과했습니다. 실제 DSO의
기존 예외/취소 8개와 요청 wrapper 13개가 포함됩니다. 첫 ARM 명령의 실행 비트
오류와 초기 host의 fixture 경로 오류에 따른 21개 skip은 보존했고, 명령·입력
경로를 바로잡아 위 최종 검사를 수행했습니다.

Codex 독립 리뷰 네 건은 공개 작성 코드만 검토했습니다. cadence 회귀와 고정
50ms sleep도 통과하던 wake 테스트 공백을 수정했고, 같은 sleep mutant는 이제
exit 134로 거부됩니다. 최종 리뷰에서 미해결 P1/P2는 보고되지 않았습니다.
이는 위 실제 요청 관측 경합이 해결됐다는 뜻이 아닙니다.

별도 작성 worker의 idle·연속 입력·비capture·로그 실패·poll 오류 다섯 검사는
종료 요청 후 약 819–848ms에 복귀했습니다. 이 worker 검사에서는 모델 계산이
비활성이며 cadence는 별도 Pipeline 검사입니다. 연속 입력 검사의 성공 343건·
send 오류 2건을 무손실 부하 증거로 쓰지 않습니다. 지속 poll 오류 때 약 1초
동안 poll 19회로 오류 spin이 없음을 확인했습니다.

원본 VM은 주 에이전트가 직접 실행했습니다. Claude의 추가 조사 커밋은 없었고,
마지막 관련 독립 자료는 `e5d87c1`의 [LDS async 기록](LDS_ASYNC_2026-09-30.md)입니다.
이번 단위의 새 Claude 실행·리뷰나 실차·폰 검증은 주장하지 않습니다.
원본 파일·전체 로그·비공개 fixture는 게시하지 않습니다.

## 입력 고정

| 항목 | SHA-256 |
| --- | --- |
| 순정 커널 | `57fe0cda7c1f994a6ca6b3cce7d9cf247f3b5607ebf1ddc12f02598662d5e240` |
| 원본 rootfs | `61bbcea608cc915f45a1775d4f49fb1603e4d92a0163311ced15220d03cf9d44` |
| 최종 libmx5dr.so | `e7c4e73e66a06ac4299437102d60f57eb82792c203132b5bdec9d496f65671c9` |
| 최종 vimtap | `7cea2454bf95334fecf922027684bc6c2034e42ad1710a0676b6abcbb056db3b` |
| 최종 외부 caller | `eeba1ceb467b7e06f6466a6c15ed753be1857e1b62bf14e4545dca95800807f3` |
| r6 initrd | `bc41e0c8712076160cad3de87cfd906b55468f20f658868fea12be8c192ebbf6` |
| r6 console | `db3393721ed6f4f1dad393e32cea90db270acfa44e566bbf110f378000fb2096` |
| r5 실패 console | `7e29a12ddd391843ee900088a50b59a5ea12ad59e19412e9914c7ff5cf986efe` |
| 완료 검증기 | `7f7335bdb78fff7d10ba60061abad1ecfe83defd5ecbc74605fa66ef27837e12` |

runner의 timeout/QEMU 종료값은 통과 판정이 아닙니다. 원본 API caller의 결과,
입력 해시, journal과 raw 대응이 근거입니다. 물리 센서 단위·시간·품질,
정상 전체 차량 기동·복구, 유효 GPS와 휴대폰 수용은 여전히 미검증입니다.
