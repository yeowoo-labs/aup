# 002 — DSL 방향과 text

2026-09-25. 상태: 방향 확정, v0b·v0c에서 구현.

## 위치

aup는 범용 언어가 아니라 **무엇이 무엇에 연결되는지를 선언하는 언어**다. 앱은 상태·입력·반응·보기를
적고, 저장·트랜잭션·화면 모델·스케줄링은 Pure가 맡는다. 인터페이스 어휘(`list`, `table`, `tone`,
`focus` 같은 것)는 언어에 넣지 않는다. **Vega는 aup 라이브러리**로서 그 어휘를 제공한다
(`use vega`). aup 자체는 일반적인 투영(`view`)과 모듈만 제공한다. Java 언어와 JavaFX의 관계와 같다.

## 원칙

1. 모든 구문은 Pure가 로드 전에 검증할 수 있는 것으로 컴파일된다. 무제한 반복·재귀는 없다.
   길이에 비례하는 연산은 이벤트 작업 예산을 쓰고, 넘으면 이벤트가 실패한다.
2. 이벤트가 트랜잭션이다. 효과는 이벤트 경계에서만 일어나고, 실패하면 아무것도 바뀌지 않는다.
3. 형태는 한 번만 선언한다. 같은 `record`가 저장·메시지·전송·보기에 쓰이고 표현은 런타임이 고른다.
4. 표면은 가독성을 우선한다. 줄바꿈으로 끝나는 문장, `{}` 블록, `state / input / on / view`.
   식별자는 당분간 ASCII다(키·도구·상호운용). 텍스트 내용은 전부 Unicode다.
5. 기능은 실제 소비자가 끌고 온다(v0b는 기존 편집기 동등성, 레코드·리스트는 Vega의 첫 컴포넌트).

## text

**한 타입이다.** `text`는 유효한 UTF-8의 불변 값이다. 한 단어든 1 MiB 문서든 같은 타입이다.
작은 값은 상태 안에 두고, 큰 값은 Pure 영속 트리의 익스텐트 열로 두지만 앱은 차이를 모른다.
"메모리 문자열과 파일·BLOB 문서는 다른 것"이라는 관례를 없앤다. 임의 바이트는 `bytes`다.
`bytes(t)`는 항상 성공하고, `text(b)`는 UTF-8을 검사해 잘못되면 이벤트가 실패한다.

```text
state title: text = "할 일"
state note: text = ""                  # 한 줄이든 1 MiB 문서든 같은 타입
state at: cursor in note               # 항상 글자 경계에 있고 편집을 따라간다

input typed = text                     # 호스트가 준 확정 입력(IME 조합 완료분)
input back = key Backspace
input left = key ArrowLeft

on typed      { note.insert(at, typed) }        # at은 삽입한 글자 뒤로 간다
on back.down  { note.remove(at - 1 char .. at) }
on left.down  { at = at - 1 char }              # 처음에서 멈춘다

view header = "{title} · {note.chars}자"
view current = note.line(at)           # 커서가 있는 줄. 복사하지 않는다
```

- **리터럴.** `"안녕"`. 이스케이프는 `\n \t \" \\ \{ \u{AC00}`. 보간은 `"남은 일 {remaining}개"`.
  여러 줄은 `"""`로 쓰고 공통 들여쓰기를 뺀다.
- **위치는 정수가 아니다.** 바이트·UTF-16·코드포인트 인덱스가 섞이는 문제를 없애려고
  `cursor in 값` 타입을 둔다. 커서는 항상 글자 경계에 있고, 범위를 벗어나지 않으며, 같은 이벤트의
  편집에 맞춰 런타임이 옮긴다. 이동은 단위로 쓴다: `at + 1 char`, `at - 1 line`, `note.start`, `note.end`.
  범위는 `a .. b`, 부분은 `note[a .. b]`, 줄은 `note.line(at)`이다. 모두 복사 없는 보기다.
- **글자 단위.** 목표는 사용자가 한 글자로 보는 단위(grapheme cluster)다. 첫 구현의 `char`는 Unicode
  scalar이고, 분절 표(UAX #29)는 크기 비용이 있어 선택 모듈로 붙인다. `word`도 그때 추가한다.
- **편집.** `insert`, `remove`, `replace(a .. b, s)`는 Pure draft의 splice가 된다. 이벤트가 성공하면 커밋된다.
- **길이는 단위를 적는다.** `note.chars`, `note.lines`, `note.bytes`(저장 크기). 모호한 `len`은 없다.
- **비교는 정확하다.** `==`는 바이트가 같을 때 참이다. 정규화를 조용히 하지 않고 입력 바이트를 보존한다.
  정준 동치는 Unicode 모듈의 `same(a, b)`, 로케일 정렬도 모듈이다. 기본 정렬은 바이트 순서(결정적)다.
- **숫자 표시.** 보간은 가장 짧은 왕복 10진수(워크벤치와 같음)이고, `{price:.2}`처럼 형식을 적을 수 있다.
- **표시 폭은 text의 일이 아니다.** 터미널 칸 수(동아시아 폭), 글꼴 shaping, 양방향 텍스트는 Vega 표면이 맡는다.

## 단계

| 단계 | 들어가는 것 | 합격 기준 |
|---|---|---|
| v0a ✅ | 스칼라 상태, 키 입력, 핸들러, 보기 | 워크벤치 엔진과 비트 단위 일치 |
| v0b | `bytes`, 키별 영속 문서, `message` 입력, 응답, `require` ✅ / `text`(리터럴·보간·`cursor in`·`char`/`line`), `text` 입력 | 기존 편집기 CLI와 바이트 단위 일치 ✅ / TUI 동작 |
| v0c | `i64 bool`, `record`, `list`/`map`, `index of`, 순수 `fn`, `use` 모듈 | 모든 연산에 실행량 상한 |
| v0d | `use vega`로 첫 인터페이스 투영(목록·표·커서·톤) | 한 명세가 두 표면 이상에서 동작 |

구현은 C로 손수 쓴 재귀 하강 파서를 유지한다. 문법은 EBNF로 문서화하고, 이후 `aup fmt`와 언어 서버를 붙인다.
