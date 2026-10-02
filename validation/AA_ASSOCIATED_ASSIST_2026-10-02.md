# 같은 LDS 연결의 ASSIST 적용·복구 — 2026-10-02

고정 `d66ec1ef2f8e7116e5735f518da3ac27004cb1f8`의 실제 LDS·AA 제품을
사용해, 현재 callback의 LDS 연결과 작성한 자격의 ASSIST 계산·원본 AA
송신을 한 실행에 결합했습니다. 기존 [null-reader 복구 검사](AA_D66_APPLICATION_2026-10-02.md)와
별도 실행입니다. 제품 소스와 여섯 배포 바이너리는 변경하지 않았습니다.

**자격 거부 경우의 저장 캡처는 후속 판독을 통과했지만, epoch 불일치
경우는 마지막 GPS 복귀 대기에서 실패했습니다.** 전체 두 경우를 통과로
세지 않습니다. live ASSIST와 물리 입력 공급부는 여전히 미완료입니다.

## 실제로 연결한 경로

원본 PTY reader·parser·cache·LDS 응답에서 실제 제품 Publisher와 읽기 전용
AA Registry를 거쳐, 같은 `PositionContext`의 Owned 사본을 읽었습니다.
reader 주소는 해당 AA ELF에서 얻었고 Registry 소스 링크, map/Owned 주입,
늦은 응답으로 callback 출처를 수정하는 방법은 사용하지 않았습니다.

같은 callback의 MATCHED 사본을 작성 자격의 필요조건으로 사용했습니다.
센서 값·측정 시각·보정·품질·source/session epoch는 작성 입력입니다.
실제 제품 AssistWorker가 계산한 결과를 원본 BLM LOCATION 경로에서
송신했습니다. 원본 모듈 아홉 개와 제품이 설치한 30개 슬롯을 사용했으나,
작성한 부분 초기화이며 정상 전체 SM/AAPA 기동 시험은 아닙니다.

최초 worker 기동 전 연결할 수 없었던 POSITION/SEND도 원시 기록에
보존했습니다. 초기 UNAVAILABLE 구간만 정규화 입력에서 제외하고, 첫
MATCHED GPS로 BEGIN·기준점을 만들었습니다. 이후 POSITION은 음성 입력을
포함해 동일한 내용·순서로 공급합니다. 관측 시각은 물리 측정 시각이 아닙니다.

## 자격 거부 실행과 저장 자료 판독

첫 launcher는 32.553883031초 후 **종료 1**이었습니다. client와 server는
각각 0으로 끝났고 최초 계산·연결 판정도 통과했으나, wire 판정기가 정상
수집 종료 뒤의 OBSERVE 상태까지 ASSIST로 기대하여 실패했습니다.
실제 `freeze_capture()`는 종료 때 OBSERVE로 철회합니다.

requested capture_end 뒤 마지막 OBSERVE만 허용하도록 판정기를 고쳤습니다.
조기 모드 전환, audit/drop/request 유실, 미철회, 남은 resolver 항목은 계속
거부합니다. 별도 작성 대조와 동일 저장 자료의 후속 application·wire·startup
판독이 모두 0입니다. 최초 실패로 실행하지 못했던 hardware 접근 부재 판독도
같은 저장 strace로 별도 완료했습니다. 원본 실행을 다시 하지 않았고 최초
`run.success=false`·종료 1·오류 출력은 그대로 보존했습니다.

독립 검토자가 원문에서 확인한 결과는 다음과 같습니다.

- 위치/송신 21쌍과 원시 값 189개. 초기 UNAVAILABLE 세 쌍 뒤 MATCHED
  18쌍을 확인했으며 같은 backing object의 LDS 쓰기·AA 읽기 전용 매핑입니다.
- 정규화 POSITION은 호출 4–21을 순서대로 소비했습니다. 원문 값·토큰·
  Owned·수신 시각을 바꾸지 않았습니다.
