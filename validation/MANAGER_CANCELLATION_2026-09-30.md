# 전체 manager 이후 요청 취소·재개 비교 — 2026-09-30

[제품 연결 검사](REQUEST_PRODUCT_2026-09-30.md)의 r3에서는 전체 manager 실행
뒤 첫 연결을 정리하고 제공자를 재개한 다음 AA용 요청이 fixture deadline 안에
완료되지 않았습니다. 그 실패를 축소 비교 성공으로 닫지 않고 직접 후속 실행했습니다.

**후속 전체 실행과 반복 비교는 통과했지만, 이전 timeout의 원인은 아직
확정하지 못했습니다.** 제품 소스·ELF를 변경해 해결한 기록이 아닙니다.
모든 실행은 원본 NA 74.00.324A 커널·rootfs의 격리 VM이며 실차·폰 검증이 아닙니다.

## 이전 실패에서 구분한 경계

기존 r3 D-Bus monitor에는 새 연결의 `GetSelectedGPS_sync`,
`GetDRUnitStatus_sync`에 대응하는 응답이 없고, 비동기 `GetPosition`에는
실제 응답이 기록돼 있습니다. 뒤이은 제어 조회에도 응답이 기록되지 않았습니다.
따라서 해당 로그를 LDS 전체 정지나 제품 Observer deadlock의 증명으로
해석하지 않습니다. AA util의 동기 단계와 data reply를 구분해야 합니다.
90초는 전체 fixture의 제한 시간이며 개별 요청에 설정한 timeout이 아닙니다.

## r1 — 전체 제품 파이프라인과 네 종료 경계

기존 외부 caller에 각 요청 API의 진입·복귀 marker만 추가했습니다.
제품의 자동 cold-install과 journal, 원본 manager의 자동 요청·큐·위치·send,
명시한 합성 seed와 SCRUB, manager 정지와 추가 task 소멸까지 실행한 뒤
다음 네 경우를 모두 수행했습니다. 최종 시험에서 취소 단계를 제외하지 않았습니다.

| 경우 | 실제 결과 |
| --- | --- |
| AA util 요청 네 개 → 제공자 정지 → connection free | pending 4 유지, free 뒤 0, 위치 callback 없음 |
| 새 연결의 AA util 요청 네 개 → 정지 → disconnect/free | 요청 제출 모두 복귀, disconnect에서 pending 0, 위치 callback 없음 |
| 제공자 정지 중 data 요청 네 개 → dispatch → free | pending 4 유지, free 뒤 0, 위치 callback 없음 |
| 다시 만든 연결의 data 요청 네 개 → dispatch → 제공자 재개 | 해당 요청의 위치 네 개만 완료, pending/worker 0 |

fixture는 검사 11,211회 후 종료 0이었습니다. 제품 journal은 136개 기록으로,
실제 요청과 연계된 mode 0 위치 18개, 명시적 합성 위치 1개, send 97개를
포함합니다. SCRUB 7개는 지정한 바이트만 바꿨고 나머지 원본 전달은 동일했습니다.
실제 LDS의 UTC·좌표는 모두 0이며 유효한 GPS가 아닙니다.

별도 검증기로 원본/제품/외부 caller 해시, 16개 제출의 진입·정상 복귀,
네 단계의 실제 pending/free, 위치/send Trace와 receipt 시각 순서를 대조했습니다.
마지막 durable capture와 health에는 drop·audit fault·관측 손실·남은 요청/worker가
모두 0입니다. 반환 marker 하나를 제거한 검사 입력은 별도 mutation에서 거부됐습니다.
OBSERVE 설정 뒤 진단용 API로 SCRUB을 선택한 범위는 앞선 검사와 같습니다.
이 진단을 실제 설정에 의한 SCRUB 기동으로 주장하지 않습니다.

## r2 — manager 이후 후크 유무의 반복 비교

같은 외부 caller를 별도 두 프로세스로 실행했습니다. baseline은 제품 DSO가
로드되지 않았음을 NOLOAD 조회로 확인했습니다. 제품 사례는 실제 startup
preload와 자동 설치 성공을 확인했습니다. 각 경우에 LDS와 aap_service를
새로 시작했습니다. 진단 프로그램은 원본 서비스의 응답을 그대로 검사했습니다.

두 경우 모두 원본 AA manager를 초기화·시작하고 자동 요청을 실행한 뒤,
명시적 합성 위치, 원본 요청 burst, manager 정지·추가 task 소멸·큐 배출을
수행했습니다. 이어서 서로 다른 이름의 연결을 20번 만들었습니다. 각 연결에서
AA util 요청 네 개를 제출하고 LDS 정지 상태에서 300ms 대기했습니다.
해당 연결을 dispatch하지 않은 채 free한 뒤 제공자를 재개했습니다.
manager 연결의 dispatch는 유지했습니다.

