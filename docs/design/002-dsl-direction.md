# 002 — DSL 방향과 text

2026-09-25. 상태: 방향 확정. v0b 구현, v0c는 레코드·리스트·인덱스·조건식·표 보기·`fn`·`use`까지 구현.

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
| v0b ✅ | `bytes`, 키별 영속 문서, `message` 입력, 응답, `require`, `text`(리터럴·보간·`cursor in`·`char`/`line`), `text` 입력 | 기존 편집기 CLI와 바이트 단위 일치, text 편집이 참조 모델과 일치 |
| v0c | `record`, `list of`, `index of`, 조건식, 항목 투영(`each`), 순수 `fn`, `use` 라이브러리 ✅ / `i64 bool`, `map` | 모든 연산에 실행량 상한, 리스트가 참조 모델과 일치 ✅ |
| v0d ✅ | `use vega`로 첫 인터페이스 투영(목록·표·커서·톤), [Vega 001](/srv/git/vega.git) | 한 명세가 세 표면(TUI·CLI·NUI 문장)에서 동작, 표면은 보이는 칸만 계산 |

### 구현된 text (v0b)

`examples/notes.aup`가 작은 편집기 전체다. 위 문법 중 다음을 구현했다.

- `text` 상태·문자열 초기값, 문자열의 이스케이프와 `{보간}`(수와 text), `input 이름 = text`
- `cursor in 이름`, `char`/`line` 거리, `.start`/`.end`, `insert`/`remove`, `.line(커서)`
- text의 `.chars`/`.lines`/`.bytes`, 커서의 `.chars`(처음부터 센 글자 수)와 `.line`(줄 번호)
- `text(bytes)`/`bytes(text)`. 응답과 문서 `replace`는 text를 UTF-8 바이트로 받는다.

`char`는 Unicode scalar다. 여러 줄 문자열, `{price:.2}` 같은 형식 지정, grapheme과 `word`는 아직 없다.
삽입 위치에 있던 커서는 모두 삽입한 글자 뒤로 간다(입력 커서와 표시가 같은 규칙). 보간 안에는 문자열을 쓸 수 없다.

검증: 무작위 입력(ASCII·한글·이모지·결합 문자·NUL·줄바꿈)과 이동·지우기 4,800단계에서 본문, 커서, 표시,
보기가 Python 참조 모델과 같다. 이스케이프와 숫자 표시(`1e+21`, `0.30000000000000004`, `2.5e-7`, `-0`은 `0`)를
확인한다. 컴파일러와 Pure 실행기를 각각 sanitizer로 빌드해 같은 검사를 통과했다.

### 구현된 레코드·리스트 (v0c-1)

`examples/tasks.aup`가 할 일 목록 전체다.

```text
record Task {
  title: text                         # text 필드. 기본값은 "" (title: text = "..." 로 바꾼다)
  done = 0                            # 수 필드와 기본값
}

state tasks: list of Task             # 런타임이 소유하는 항목 목록
state at: index of tasks              # 항상 tasks의 항목을 가리키는 위치

on typed { tasks.append(Task { title: typed }) }         # 빠진 필드는 기본값
on toggle.down { tasks[at].done = !tasks[at].done }
on drop.down { tasks.remove(at) }
on again.down { tasks.insert(at, Task { title: tasks[at].title }) }
on down.down { at = at + 1 }                              # 마지막 항목에서 멈춘다

view current = tasks[at].title
view status = "{at.position + 1}/{tasks.count}"
```

- **위치는 정수가 아니다.** 커서처럼 `index of 리스트` 타입을 둔다. 인덱스는 항상 그 리스트의 항목을
  가리키고(빈 리스트면 0), 앞에 넣거나 지워도 같은 항목을 따라가며, 가리키던 항목이 지워지면 다음 항목
  (없으면 마지막)으로 간다. `at + n`, `at - n`은 처음과 마지막에서 멈춘다. `.first`/`.last`, `.position`(앞 항목 수).
- **다른 리스트의 인덱스는 컴파일 오류다.** `ys[at]`처럼 `xs`의 인덱스로 `ys`를 읽는 모듈은 Pure도 로드 단계에서 거부한다.
- **항목 읽기는 실패할 수 있다.** 빈 리스트의 `tasks[at]`은 이벤트를 `not_found`로 실패시킨다(아무것도 바뀌지 않음).
  보기는 그때 값이 없다(`pure-run`은 `null`).
- **변경은 되돌릴 수 있다.** 리스트 변경은 이벤트마다 기록되고, 이벤트가 실패하면 모두 되돌린다.
- **레코드를 바꿔도 데이터가 남는다.** 모듈을 교체할 때 리스트는 이름으로 남고, 레코드가 바뀌었으면 항목마다
  이름과 타입이 같은 필드를 옮기고 새 필드는 기본값을 쓴다. 마이그레이션 스크립트가 필요 없다.
- 레코드 값(`Task { ... }`)은 아직 `append`/`insert`의 인자로만 쓴다. 레코드 상태, 항목 전체 읽기, 정렬·검색은 없다.

