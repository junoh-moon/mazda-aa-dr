# LDS 제품 출처 관측 → 실제 AA 기록 실행

고정 소스 `50ba484133b0e52739e5af35f7dcc993d5a3f850`의 실제 제품 DSO 두 개로
원본 LDS 부분 갱신부터 AA 원시 위치·출처 기록까지 연결했습니다. 전용 관측기를
소스 링크하여 대신 실행한 결과가 아닙니다. 작성한 초기화·입력·worker 호출 경계는
아래에 별도로 명시합니다. **물리 입력 자격과 live ASSIST는 여전히 미구현·비활성입니다.**

## 제품과 범위

| 산출물 | SHA-256 |
|---|---|
| `libmx5dr-ldstap.so` | `29a2cbeb7a9f29eb83a9e6ac05a91ec3d0b73fad1dea579e016db5710b79b73f` |
| `libmx5dr.so` | `23d8265a300aff67a0123d39905f56e61fabf1a85d2c277f533d3c163382b8cf` |

대상은 NA 74.00.324A 원본 라이브러리·동적 로더·libc입니다. 고정 GCC 4.9.1로
작성 caller를 빌드하고 QEMU user mode에서 실행했습니다. 시스템 호출은 호스트
커널을 사용하므로 실제 CMU 커널·장치·SM 전체 기동 시험이 아닙니다. 제품 DSO는
수정하지 않았고 caller에는 식별 확인용 SHA 구현만 링크했습니다.

[Cold 설치기 검증](LDS_COLD_INSTALL_2026-10-02.md)은 정확한 등록 ID·슬롯과
실패 전달을 다룹니다. 이 기록은 그 제품이 실제 원본 입력·응답과 AA 기록까지
도달하는 후속 실행입니다. 공개 `v0.3.9-shadow.3`의 실차 검증으로 소급하지 않습니다.

## 실제로 실행한 연결

1. 실제 LDS preload의 `dlopen` 경로로 원본 서비스 라이브러리를 열었습니다.
   서비스 노출 전 제품의 데이터 슬롯 25개가 모두 설치됐는지 대조했습니다.
   원본 cache 초기화와 callback 등록은 수정된 서비스 import를 통과했습니다.
2. 작성한 NMEA 문장 7개를 원본 parser에 주고, 원본 driver 등록 테이블에 남은
   실제 제품 callback을 호출했습니다. 원본 RMC/GSA/GGA handler가 원본 mutex와
   읽기 사본을 통해 cache를 갱신했습니다. 수동 `origin_begin/end`는 사용하지 않았습니다.
3. 원본 data interface가 별도 프로세스의 원본 data client 요청 9개에 응답했습니다.
   원본 service snapshot·reply 생성·raw send 뒤 실제 LDS 제품 Sender가 출처를
   전송했습니다. 요청 serial과 응답자 serial은 서로 다르게 만들고 대조했습니다.
4. AA 쪽은 작성 cold lease에서 실제 제품 installer를 직접 호출하여 원본 슬롯 21개와
   코드 진입점 4개를 설치했습니다. AA 자동 bootstrap을 실행한 것은 아닙니다.
   원본 요청·응답 callback의 아홉 scalar를 작성한 즉시 `WorkerScope`에 전달한 뒤,
   실제 제품 adapter capture·sink·queue·runtime worker·journal을 실행했습니다.
5. 실제 AA journal의 원시 `position` 9행과 별도 `lds_sideband` 9행을 고정 소스의
   분석기에 입력했습니다. 모두 transport endpoint와 원본 요청·응답 식별자로
   연결됐으며 payload나 가까운 관측 시각으로 연결하지 않았습니다.

LDS 쪽 parser scratch 초기값, 검증한 service validity mutex의 원본 libc 초기화,
문장·질의 순서는 작성한 조건입니다. AA 쪽 `mode=OBSERVE` 설정·설치 완료 표시·worker 스레드 시작과 즉시
`WorkerScope`도 작성했습니다. LDS 자동 loader는 실제 `SHADOW` 설정을 읽었습니다. 실제 원본 WorkerQueue, 전체 ServiceInit/SM,
장치 reader thread, AA 위치 송신·휴대폰·지도 앱은 실행하지 않았습니다.

## 결과

원본 초기 cache 질의 1회, 문장별 질의 7회, 새 쓰기 없는 재질의 1회를 확인했습니다.
아홉 응답의 모든 원시 위치 필드가 제품 출처 snapshot과 같았고, 기록 유실은 0입니다.
실제 AA worker의 종료 기록과 `capture.done`도 생성됐습니다. 관측 출처를 위치 값이나
품질 값으로 보충하지 않았습니다.

