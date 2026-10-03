# 읽는 순서

1. `report.md`: 한국어 결론, 20개 유효 시험, 한계 및 제외 시도.
2. `evidence.json`: 환경·입력·관찰·시각·사진 참조의 구조화 자료.
3. `screenshots/`: 에이전트가 판독한 원본 DHU 화면 121장. 개인 알림이 포함된 A30-30 사진은 제외했습니다.
4. `commands/`, `commands-timed.jsonl`, `dhu.ini`, `geometry/`: 입력과 실행 추적.
5. `transport-summary.json`, `logs/`: TLS 협상 로그와 TCP read chunk 메타데이터. 복호화된 위치 메시지는 없습니다.
6. `install-cleanup.md`, `cleanup.json`: 설치 및 실제 정리 내역.

`requested-protocol.md`는 사용자가 제공한 원문 자료입니다. 실제 차량의 동작에 관한 설명은 검증되지 않은 배경 주장으로 취급하십시오. 관찰 결과는 보고서와 증거 JSON을 기준으로 읽으십시오. 보조 코드는 `methods/`에 있으며 독립 실행 패키지가 아닙니다.

SHA256SUMS는 각 파일의 무결성 확인용 목록입니다. OSM 도형 출처는 보고서에 표시했습니다.