한도: 레코드 8개, 레코드당 필드 16개, 전체 필드 64개, 리스트당 항목 65,536개.

검증: 무작위 추가·이동·토글·복사·이름 바꾸기·지우기 4,800단계(빈 리스트에서 실패하는 이벤트 약 500건 포함)에서
리스트, 인덱스, 보기, 실패 여부가 Python 참조 모델과 같다. 필드를 더하고 순서를 바꾼 레코드로 교체해도 항목이
남는다. 진단 25건을 더했다.

### 구현된 조건식과 표 보기 (v0c-2)

```text
view current = if tasks.count > 0 { at.title } else { "(empty)" }

view board = each task in tasks {        # 항목마다 한 행
  title = task.title                     # task는 그 행의 인덱스
  mark = if task.done { "x" } else { " " }
  here = task.position == at.position
}
```

- **조건식.** `if 조건 { 값 } else { 값 }`, `else if` 연쇄. `else`는 필수이고 모든 가지의 타입이 같아야 한다
  (커서·인덱스는 같은 text·리스트). 고른 가지만 계산하므로 `if x > 0 { 10 / x } else { 0 }`은 실패하지 않는다.
  보기·핸들러·식 안 어디서나 쓰며, 값을 가진 채 8단계 넘게 중첩하면 컴파일 오류다(Pure 검증기의 합류 한도).
- **인덱스로 필드 읽기·쓰기.** 인덱스가 자기 리스트를 알므로 `at.title`, `at.done = 1`이 `tasks[at].title`과 같다.
  그래서 레코드 필드 이름으로 `position`은 쓸 수 없다.
- **표 보기.** `view 이름 = each 행 in 리스트 { 열 = 값 ... }`는 리스트 항목마다 행 하나를 낸다. 열은 수나 text이고
  행 이름(인덱스)과 다른 상태를 읽는다. 호스트는 보여 줄 행의 칸만 Pure에 물어 계산한다. 목록을 화면 모델로 복사하거나
  보이는 범위를 앱이 계산하는 코드(가상 스크롤)가 필요 없다. 열 이름은 표 안에서만 유일하면 된다.
- 목록·표·포커스·톤 같은 인터페이스 어휘는 여전히 aup에 없다. Vega가 이 투영 위에서 제공한다.

검증: 표 열과 조건 보기를 더한 할 일 목록이 참조 모델과 4,800단계에서 같고, 고르지 않은 가지가 계산되지 않음을 확인한다.
7단계 중첩은 컴파일되고 Pure가 받아들이며, 8단계는 aup가 거부한다(Pure도 거부하는 깊이). 진단 16건을 더했다.

### 구현된 fn과 use (v0c-3)

```text
# examples/words.aup: 상태 없이 fn만 가진 라이브러리
fn plural(count, one, many) = if count == 1 { one } else { many }
fn counted(count, one, many) = "{count} {plural(count, one, many)}"

# examples/tasks.aup
use words
view summary = words.counted(tasks.count, "task", "tasks")
```

- **fn은 식 하나다.** `fn 이름(매개변수, ...) = 값`, 매개변수가 없으면 `fn limit = 3`(상수처럼 `limit`으로 쓴다).
  매개변수에 타입을 적지 않는다. 부르는 곳마다 인라인하므로 수·text·커서·인덱스 무엇이든 받는다. 본문의
  타입 오류는 본문 위치에 `(in fn 'f' called at 줄:열)`을 붙여 알린다.
- **런타임 호출이 없다.** 인자는 부를 때 한 번 계산해 스택에 두고, 본문은 Pure의 PICK으로 읽고 SLIDE로 치운다.
  인자를 여러 번 써도 다시 계산하지 않으며, 쓰지 않는 인자도 계산한다(값 전달). 재귀는 컴파일 오류다.
  그래서 실행량 상한과 로드 전 검증이 그대로 유지된다.
- **use는 라이브러리를 가져온다.** `use words`는 소스 옆의 `words.aup`, 없으면 `AUP_PATH`(`:`로 구분)의 디렉터리에서
  찾는다. 라이브러리는 `fn`과 `use`만 선언하고 앱의 상태를 볼 수 없다(매개변수만 읽는다). 부를 때는
  `words.counted(...)`처럼 라이브러리 이름을 붙인다. 순환 use는 오류이고, 라이브러리의 오류는 그 파일 경로로 알린다.
- Vega는 이 자리에 들어간다. 인터페이스 어휘를 fn과 표 보기 위의 규약으로 제공하는 aup 라이브러리다.

한도: fn 64개, 매개변수 8개, 라이브러리 8개.

검증: 라이브러리를 쓴 할 일 목록이 참조 모델과 일치하고, 인자를 한 번만 계산하며(값 전달), 라이브러리 경로·순환·
상태 선언·앱 상태 읽기 오류가 해당 파일 위치로 보고된다. 같은 소스와 라이브러리는 같은 모듈을 만든다. 진단 10건을 더했다.

구현은 C로 손수 쓴 재귀 하강 파서를 유지한다. 문법은 EBNF로 문서화하고, 이후 `aup fmt`와 언어 서버를 붙인다.
