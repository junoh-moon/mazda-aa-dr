# 세션 경계의 송신 후보 철회 — 2026-09-30

**세션 후크가 기존 DR 송신 후보를 철회하지 않는 결함을 수정했습니다.**
생성·파괴·상태 callback 전후에 adapter generation을 무효화하고, 원본 호출이
진행 중이면 세션 조회를 `transition`으로 표시합니다. 세션 reader를 사용하는
ASSIST 선택은 실제 send 저장소가 관측된 live 상태가 아니면 원본을 전달합니다.
관측된 live handle은 요청 자격·정상 폰 연결·폰 수용을 증명하지 않습니다.
제품의 `provenance=false`, `allow_assist=false`는 유지했습니다.

## 결함과 변경

이전 세션 관측은 생성·파괴·상태를 기록했지만 adapter의 `invalidate()`와
연결되지 않았습니다. 명시적인 합성 qualification을 공급한 검사에서 파괴 후의
이전 snapshot 재등록이 성공했고, 원본 lifecycle 함수 안에서도 DR 대체가
선택됐습니다. 실사용 ASSIST가 켜져 있었다는 의미는 아닙니다.

- 생성 실패·파괴 실패도 호출 진입과 종료에서 이전 후보를 철회합니다.
- 종료 때 다시 철회하여, 전환 중 별도 worker가 발행한 후보도 이후 사용하지
  못하게 합니다. C++ 예외와 deferred cancellation도 같은 정리를 거칩니다.
- 상태 callback도 원본 호출 전체를 전환 구간에 포함합니다. 동시 reader는
  기다리지 않고 `transition`을 반환하며 callback event를 버리지 않습니다.
- OEM 인자·포인터·반환·errno와 정확히 한 번의 원본 호출을 보존합니다.
  송신 선택 뒤의 OEM teardown을 동기화하거나 원본 호출을 취소하는 변경은 아닙니다.

[세션 회귀](../tests/adapter/session_hooks_test.cpp)에 파괴·재생성·생성 실패·파괴
실패·상태 변경과 세 함수의 진행 중 publication 등 **8개**를 추가했습니다.
각 경우의 최초 합성 후보는 선택되고, 전환 전/도중 후보는 거부되며, 전환 후
새 generation에 명시적으로 발행한 후보는 선택됩니다. 새 POSITION만으로
이전 후보를 다시 사용할 수 없다는 검사도 포함합니다. 최종 fixture는 snapshot
등록을 별도 pthread worker에서 수행하며, 실제 계산·물리 자격을 생성하지 않습니다.

## 호스트·ARM·원본 공유 런타임

- 수정 전 native 검사에서 새 8개가 모두 실패하는 것을 먼저 확인했습니다.
  수정 전 제품 ELF `8f065b8a…`에서도 최종 worker fixture의 8개가 모두 실패했습니다.
- `make test`의 Python 290개와 C/C++ 검사를 실행했습니다. 최초에는 fixture 경로를
  지정하지 않아 packaging 21개가 생략됐습니다. 원본 파일과 이번 빌드의 비공개
  검사 ZIP을 지정하여 packaging 126개를 다시 실행했고 생략 없이 통과했습니다.
  이 후속 실행까지 합쳐 각 검사 그룹을 완료했으며 최초 skip 로그도 보존했습니다.
- 고정 GCC 4.9.1 전체 ARM runner를 통과했습니다. 최종 worker fixture로 다시
  실행한 host adapter와 실제 제품 DSO의 세션 **22개**도 통과했습니다.
  같은 22개를 원본 loader/libc/shared C++ runtime의 chroot에서도 통과했습니다.
- 최종 fixture의 ASan/UBSan 22개를 통과했습니다. LeakSanitizer는 실행하지 않았습니다.
  TSan 최초 순회는 두 번째 사례의 출력 없는 실패로 완료하지 못했습니다.
  같은 사례 재확인에서는 `unexpected memory mapping`과 성공이 모두 나타났습니다.
  `setarch x86_64 -R`로 해당 프로세스만 ASLR을 끈 TSan 22개는 모두 통과했습니다.
  최초 출력 없는 종료의 정확한 원인은 확정하지 않으며 초기 실패도 보존합니다.
- 설명 문서까지 반영한 최종 소스를 새 디렉터리에서 재빌드했습니다. 다섯 ARM
  산출물 모두 위 검사·아래 VM에서 실행한 산출물과 바이트가 같고, 최종 빌드
  입력 검증도 통과했습니다. 새 독립 에이전트 리뷰는 수행하지 않았습니다.

## 원본 LDS·AA 실행