- DR 대체 6건 중 자격 상실·새 GPS/BEGIN 뒤 대체가 3건입니다.
- 호출 19는 같은 MATCHED Owned를 유지한 채 작성 자격만 거부했습니다.
  SEND는 BAD_PROVENANCE와 원본 48바이트이며, 후보가 관측된 상태였습니다.
  37.673549ms 뒤 해제한 동일 POSITION이 입력 411로 소비되어 INPUT_FAULT와
  철회 증가를 일으켰습니다. 마지막 GPS 두 건도 원본으로 전달됐습니다.
- 원본 Close의 fd=-1, 모듈/슬롯, 저장된 입력 172개·소스 432개 전후,
  소유 프로세스·IPC·별칭 정리를 대조했습니다. 파일 소유권 94항목을
  복원할 때 내용·기타 보존 대상 metadata는 바꾸지 않았습니다.

독립 원문 판독 결과 SHA-256:
`1d33a6ceef78128f4240c57c728ab92b34c16743cb521ef2f4933a2cc017af29`.
수치 판정은 작성 모델과의 일치이며 실제 위치 정확도가 아닙니다.

## epoch 불일치의 최초 실패

두 번째 실행은 같은 C++·원본 13문장 입력을 사용하고 작성 provenance의
source epoch만 101에서 102로 바꿨습니다. MATCHED Owned와 요청 식별자를
바꾸지 않았습니다. 31.109986587초 후 launcher 1, client 91
(`final_original_gps_forwarding`)로 끝났습니다. 시간 초과에 의한 host 강제
종료는 아니며 소유 자원 정리와 입력 172개 전후 동일성은 확인했습니다.

journal은 완전한 370행 뒤 371행의 497바이트가 잘렸습니다. 전체 캡처가
아닙니다. 완전한 prefix에는 위치/송신 19쌍·DR 대체 6건, 호출 19의
MATCHED·EPOCH_MISMATCH·원본 송신과 입력 411의 INPUT_FAULT가 있습니다.
이 부분 관측을 두 번째 경우의 전체 계산·복구 통과로 세지 않습니다.

최종 PTY 입력과 원본 cache 갱신은 관측했으나 이후 GPS 송신 두 건이 오지
않았습니다. 저장 monitor에는 마지막 GetPosition의 응답과 두 control 요청의
ServiceUnknown 응답이 있으며, 그 뒤 새 GetPosition이 없습니다. 내부 수신·
대기 경계의 원인은 조사 중입니다. 기존 원본 요청 정체와 같은 원인이라고
단정하거나 재실행 성공으로 첫 실패를 덮지 않습니다.

## 독립 검토와 제한

준비 중 coordinator의 두 번째 drain 뒤 pause 누락과 판정기의 Owned·
시각 불변식 누락을 작성 반례로 보완했습니다. 실제 Claude 검토는 시간 제한
없이 326.416550189초 후 종료 0으로 끝났습니다. 후속 독립 검토에서 같은
입력 집합의 순서 변경을 잘못 승인하던 판정기도 보완했습니다.

후기 GPS가 음성 입력을 추월한다는 Claude의 예시는 전체 coordinator의
release·실제 fault 대기 때문에 도달할 수 없었습니다. 첫 SEND 전 후보의
lease가 끝나도 이미 소비한 다음 window의 발행이 계속 유효할 수 있음을
실제 기록으로 구분했습니다. 제한이나 제품 lease를 늘리지 않았으며 모든
스케줄에서 시험이 성공한다고 주장하지 않습니다.

이번 단위는 private fixture·판독과 새 실행 증거입니다. 제품 변경이 없어
전체 host/ARM suite를 다시 실행하지 않았으며, 기존 전체 검사 횟수를
새 결과에 더하지 않았습니다. 실차 기동·물리 센서 시각/품질·S25/지도 수용은
미검증이고 실제 qualified 공급부는 TODO입니다. 이번 기록은 공개 ZIP을
바꾸거나 추가 차량 시험을 요청하지 않습니다.