| 항목 | 후크 없음 | 실제 제품 후크 |
| --- | ---: | ---: |
| AA util 요청의 정상 반환 | 80 | 80 |
| connection free·제공자 재개 | 20회 | 20회 |
| 취소한 요청의 작성 callback | 0 | 0 |
| 관측 슬롯의 pending 4 → free 0 | 관측 후크 없음 | 매 회 확인 |
| manager·세션·queue 종료 API 및 caller | API 모두 복귀·종료 0 | API 모두 복귀·종료 0 |

시험 전후마다 별도 원본 dbus-send로 두 제어 조회와 위치 조회를 호출했습니다.
두 사례의 총 12개 조회가 모두 응답하고 종료 0이었습니다. 제공자 PID 생존이나
제출 status만으로 응답을 추정하지 않았습니다. 세션 종료의 기존 OEM mutex
오류는 별도 미해결 범위이며 정상 폰 세션 종료를 검증한 결과로 세지 않습니다.

이 반복 비교는 r1과 완전히 같은 실행이 아닙니다. 공통 시간 구간으로 manager를
실행했으며, 취소용 callback은 직접 작성한 함수이고 해당 연결은 dispatch하지
않았습니다. baseline에 없는 journal 검사를 성공 stub으로 대체하지 않았습니다.
실제 BLM callback의 지연 응답과 native SCRUB은 위 r1에서 별도로 검사했습니다.

## 재현 자료와 남은 범위

제품 DSO는 커밋 `0be5a526ba17fdbe493f663f281f269b4331c03f`의 구현이며 SHA-256은
`42568553215bea8ef9998add0ba6e0f208c438455cfb576cfae4e808d883331b`입니다.
앞선 전체 host/ARM 검사와 byte-identical하므로 제품 회귀를 중복 실행하지 않았습니다.
이번 추가 검사는 고정 GCC 4.9.1의 두 외부 caller와 원본 VM 실행입니다.

| 산출물 | r1 | r2 |
| --- | --- | --- |
| 외부 caller SHA-256 | `df41b7e9a939127e01a8cb74e6d240a3a711619d854fc5f9a046cb6c83ffbc47` | `0ab75e6405f5afad22c792ad711b6e3dd86296030f8777c8d5161e3d28a1494b` |
| initrd SHA-256 | `819c645a0939a5d7c9556556e97c8d636826d376ab974fb51e4f51ec17457385` | `96469f2966ea8fadb935c1099e152f8829bf13fdcda31f7ce3a5eed46ad2f122` |
| console SHA-256 | `e501c04d14a5787a2d0aabef63bc9f4e2e8dc9d2bb5dd98c4561ed4d326d95ec` | `a0d0ff4266da96a6422ed8d19de0fc726b37023242dfc2facac15e30aa259b13` |

r2도 별도 검증기로 해시, hook 유무, 모든 제출·free·재개, API 조회와 종료를
대조했습니다. 마지막 free marker 하나를 제거한 입력은 제품 사례만 실패로
판정했고, 원본 증거는 변경하지 않았습니다. r1/r2 runner는 guest halt 뒤
각각 140.016/340.019초 제한으로 종료 124, QEMU는 종료 0이었습니다.
이 종료값이나 제한 시간 도달을 통과 근거로 사용하지 않았습니다.
비공개 자료는 `evidence/manager-cancel-20260930/`에 보존하며 OEM 파일·전체
console·주소 기록을 게시하지 않습니다. 원본 커널·펌웨어 파일은 변경하지
않았으며 제품은 기존과 동일한 후크를 설치합니다. 호스트 네트워크·장치 전달·
공유 디렉터리는 없습니다.

원본 API 실행·검증은 주 에이전트가 직접 수행했습니다. 이번 조사에 새
하위 에이전트나 Claude 실행은 없었습니다. 공유 저장소를 다시 확인했으며
Claude의 마지막 관련 자료는 `e5d87c1`의 [LDS async 기록](LDS_ASYNC_2026-09-30.md)입니다.

한 번 실패했던 조건이 후속 실행에서 통과한 것은 원인 해결의 증거가 아닙니다.
이전 r3 실패와 후속 성공을 모두 유지합니다. 같은 이름 재연결, 실제 timeout
만료, 모든 OEM userdata 누수 여부, 정상 전체 차량 기동·물리 센서·폰 수용은
이번 검사로 해결하지 않았습니다. live ASSIST는 계속 비활성이며 공개 ZIP은
`v0.3.1-shadow.1` 그대로입니다. v1.0 완료를 선언하지 않습니다.
