# Graphify 프로젝트 전용 구성·초기 그래프 — 2026-10-28

## 범위와 실행 환경

소유자가 이 저장소에 한해 Codex CLI·Claude Code·OpenCode 스킬과 프로젝트
지침을 적용하고, uv로 격리 실행하며, 설치 뒤 그래프까지 생성하도록
요청했습니다. 버전 업그레이드는 허용했습니다.

기준 HEAD는 `02e58c08f85c07b6cec65c78adb4266a9343253b`이며, 이 기록은 그 위의
Graphify 구성 작업 트리를 검증한 결과입니다. 공식 PyPI 패키지
`graphifyy`의 실행 시 버전은 **0.9.97**입니다. 버전 고정 설정은 없습니다.

`tools/graphify`는 저장소 루트를 선택하고 `uv tool run --isolated --from
graphifyy graphify`를 실행합니다. 패키지와 의존성은 uv 캐시의 도구 환경에
있으며 전역 CLI/스킬 설치 명령은 실행하지 않았습니다. 사용자 전역 설정,
Git 훅, MCP, 상주 감시기도 추가하지 않았습니다.

스킬은 `.agents/skills/graphify/`, `.claude/skills/graphify/`,
`.opencode/skills/graphify/`에 있습니다. 공통 절차는 첫 경로에 두고 나머지
두 진입점이 참조합니다. 루트 `AGENTS.md`에 프로젝트 지침을 추가했고
`CLAUDE.md`는 그 지침을 가져옵니다. 공식 CLI를 사용하는 프로젝트 전용
스킬이며, 전역 설치·업그레이드를 수행하는 upstream 스킬을 그대로
설치한 것은 아닙니다.

## 초기 그래프 생성

실제로 실행한 명령은 다음과 같습니다.

```sh
./tools/graphify extract . --code-only --max-workers 2
./tools/graphify cluster-only . --no-label
./tools/graphify query "G3 heading carry NO_FIX bearing overlay" --budget 1200
./tools/graphify explain G3Motion
```

모두 종료 코드 0입니다. 결과는 Git에서 제외한 `graphify-out/`에 있습니다.

| 항목 | 관측 결과 |
|---|---|
| 코드 분석 대상 | 452개 파일 |
| 그래프의 고유 `source_file` | 417개(빈 경로 제외) |
| 노드 / 연결 / 커뮤니티 | 11,908 / 27,858 / 498 |
| 보고서의 추출 / 추론 비율 | 91% / 9% |
| 의미 분석에서 제외한 비코드 자료 | 820개(문서 771, 이미지 49) |
| 미분류 자료 | 29개(`.S`, AndroidManifest.xml 등 포함) |
| 분석했으나 심볼이 없는 파일 | 3개(아래 참고) |
| HTML 기본 시각화 | 커뮤니티 498개, 커뮤니티 간 연결 1,690개 |

심볼이 없는 파일은 `packaging/usb-entry/js/run.js`, `src/runtime/sha256.h`,
`tests/runtime/loader/bad.c`입니다. 분석 대상 수를 모든 파일·심볼의 완전한
포함 증거로 해석하지 마십시오. 미분류 파일과 문서의 의미는 그래프에
포함하지 않았으며 원문 확인이 필요합니다.

`graph.json`, `GRAPH_REPORT.md`, `graph.html` 생성을 확인했습니다. HTML은
기본 5,000노드 한계를 넘어 커뮤니티 요약 보기로 생성됐고 브라우저에서
시각적으로 확인하지는 않았습니다. CLI 질의는 전체 그래프를 사용합니다.
`G3Motion` 질의는 `src/navigation/pipeline.h`의 선언과 `G3Sample`,
`src/runtime/g3_heading.h` 등의 참조 관계를 실제 반환했습니다.

## 격리·제외 규칙 검증

- `GRAPHIFY_BACKEND=none`으로 외부 LLM 자동 선택을 끄고 `--code-only`로
  생성했습니다. 보고서의 모델 토큰 사용은 입력 0, 출력 0입니다.
- 사용자 전역 질의 로그는 `GRAPHIFY_QUERY_LOG_DISABLE=1`로 껐습니다.
- 임시 디렉터리에서 실제 Graphify `detect`를 실행해 일반 소스와 깊이 8의
  소스는 포함되고 비공개 자료·다른 worktree·OEM 분석물·깊이 9 이상의
  소스는 제외되는 것을 확인했습니다.
- 실제 `graph.json`의 모든 비어 있지 않은 `source_file`을 jq로 검사해
  비공개 경로/다른 worktree 경로 0개, 깊이 8 초과 경로 0개를 확인했습니다.
- `git check-ignore`로 그래프 JSON과 HTML이 Git에서 제외됨을 확인했습니다.
- 셸 구문 검사, 세 스킬의 `quick_validate.py`, `git diff --check`가
  통과했습니다.

## 클라이언트와 회귀 검사

세 클라이언트의 프로젝트 스킬 파일 검증은 통과했습니다. 새 클라이언트
세션을 열어 스킬 발견·모델 실행을 확인하지는 않았습니다.

OpenCode는 `opencode debug --help`부터 종료 코드 1이며,
`@opencode/cli's postinstall script was not run` 오류를 출력했습니다.
기존 CLI 설치 문제로 실행 검증을 진행하지 못했습니다. 프로젝트 한정
요청을 유지하기 위해 전역 OpenCode 설치를 고치지 않았습니다.

`make test`도 시작했으나 개발 도구 구성에 비해 제품 전체 회귀 검사의
범위가 지나치게 넓고 오래 걸려, `test-runtime`의 BETA worker 시나리오
실행 중 이 작업이 시작한 프로세스 그룹에 SIGINT를 보내 중단했습니다
(종료 코드 130). 중단 전까지 진행 로그에 실패 보고는 없었지만 **전체
미완료이며 통과로 기록하지 않습니다**. 이후 제품 회귀 검증이 필요하면
별도로 처음부터 완료해야 합니다. Graphify에 필요한 생성·질의·스킬·제외
범위 검증은 위와 같이 완료했습니다.
ARM·QEMU·차량·폰 시험은 실행하지 않았습니다. 이 변경은 호스트 개발 도구
구성이며 제품 기능·릴리즈·설치 승인이나 실차 검증 수준을 바꾸지 않습니다.

사용법과 업그레이드는 [프로젝트 안내](../docs/GRAPHIFY_KO.md)를 따르십시오.
