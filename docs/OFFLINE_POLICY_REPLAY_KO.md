# OBSERVE 로그로 다음 실험 준비 — draft

관련: #5, #6. 기준 소스는 OBSERVE 릴리즈 커밋 `048ee57`이다.
이 문서와 `tools/replay_location_policy.py`는 **PC에서 수행하는 후속 실험 준비**다.
실차 로그가 없는 상태에서 먼저 구현했으며 차량·휴대폰 검증을 대체하지 않는다.
CMU 실행 파일, installer, ASSIST 차단, 원본 send 1회 계약은 변경하지 않는다.

## 실행

이 PR의 `tools/`를 받은 PC에서 회수한 TAR를 직접 읽는다. 압축을 풀지 않는다.

```sh
python3 tools/replay_location_policy.py /path/to/mx5dr-logs.tar > policy-report.json
```

파일·디렉터리·TAR 입력과 로테이션 순서는 기존 `analyze_logs.py`를 재사용한다.
여러 세션을 별도 인자로 줄 때는 시간순으로 준다. 기존 릴리즈 ZIP에는 이 도구가 없다.
출력에는 좌표·payload hex를 넣지 않지만 시각·개수도 개인정보일 수 있으므로
실차 보고서를 공개 PR에 자동으로 업로드하지 않는다.

## 미리 구현한 가설

| 정책 | 원래 mode=0 LOCATION에 대한 오프라인 계산 | mode=1/2/3 |
| --- | --- | --- |
| OBSERVE | 원본 바이트 전달 | 원본 유지 |
| SCRUB | byte 32, 36..39, 40, 44..47만 0으로 설정 | 원본 유지 |
| DROP | 생략할 LOCATION 선택 수만 계산 | 원본 유지 |

SCRUB은 speed/bearing의 유무 flag를 지운다. 유효한 속도 0 전송이 아니다.
DROP의 생략된 호출 반환값·errno는 `null`, 부작용은 `not_established`로 남긴다.
기록된 send 반환값 분포는 mode별 참고 자료이며 가짜 성공 반환값의 근거가 아니다.
이 도구는 폰 fallback, 지도 앱의 외삽, 관성항법 위치를 시뮬레이션하지 않는다.

`project_payload()`는 이 바이트 정책을 실행하는 순수 함수다. DROP의 `None`은
OEM 호출을 실제로 생략하는 코드가 아니라 오프라인 payload 부재 표현이다.
리포트의 `counterfactual`은 세 정책으로 해당 **기록된 표본**을 선택했을 경우의
개수다. 로깅이 억제된 중첩 송신이나 실제 송신 부작용은 재구성하지 않는다.

## 유효성 및 구간 계약

기존 auditor의 전체 입력 검사 통과에 더해 다음 조건이 필요하다.

- OBSERVE boot(mode=1), runtime_mode=1, 명시적인 audit_fault=0.
- 정확한 position call/generation 상관과 mode 일치, position 시각 ≤ send 시각.
- 요청별 LOCATION 1개, type=1·length=48·원본과 송신 바이트 동일.
- mode=0은 OBSERVE의 DISABLED reason, mode=1/2/3은 NOT_UNKNOWN reason.
- flag byte는 0 또는 1. 알 수 없는 mode/reason과 send 시각 역행은 판정 보류.
- 기존 auditor가 요구하는 설치 성공·drop=0·최종 send 이후 health 등도 충족.

**하나라도 실패하면 전체 입력의 counterfactual을 비운다.** 정상 부분만 골라
실험 가능하다고 주장하지 않는다. 표본 수 등 남은 값은 불완전한 기술 통계다.
종료 코드는 0=로컬 기록 검사 통과, 1=불변식 위반, 2=불충분한 근거다.
0이어도 `live_activation_allowed=false`이며 설치 승인이 아니다.

구간은 같은 프로세스 boot와 mode에서 연속으로 관측된 LOCATION을 묶는다.
mode 변경·다른 send·불량 기록·2.5초 초과 간격에서 끊는다. `--max-gap-ms`는
이 그룹화 기준만 조절한다. 생산자 신선도 임계값이나 GPS 단절 감지기가 아니다.
시간은 마지막 관측 시각−첫 관측 시각이며 단일 표본은 0초다. 다음 표본까지
임의로 연장하지 않는다. 메모리 상한은 구간 10,000개, 요청 상관 100,000개이며
초과 시 조용히 잘라 성공하지 않고 판정을 보류한다.

반복 좌표는 동일 구간의 인접 byte 8..15 일치 쌍만 센다. 정차·양자화도 같은
좌표를 만들 수 있어 stale 또는 터널을 확정하지 않는다. mode=3 관측 개수는
순정 DR 경로의 후속 조사 자료이며 정확도·앱 수용 증거가 아니다.
SMDB/collector 시각은 기존처럼 receipt일 뿐이고 ASSIST 입력으로 승격하지 않는다.

## 리뷰와 실차 로그 이후에 결정할 것

1. 실제 v0.2.0 OBSERVE export에서 필드와 구간 분리가 맞는지 확인한다.
2. mode=0 반복 좌표와 optional field 존재를 폰/앱 화면·로그와 비교한다.
3. mode=3이 있으면 native 경로를 우선 검토한다. provider poll을 exact request로 취급하지 않는다.
4. DROP은 caller의 반환값 사용·상태/소유권/오류 부작용을 별도 분석한 뒤에만
   live adapter의 0회/1회 호출 계약을 설계한다. 현재 도구만으로 켤 수 없다.
5. yaw의 생산 시각·품질·보정이 확보되지 않으면 자체 ASSIST 경로는 계속 차단한다.

이 조건이 남아 있어 draft로 올리며 #5/#6을 닫지 않는다. 합성 테스트 통과는
바이트 선택·구간 경계·불완전 로그 보류를 검증할 뿐 실차 효과를 입증하지 않는다.