| 관측 단계 | mode, UTC, 위도, 경도, 고도, 방위, 속도, horizontal, vertical의 할당 번호 |
|---|---|
| 초기 cache | `0,0,0,0,0,0,0,0,0` |
| RMC A | `1,1,1,1,0,1,1,0,0` |
| GSA A | `2,1,1,1,0,1,1,2,2` |
| GGA A | `3,1,3,3,3,1,1,2,2` |
| GGA B만 갱신 | `4,1,4,4,4,1,1,2,2` |
| 새 쓰기 없는 질의 | `4,1,4,4,4,1,1,2,2` |
| RMC B | `5,5,5,5,4,5,5,2,2` |
| invalid GGA | `6,5,6,6,6,5,5,2,2` |
| invalid RMC | `7,7,7,7,6,7,7,2,2` |

`0`은 관측된 할당 출처 없음입니다. 번호는 제품이 관측한 할당 identity이며,
센서 생산 순번·측정 시각이 아닙니다. 같은 숫자를 다시 대입하면 새 할당이고,
갱신 없는 재조회는 이전 identity를 유지합니다. RMC의 mode 대입이 이전 GGA/GSA
상태에 의존할 수 있으므로 하나의 문장에 모든 물리 품질 출처를 귀속하지 않습니다.

분석기는 exact-key 연결 9건을 보고했지만 전체 결과는 **exit 2, 미판정**입니다.
직접 installer를 호출한 작성 AA 기동에서 runtime의 bootstrap 결과는
`not_attempted`로 남았고, 실제 type-1 AA 송신 payload는 실행하지 않았기 때문입니다.
해당 두 미판정 이유를 지우거나 성공으로 바꾸지 않았습니다.
`qualification=not_established`, `producer_time_status=unknown`,
`assist_ready=false`를 유지했습니다.

별도 검토자가 같은 전용 bus monitor와 실제 AA 로그를 다시 읽어 식별자 여섯 개로
9건을 연결하고 원본 응답 scalar 81개를 대조했습니다. 행 순서를 뒤집어도 결과가
같았으며, GUID·client·요청 serial·응답 serial·중복·payload 변조 여섯 가지는
거부됐습니다. 원본 실행을 여섯 번 추가한 결과가 아닌 저장 자료의 독립 대조입니다.

동일한 최종 DSO 조합의 별도 cold 실행은 LOCAL loading에서 OFF 대조·LDS 단독·
AA/LDS preload 양쪽 순서를 검사했습니다. GLOBAL loading에서도 OFF와 양쪽
preload 순서를 추가 대조했습니다.
원본 cache Initialize/Get/Update/Clear의 반환값·errno·원시 바이트가 같고, caller의
`dlclose` 뒤에도 제품이 보유한 원본 참조로 전달할 수 있었습니다. OFF에 활성 설치를
요구한 대조는 의도대로 실패했습니다. LAZY loading의 별도 native/OFF 두 실행에서도
원본 resolver 상태와 cache 전달 결과가 같았습니다.

## 보존한 실패와 한계

최초 최종 통합 실행에서 AA worker는 기본 sideband 채널의 `EADDRINUSE`를 기록했고
검사는 실패했습니다. 같은 환경의 전체 host/ARM 검사가 동시에 worker를 실행 중이었고,
실패 뒤 procfs에서도 해당 채널 사용을 확인했습니다. 실패 순간의 소유 PID까지는
확정하지 못했습니다. 두 전체 검사가 종료된 뒤 같은 제품·채널·caller로 재실행하여
통과했으며 제품의 재시도·가드·채널 정책은 바꾸지 않았습니다.

초기 작성 fixture가 LAZY import를 이미 해석된 주소로 가정한 실패와, ARM PLT 문맥 없이
resolver 포인터를 직접 호출한 실패도 별도로 보존했습니다. 원본·제품 결함으로 세지
않으며 해당 작성 호출을 고친 뒤 native/OFF 대조를 실행했습니다.

전체 ServiceInit에는 config·timer·control interface·system/USB 알림·SM 연결이 더
필요합니다. 현재 caller가 실행한 원본 초기화 일부를 전체 서비스 성공으로 대신하지
않습니다. 물리 센서의 생산 시각·누적 구간·품질, 실제 receiver 수명 자격, 보정값과
휴대폰 수용은 [v1.0 조건](../docs/V1_READINESS_KO.md)에 남습니다.

전용 bus·QEMU 자식·임시 `/jci` 및 `/data_persist` 별칭은 소유한 컨테이너 안에서만
생성했고 실행 후 제거했습니다. 제품·원본 입력 해시는 실행 전후 같았습니다.
이 실험이 새로 설치한 도구는 없으며 공유 검증 환경의 전체 정리는 별도 최종 기록을
따릅니다. 원본 코드·덤프·입력/응답 원문은 비공개 증거로만 보존합니다.
