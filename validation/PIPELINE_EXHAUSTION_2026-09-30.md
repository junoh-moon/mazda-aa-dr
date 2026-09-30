# Pipeline 순번 소진 시 상태 철회 — 2026-09-30

기준 커밋은 `8cbcc29f8748c37793bd1f31322d58ceae8e1372`입니다. 공개
Pipeline API에서 generation을 `UINT64_MAX-1`로 시작하여 정상 MODEL 예측을
만든 뒤 순번 소진 상태에서 잘못된 입력을 넣었습니다. 이전 구현은 파이프라인을
비활성화하면서 estimate의 두 유효성 플래그만 지웠습니다. 이후 진단의 core
query가 아직 남은 seed에서 `model_valid=true`, `result=OK`를 다시 만들었습니다.
실제 물리 입력이 이 극단적인 순번에 도달했다는 증거는 없습니다.

별개로 위치 순번을 `UINT64_MAX`로 둔 qualified 공개 API 재현에서, 처리 중
`control()`의 fault가 큐를 비운 뒤 `drain()`이 기존 큐 크기를 다시 감소시켜
ASan/UBSan에서 `Event[128]`의 index 129 범위 위반과 충돌을 일으켰습니다.
같은 큐 초기화는 generation 소진 시에도 발생할 수 있었습니다. 원래 코드의
generation 소진 후 revoke·위치 처리·anchor 교체 경로는 reset 오류를 `OK`
또는 다른 결과로 덮기도 했습니다.

수정한 `fault()`는 generation을 감싸지 않고, 활성 코어·seed·대기 큐·보정
상태를 먼저 reset한 다음 순번 소진 시 파이프라인을 비활성화합니다.
`drain()`과 위치 처리는 내부 reset을 감지하면 즉시 해당 fault를 반환하여
초기화된 큐에 재접근하거나 원인 상태를 덮지 않습니다. 위치 기준점 순번을
증가시키는 경로도 소진 여부를 확인합니다. 정상 경로의 순정 전달 계약이나
ASSIST 설정은 바꾸지 않았습니다. 비활성화된 인스턴스의 재사용에는 명시적인
`init_model()` 또는 `init_qualified()`가 필요합니다.

새 회귀는 MODEL의 진단·미리보기 차단, qualified의 직접 입력/revoke/위치/
anchor 교체, 비말단 위치 순번 소진 뒤 재초기화와 정상 재시작을 검사합니다.
기존 소스와 새 테스트를 조합한 ASan/UBSan 음성 대조에서 MODEL 유효성 재노출,
revoke·위치의 `drain=OK`, anchor의 잘못된 결과, 위치 순번의 범위 위반을 각각
재현했습니다. 수정본의 GNU native와 ASan/UBSan navigation은 각각 **2,414개**
검사를 통과했습니다. 이 극단적 generation에서는 core 계산이 유효해도 wire의
32비트 generation 표현 범위를 넘으므로 qualified bridge는 `OVERFLOW`입니다.
해당 상태를 ASSIST 가능 결과로 세지 않았습니다.

root가 직접 실행한 전체 `make test`는 GNU에서 Python **330개**와 C/C++
검사를 skip 없이 통과했습니다. 작성 worker **23개**, raw **12,146개**의
일반 판정은 모두 inconclusive, violation 0입니다. 예상된 reset/rejection을
성공적인 위치 검증으로 세지 않았습니다. 고정 GCC 4.9.1의 제품 ARM 5개를
57개 입력 해시와 함께 새로 빌드했고, QEMU의 ARM 전체 runner와 navigation
2,414개 검사가 통과했습니다. `libmx5dr.so` SHA-256은
`c15b0a9db926021def9baa4216a36bbace87d732e8fa88d558d2efc51ac450a7`
입니다. 빌드와 실행 로그·음성 대조는 비공개
`evidence/pipeline-exhaustion-20260930/`에 보존했습니다.

Claude Code의 독립 정적 리뷰에서는 수정본의 P1/P2가 보고되지 않았습니다.
리뷰어가 호스트·ARM 결과를 재실행해 확인한 것은 아니며, 후속 잘못된 입력이
첫 terminal fault의 `status.result`를 바꿀 수 있다는 낮은 우선순위 관찰이
있습니다. 별도 Codex 리뷰 위임은 자동 심사에서 거절되어 이번 변경에 대한
독립 Codex 검토 완료를 주장하지 않습니다.

이번 단위에는 새 순정 VM 실행, 유효 LDS GPS 수신, 물리 차량·폰 검증이
없습니다. 공개 ZIP은 여전히 `v0.3.1-shadow.1`입니다. ASSIST는 꺼져 있고
센서 생산 시각·출처, 정상 전체 기동·복구, 물리 정확도와 앱 수용은 별도
미완료 조건입니다.

| 파일 | SHA-256 |
| --- | --- |
| `src/navigation/pipeline.cpp` | `129a06aaf6c6b66a071297b65bb1648f9c33c8c9841bbe0002e11693b7b18562` |
| `tests/navigation/test_navigation.cpp` | `b842926bd34500b929e9ff397851dd53bbad52bf52c66f4f7a0b034d1a6e44c4` |
| 전체 host 로그 | `dbef14822f5fd577cf011bf20822120627d716fbbd155e1937660d427ad7dd4d` |
| 전체 ARM 로그 | `bb518352c04e726b6364e24ac79da359bdd24e6140985fbe8489b4e6c75c3b11` |