NA 74.00.324A의 원본 kernel/rootfs/LDS/BLM/AA interface와 새 제품을 격리 VM에서
실행했습니다. 제품의 cold-install·원본 해시·함수 검사 조건을 유지했고, 실제
원본 API를 호출했습니다. 호스트 장치·네트워크·공유 디렉터리는 연결하지 않았습니다.
기존 CMU 진단 절차의 machine ID 조정 후 debugger를 분리한 실행이며,
정상 차량 전체 기동을 재현한 것은 아닙니다.

[앞선 지연 응답 시험](SESSION_TRANSITION_2026-09-30.md)의 두 경우를 다시 실행했습니다.
클라이언트 dispatch를 멈추어 전달만 지연했으며 LDS 응답 본문을 바꾸지 않았습니다.
실제 LDS 응답은 모두 mode 0·0값이며, 별도로 명시한 합성 GPS 한 건의 캐시와
구분했습니다. 각 경우의 실제 요청은 5·9건, 파괴를 가로지른 응답은 각 1건입니다.
정지 유지 경우의 지연 하위 송신은 없었고, 재생성 경우에는 이전 요청이 새 세션으로
type 1 LOCATION·type 21을 송신했습니다. LOCATION의 하위 반환은 0이며 OBSERVE
원본 바이트를 보존했습니다. journal/request loss와 session fault는 모두 0입니다.

원본 파괴에서 generation 7→9, 재생성에서 9→11을 확인했습니다. 지연 요청의
발행 문맥은 여전히 lifetime 1, 재생성 후 실제 송신은 lifetime 2입니다.
일반 분석기는 정지 유지에 `local_checks_pass`, 재생성에
`session_changed_since_issue` 2건과 `inconclusive`를 반환했습니다.
새 generation으로 실행된 늦은 POSITION이 새 요청 자격을 얻었다고 세지 않습니다.

별도 프로세스에서는 원본 create/start/status/stop/destroy 두 주기를 실행했습니다.
시작 인자는 명시한 합성 0값이며, 실제 callback은 INVALID(원시 state 0)입니다.
send/start 반환 0과 stop 264도 유지됐습니다.

| 주기 | 생성 후 generation | 실제 callback 완료 후 | 파괴 후 |
| --- | ---: | ---: | ---: |
| 1 | 4 | 6 | 8 |
| 2 | 10 | 12 | 14 |

세 caller 모두 완료 표식·exit 0·durable capture 종료를 확인했습니다. 바깥 runner의
175초 제한 종료 124/QEMU 종료 0은 통과 근거가 아닙니다. 전용 판정기는 생성·
파괴·callback의 generation과 기존 요청/송신·종료 조건을 확인했습니다. 세대 증가를
없애거나 callback caller의 종료 표식을 제거한 입력 세 개를 모두 거부했습니다.
첫 runner 시도는 아직 작성 중인 initrd의 변경을 감지하여 VM 시작 전에 거부했습니다.
이미지 생성 완료 뒤 실행한 결과가 위 근거입니다.

| 최종 실행 자료 | SHA-256 |
| --- | --- |
| 제품 `libmx5dr.so` | `e09cb36217254df3636dadd9208641804c366fd7547161e9c4449c261d9da898` |
| VM initrd | `1f1355a965a377e0523f2aec7f242b5d0484b4626211066aa687bbf5cee1807e` |
| 종료 후 console | `aad9f8e117815d9614d154f4dfbb05b46b866728074f4d8914d9ad148d332b14` |

## 정리와 남은 작업

비공개 `evidence/session-lifetime-20260930-854f74d`에 source patch·작성 caller·검사
원본·실패/통과 로그·산출물 해시를 보존합니다. OEM 바이너리나 전체 로그를 공개하지
않습니다. 전용 컨테이너의 추가 패키지 120개·갱신 패키지 6개와 고정 toolchain
2,124개 파일을 기록하고 컨테이너를 제거했습니다. 임시 디렉터리·VM 이미지·검사용
미게시 ZIP·호스트 임시 빌드도 제거했습니다. 호스트 패키지 설치는 없었으며,
Docker 이미지·컨테이너 목록이 설치 전과 같음을 비공개 cleanup 기록으로 확인했습니다.

**미구현:** MODEL 계산기·GPS holdout을 AA 세션 전환에 맞춰 reset하고 새 기준점을
요구하는 연결, 늦은 이전 요청을 새 계산 입력으로 취급하지 않는 처리, 실제 요청별
provider/receiver/session qualification입니다. adapter 후보 철회만으로 이를
완료했다고 세지 않습니다. 물리 센서의 시간·단위·품질, 폰/앱 수용과 실제 전원 복구도
미검증입니다. 공개 ZIP을 변경하지 않았고 v1.0은 아직 미완료입니다.
