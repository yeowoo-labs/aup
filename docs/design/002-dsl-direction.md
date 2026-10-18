# 002 — DSL 방향과 text

2026-09-25. 상태: 방향 확정. v0b 구현, v0c는 레코드·리스트·인덱스·조건식·표 보기·`fn`·`use`까지 구현.

## 위치

aup는 범용 언어가 아니라 **무엇이 무엇에 연결되는지를 선언하는 언어**다. 앱은 상태·입력·반응·보기를
적고, 저장·트랜잭션·화면 모델·스케줄링은 Pure가 맡는다. 인터페이스 어휘(`list`, `table`, `tone`,
`focus` 같은 것)는 언어에 넣지 않는다. **Vega는 aup 라이브러리**로서 그 어휘를 제공한다
(`use vega`). aup 자체는 일반적인 투영(`view`)과 모듈만 제공한다. Java 언어와 JavaFX의 관계와 같다.

## 원칙

1. 모든 구문은 Pure가 로드 전에 검증할 수 있는 것으로 컴파일된다. 끝을 모르는 반복(`while`)과 재귀도 있다(Pure 027).
   끝남은 문법이 아니라 예산이 보증한다. 걸음은 이벤트 작업 예산을, 부름의 깊이는 메모리 몫을 쓰고, 넘으면 이벤트가 실패한다.
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
state from: mark in note               # 같지만, 이 자리에 넣은 글보다 앞에 남는다

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
- **커서와 마크.** 둘 다 편집을 따라간다. 차이는 그 자리에 글을 넣을 때뿐이다. 커서(캐럿)는 넣은 글 뒤로 가고,
  마크(기억해 둔 위치, 선택의 시작점, 책갈피)는 글 앞에 남는다. 지울 때는 둘 다 지운 범위의 처음으로 모인다.
  타입은 같아서 커서에 쓰는 모든 것(이동, 비교, 범위, `insert` 위치)에 마크도 쓴다.
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
| v0c | `record`, `list of`, `index of`, 조건식, 항목 투영(`each`), 순수 `fn`, `use` 라이브러리, 집계, 키 있는 리스트(`map`의 자리), text 비교, 걷기(`for`)와 메시지의 행, `bool`, `i64` ✅ | 모든 연산에 실행량 상한, 리스트가 참조 모델과 일치 ✅ |
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
- 레코드 값(`Task { ... }`)은 `append`/`insert`/`add`의 인자와 항목 전체 대입(아래 "펼치기와 항목 전체")에 쓴다. 레코드
  상태는 없다. 찾기는 `first(...)`(조건)와 키 있는 리스트의 `xs[키]`(키)로 한다.

한도: 레코드 16개, 레코드당 필드 16개, 전체 필드 128개, 리스트당 항목 1,600만 개(2^24).

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
- **걸러진 표.** `view open_list = each t in tasks where !t.done { label = t.title }`는 조건이 참인 항목마다 행
  하나를 낸다(리스트 순서 그대로). 열의 행 이름은 여전히 항목이라 `t.position`은 리스트 안의 위치다. Pure가 상태가
  바뀐 뒤 한 번 걸러 행과 항목의 대응을 두므로, 열린 일·끝난 일처럼 같은 리스트를 여러 목록으로 보여도 상태를
  둘로 나눠 맞출 필요가 없다. 조건 안에는 집계를 둘 수 없다.
- 목록·표·포커스·톤 같은 인터페이스 어휘는 여전히 aup에 없다. Vega가 이 투영 위에서 제공한다.

검증: 표 열과 조건 보기를 더한 할 일 목록이 참조 모델과 4,800단계에서 같고, 고르지 않은 가지가 계산되지 않음을 확인한다.
7단계 중첩은 컴파일되고 Pure가 받아들이며, 8단계는 aup가 거부한다(Pure도 거부하는 깊이). 진단 16건을 더했다.

### 집계: count, sum, max, min

```text
view open = count(t in tasks where !t.done)
view points = sum(t.points for t in tasks)
view best = max(p.score for p in players where p.active)
view summary = "{count(t in tasks where !t.done)} open of {tasks.count}"
```

- **찾기.** `first(항목 in 리스트 [where 조건])`은 조건이 참인 첫 항목의 인덱스다(그 리스트의 인덱스 타입이라
  `at = first(t in tasks where t.title == name)`, `first(…).done = 1`, `first(…).position`으로 쓴다). 없으면 보기는
  비고 핸들러는 `not_found`로 실패한다. 찾으면 거기서 멈춘다.
- **모양.** `count(항목 in 리스트 [where 조건])`, `sum|max|min|distinct(값 for 항목 in 리스트 [where 조건])`. `distinct`는
  포함된 항목의 서로 다른 값의 수다(SQL의 `count(distinct …)`). 값을 둘 받는
  `max(a, b)`·`min(a, b)`는 그대로 호출이고, 같은 이름의 fn(`fn sum(a, b)`)도 그대로 부를 수 있다. 첫 식 뒤의
  `for`, 또는 `count(`의 `이름 in`으로 가른다.
- **의미.** 항목마다 블록을 계산해 센다(조건이 참인 항목), 더한다, 가장 큰·작은 값을 고른다. 포함된 항목이 없으면
  count·sum은 0이고 max·min은 값이 없다(보기는 비고, 핸들러는 `not_found`로 실패).
- **블록은 표의 열처럼 돈다.** Pure의 FOLD가 뒤따르는 블록을 항목마다 열로 실행한다. 블록은 자기 항목과 상태만
  읽는다. 바깥 fn의 매개변수, 둘러싼 열의 행, `dt`, 메시지 필드는 쓸 수 없다(쓰면 컴파일 오류로 이유를 알린다).
  집계 안에 집계는 없다. 그래서 비용은 블록 길이 × 항목 수로 한정되고, 목록의 사본이나 집계용 상태가 필요 없다.

검증: 무작위 추가·토글·이동·삭제 3,600단계에서 네 집계(조건 있는 것 포함, 빈 max·min, 보간 안의 집계)가 파이썬
참조 모델과 같다. 진단 7건(중첩, 리스트 아님, text 합, 열의 행, 바깥 매개변수, `dt`, `in` 누락)을 더했다.

### 키 있는 리스트와 text 비교

```text
record Tally {
  word: text
  times = 0
}
state tallies: list of Tally by word      # 단어 순서, 단어마다 하나

on typed {
  if !tallies.has(typed) {
    tallies.add(Tally { word: typed })    # 키 자리에 넣는다. 이미 있으면 이벤트가 exists로 실패
  }
  tallies[typed].times = tallies[typed].times + 1
  at = tallies[typed]                     # 키로 찾은 인덱스
}
on forget.down { tallies.remove(at) }     # 인덱스로도, tallies.remove("pear")처럼 키로도 지운다
view later = count(t in tallies where t.word > "m")
```

- **맵을 따로 두지 않는다.** `list of 레코드 by 필드`는 항목을 그 필드의 순서(수는 값, text는 바이트)로 두고 키마다 하나만
  둔다. 맵, 집합, 정렬된 맵이 한 리스트에 순서 규칙 하나를 더한 것이 된다. 그래서 `each`·걸러진 표·집계·인덱스·저장·
  교체·Vega 목록이 모두 그대로 쓰이고, 목록은 늘 정렬돼 보인다.
- **키로 찾는다.** `xs[키]`는 반씩 나눠 찾은 항목이다(`xs[키].필드`로 읽고 쓰며, `xs[키]`만 쓰면 그 인덱스). 없으면
  `not_found`로 실패하므로 있을지 모를 때는 `xs.has(키)`를 먼저 묻는다. `xs[인덱스]`도 그대로 쓴다.
- **넣기는 `add`다.** 자리를 고르지 않으므로 `append`와 `insert`는 키 있는 리스트에 쓸 수 없고(컴파일 오류가 `add`를
  알려 준다), `add`는 키 없는 리스트에 쓸 수 없다. 같은 키가 있으면 이벤트가 `exists`로 실패한다(아무것도 바뀌지 않음).
- **키는 바뀌지 않는다.** 키 필드에 쓰면 컴파일 오류다(지우고 다시 넣는다). 그래서 순서는 넣기로만 만들어진다.
- **리스트가 키를 얻으면.** 모듈을 바꾸거나 다시 시작할 때 키 없던 리스트가 키를 얻으면 Pure가 키 순서로 다시 놓고
  인덱스는 자기 항목을 따라간다. 같은 키가 둘이면 모듈을 받지 않는다. 데이터를 버려 맞추지 않는다.
- **text 비교.** `==`, `!=`, `<`, `<=`, `>`, `>=`가 text끼리도 된다. 바이트 순서다(UTF-8에서는 코드 포인트 순서와 같다).
  정규화하지 않는다(`é`의 두 표기는 다르다). 로케일 정렬은 앞으로의 모듈 몫이다.

검증: 무작위 3,600단계(실패 이벤트 약 670건 포함)에서 text 키 리스트(빈 text, 한글, 이모지, 앞이 같은 키)와 수 키
리스트의 항목·인덱스·보기·실패 종류가 파이썬 모델과 같다. 진단 13건을 더했다. `examples/tally.aup`가 예제다.

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
  인자를 여러 번 써도 다시 계산하지 않으며, 쓰지 않는 인자도 계산한다(값 전달). 재귀하는 fn과 블록 본문의 fn은 Pure의 함수로
  돈다(아래 "스스로 부르는 fn", Pure 027의 2c).
- **use는 라이브러리를 가져온다.** `use words`는 소스 옆의 `words.aup`, 없으면 `AUP_PATH`(`:`로 구분)의 디렉터리에서
  찾는다. 라이브러리는 `fn`, `record`(아래), `use`만 선언하고 앱의 상태를 볼 수 없다(매개변수만 읽는다). 부를 때는
  `words.counted(...)`처럼 라이브러리 이름을 붙인다. 순환 use는 오류이고, 라이브러리의 오류는 그 파일 경로로 알린다.
- Vega는 이 자리에 들어간다. 인터페이스 어휘를 fn과 표 보기 위의 규약으로 제공하는 aup 라이브러리다.

한도: fn 64개, 매개변수 8개, 라이브러리 8개.

검증: 라이브러리를 쓴 할 일 목록이 참조 모델과 일치하고, 인자를 한 번만 계산하며(값 전달), 라이브러리 경로·순환·
상태 선언·앱 상태 읽기 오류가 해당 파일 위치로 보고된다. 같은 소스와 라이브러리는 같은 모듈을 만든다. 진단 10건을 더했다.

### 라이브러리의 레코드 (구현)

[외부 리뷰 005](/srv/git/pure.git)의 (4)에서 시작한다. 라이브러리는 `fn`과 `use`만 선언할 수 있어서, 레코드는 앱의 파일
하나에 모여야 했다. 같은 저장소를 읽는 두 번째 모듈(옮기기 스크립트, 질의)은 레코드를 베껴 써야 했다. 상태는 앱 하나의
것이지만(원칙 2), 레코드는 상태가 아니라 형태다(원칙 3: 형태는 한 번만 선언한다).

```text
# store.aup: 레코드와 fn을 가진 라이브러리
record User { name: text, points = 0 }
fn rank(points) = if points > 100 { "gold" } else { "plain" }

# main.aup
use store
state users: list of store.User by name
input join = message { name: text }
on join { users.add(store.User { name: join.name }) }
view golds = count(u in users where store.rank(u.points) == "gold")
```

- **라이브러리는 `record`도 선언한다.** 여전히 상태, 입력, 보기, 핸들러, 출력은 선언하지 않는다.
- **쓸 때는 라이브러리 이름을 붙인다.** `list of store.User`, `rows: list of store.User`, `store.User { ... }`. fn과 같다.
  라이브러리 안에서는 제 레코드를 이름만으로 쓴다.
- **모듈에는 선언한 이름으로 들어간다.** Pure 모듈의 레코드 이름은 식별자이고, 상태·입력·보기와 한 이름공간이다(Pure
  `core/module.c`). 그래서 `store.User`는 모듈에서 `User`다. 레코드를 앱에서 라이브러리로 옮겨도 모듈이 같다. 같은 저장소를
  읽는 다른 모듈도 같은 레코드를 같은 이름으로 갖는다. 한 모듈에 같은 이름의 레코드 둘(앱의 것과 라이브러리의 것, 두 라이브러리의
  것)은 컴파일 오류다.
- **쓰는 레코드만 모듈에 들어간다.** 앱이 선언한 레코드는 지금처럼 모두 들어간다. 라이브러리의 레코드는 앱의 리스트 상태나
  메시지의 행 필드가 처음 쓸 때 들어간다. 레코드 많은 라이브러리(Vega)를 써도 쓰지 않는 레코드는 모듈의 자리를 차지하지 않는다.
- **레코드 값은 선언으로 맞춘다.** `store.User { ... }`는 `list of store.User`에만 들어간다. 다른 라이브러리의 같은 이름 레코드와
  섞이지 않는다.

착수 기준:

- 바꿀 결정: 라이브러리가 레코드를 선언하게 할지. 아래를 채우지 못하면 레코드는 앱의 파일에만 둔다.
- 최소 확인(`make check`):
  - 레코드를 라이브러리로 옮긴 할 일 앱(`examples/tasks.aup`)이 옮기기 전과 바이트 단위로 같은 모듈을 만든다.
  - 레코드 셋을 가진 라이브러리에서 하나만 쓰는 앱의 모듈에는 레코드가 하나다.
  - 앱이 저장소에 쓴 항목을, 같은 라이브러리를 쓰는 두 번째 모듈(옮기기)이 같은 저장소에서 읽고 바꾸고, 앱이 다시 읽는다.
  - 진단: 라이브러리 레코드를 이름만으로 쓰기, 라이브러리에 없는 레코드, 같은 이름의 레코드 둘, 라이브러리 레코드와 앱의
    상태 이름이 겹침, 다른 레코드의 값을 리스트에 넣기, 라이브러리의 상태 선언(지금처럼 오류).
  - aup, Pure, Vega의 검사가 모두 초록이다.

검증(`tests/check.py`의 `library_records`): 레코드를 라이브러리로 옮긴 할 일 앱이 옮기기 전과 같은 모듈(2,509바이트)을
만든다. 레코드 셋을 가진 라이브러리에서 하나를 쓰는 앱의 모듈에는 레코드가 하나다. 앱이 `pure-run --store`로 사용자 셋을
넣고, 같은 라이브러리를 쓰는 옮기기 모듈이 같은 저장소에서 사용자마다 점수 10을 더하고, 앱이 다시 열어 30을 본다. 라이브러리
레코드의 행 필드를 가진 메시지를 `pure-serve`에 JSON으로 보내면 행이 들어온다. 위의 진단 여섯과 함께 같은 이름의 레코드가 두
라이브러리에서 올 때, 다른 레코드의 값, 라이브러리 안의 이름 겹침, 키 있는 리스트의 힌트(`users.add(store.User { ... })`처럼
라이브러리 이름을 붙여 알린다) 진단을 더했다(12건).

한도: 라이브러리들이 선언하는 레코드는 모두 합쳐 64개다(모듈에는 그 가운데 쓰는 것만, 앱의 것과 합쳐 16개까지).

- 레코드 이름을 모듈에서 앞에 붙이지 않는 까닭은 Pure다. 모듈의 레코드 이름은 점이 없는 식별자이고 상태와 한 이름공간이다.
  `store.User`를 `store_User` 같은 이름으로 바꿔 넣으면 레코드를 옮길 때 모듈이 바뀌고, 같은 저장소를 읽는 질의(이름까지 같아야
  한다, Pure `same_states`)가 앱과 맞지 않게 된다.
- 값을 이름이 아니라 선언으로 맞추는 까닭: 쓰이지 않아 모듈에 없는 다른 라이브러리의 같은 이름 레코드가, 이름만 같아서 리스트에
  들어가는 일을 막는다.

### 상태의 수명: transient (구현)

Pure [018](/srv/git/pure.git)의 1단계다. 지금까지 모든 `state`는 저장됐다. 접속 수, 캐시, 계산의 중간값처럼 다시 시작한 뒤에는
뜻이 없는 상태도 커밋마다 저장소에 썼다. 이제 값의 수명은 선언이 말한다.

```text
state visits = 0                         # 저장한다: 다시 시작해도 남는다
transient state online = 0               # 앱이 도는 동안만: 저장하지 않고, 다시 시작하면 초기값
transient state recent: list of Hit
transient state status: text = "idle"
transient state picked: index of tasks   # 저장되는 리스트의 저장되지 않는 선택
on tap.down {
  let n = online + 1                     # 이벤트의 값: 이벤트가 끝나면 없다
  online = n
}
```

| | 실패한 이벤트 | 저장소 | 다시 시작하면 | 모듈을 바꾸면 |
|---|---|---|---|---|
| `state` | 되돌린다 | 쓴다 | 저장된 값 | 이름으로 잇는다 |
| `transient state` | 되돌린다 | 쓰지 않는다 | 초기값 | 이름으로 잇는다 |
| `let` | 버린다 | 없다 | — | — |

- **`transient state`** 는 모든 상태 타입에 쓴다(수, bool, i64, text, 커서, 리스트, 인덱스). 보기, 이벤트, 되돌리기, 메모리 예산,
  스냅숏의 읽기에서는 다른 상태와 같다. 다른 것은 저장소뿐이다: 그것만 바꾼 이벤트는 저장소에 아무것도 쓰지 않는다.
- 저장되는 커서나 인덱스는 transient text나 리스트를 가리키지 못한다(다시 시작하면 가리키던 것이 비어 있다). transient 커서나
  인덱스는 저장되는 것을 가리켜도 된다.
- 모듈의 상태 타입 워드에 비트 하나다(Pure 008). transient가 없는 앱의 모듈은 바이트 그대로 지금과 같다.
- 모듈을 바꿀 때(`aup watch`, 호스트의 SIGHUP) 저장되던 상태를 transient로, 또는 그 반대로 바꿀 수 있다. 저장소에 남은 옛 값은
  버린 상태의 것처럼 다룬다.
- `transient`는 예약어다.

검증(`tests/check.py`의 `transients`와 진단 5건, Pure의 `test-module`과 `server_transient`): 같은 앱의 transient 판과 저장 판의
모듈이 상태 타입 워드 세 바이트만 다르다. 같은 저장 이벤트(`tap`) 사이에 transient만 바꾸는 이벤트와 실패하는 이벤트를 섞은 실행의
저장소가, 섞지 않은 실행의 저장소와 파일과 바이트까지 같다. 다시 열면 transient는 초기값이고 저장되는 것은 그대로다. 달리는 동안
모듈을 바꿔 저장되던 것을 transient로, transient를 저장되는 것으로 바꾸면 다시 열었을 때 바뀐 대로다. `pure-serve`에서 transient만
바꾸는 요청 100개가 저장소의 어떤 파일도 바꾸지 않고, `kill -9` 뒤에 다시 띄우면 transient는 초기값이다.

### 메모리 안의 계산 (구현)

Pure [018](/srv/git/pure.git)의 2단계이고, [외부 리뷰 005](/srv/git/pure.git)의 (1)에서 시작한다. 반복이 리스트의 항목을 도는
걷기 하나뿐이고 걷기 안에 걷기가 없으며 이벤트 동안만 쓰는 리스트가 없어서, 그래프 탐색이나 수치 반복을 aup로 쓸 수 없었다.

```text
on route {
  let seen: list of Seen by node         # 이벤트의 리스트: 비어서 시작하고, 이벤트가 끝나면 없다
  let queue: list of Step
  queue.append(Step { node: route.from })
  seen.add(Seen { node: route.from })
  for i in 0 .. nodes.count {            # 세는 반복: 0부터 nodes.count 앞까지
    if i >= queue.count { break }        # 가장 안쪽 for를 끝낸다
    let q = queue.first + i              # 자리로 읽는 큐
    for e in edges[q.node * 1048576 .. q.node * 1048576 + 1048576] {   # for 안의 for
      if !seen.has(e.to) {
        seen.add(Seen { node: e.to, depth: seen[q.node].depth + 1 })
        queue.append(Step { node: e.to })
      }
    }
  }
  hops = if seen.has(route.to) { seen[route.to].depth } else { -1 }
}
```

- **`let 이름: list of 레코드 [by 필드]`** 는 이벤트의 리스트다. 핸들러의 맨 바깥 블록에 둔다. 상태 리스트가 하는 것을 모두 한다
  (`add`, `append`, `insert`, `remove`, `[키]`, `has`, `count`, `first`, `last`, `+ n`, 집계, `for`). 이벤트가 시작할 때 비어 있고
  끝나면 비워진다. 저장되지 않고 되돌리기를 기록하지 않는다. 모듈에서는 `let.이름`이라는 이벤트의 리스트 상태다(Pure 008의 행
  목록과 같은 플래그). 같은 이름의 리스트를 쓰는 핸들러들은 그 상태 하나를 함께 쓰므로 레코드와 키가 같아야 하고, 한 이벤트의 두
  핸들러가 같은 이름을 쓰지는 못한다. 상태 32개 안에 든다.
- **`for 이름 in 낮음 .. 높음 { }`** 은 이름을 낮음부터 높음 앞까지 1씩 올리며 돈다. 둘 다 수(정수: 아니면 `limit`)이거나 하나라도
  i64면 i64다. 이름은 읽기만 한다. `for x in 리스트`(걷기)와는 `in` 뒤가 리스트 이름이고 `{`·`[`·`where`가 따르는지로 가른다.
- **`break`** 는 가장 안쪽 `for`(걷기든 세기든)를 끝낸다.
- **중첩.** `for` 안에 `for`를 8겹까지 쓴다. 바깥 걷기의 항목은 안에서 읽는다(컴파일러가 안쪽 걷기 앞에서 인덱스 지역 칸에 담는다).
  안쪽 `for`는 바깥 걷기가 도는 리스트에 넣거나 그 리스트에서 지우지 못한다(가장 안쪽 걷기는 지금처럼 자기 리스트에서 지울 수 있다).
- **끝.** 모든 반복은 시작할 때 횟수의 상한이 정해진다. 끝을 모르는 계산은 상한을 적은 세기와 `break`로 쓴다(BFS는 노드 수,
  DFS와 다익스트라는 간선 수 + 1). 그래도 이벤트마다 실행량 예산(정하지 않으면 2^28 명령)과 메모리 한도(2^28 바이트, 예산이
  있으면 예산)가 있어서 고장 난 반복은 `work`나 `memory`로 끝나고 아무것도 바꾸지 않는다.
- `break`는 예약어다.

예제: `examples/routes.aup`(너비 우선, 깊이 우선, 무게로 가장 가까운 길), `examples/numbers.aup`(뉴턴의 제곱근, 배낭 문제).

검증(`tests/check.py`의 `computing`과 진단 14건, Pure의 `test-module`의 `loops`와 `server_runaway`): 무작위 그래프 20개의 질의마다
세 탐색이 Python 모델과 같다(600번). 뉴턴의 제곱근 200개가 같은 순서로 계산한 Python의 binary64와 비트까지, 단계 수까지 같다. 배낭
문제 100개가 동적 계획법 모델과 같다. 이벤트가 끝난 뒤 이벤트의 리스트는 모두 비어 있다.

### 계산하는 보기 (구현)

Pure [018](/srv/git/pure.git)의 3단계다. 핸들러의 계산은 소유자 스레드에서 돌아 그동안 다른 이벤트를 받지 못한다. 상태를 바꾸지 않는
계산(경로 찾기, 통계)은 보기가 맞는 자리다: 보기는 스냅숏을 읽는 연결 스레드에서 돈다.

```text
view distance(start: u32, goal: u32) {
  let seen: list of Seen by node
  let queue: list of Step
  queue.append(Step { node: start })
  seen.add(Seen { node: start })
  for i in 0 .. nodes.count {
    if i >= queue.count { break }
    let q = queue.first + i
    for e in edges[q.node * 1048576 .. q.node * 1048576 + 1048576] {
      if !seen.has(e.to) {
        seen.add(Seen { node: e.to, depth: seen[q.node].depth + 1 })
        queue.append(Step { node: e.to })
      }
    }
  }
  return if seen.has(goal) { seen[goal].depth } else { -1 }
}
```

- **`view 이름(인자…) { 문장… return 값 }`** 은 핸들러처럼 문장을 갖는 보기다. `let`(값과 리스트), `for`, `break`, `if`를 쓰고, 마지막
  줄의 `return`이 보기의 값이다(수, bool, i64, text). `return`은 본문의 마지막 줄에만 있다.
- **본문은 상태를 바꾸지 않는다.** 바꾸는 것은 그 `let`과 자기 리스트(`let x: list of R`)뿐이다. `send`, `reply`, `require`, `now`, `dt`는
  없다. 평가마다 `let`은 새로 시작하고 리스트는 비어서 시작한다.
- HTTP로는 지금의 보기와 같다(`GET /views/distance?start=1&goal=5`). 읽는 연결마다 자기 리스트를 가지므로 여러 연결이 동시에 다른 인자로
  물어도 된다. 이벤트처럼 실행량 예산과 메모리 예산이 있다(넘으면 422 `work`, 503 `memory`).
- 표 보기의 열과 필터는 지금처럼 식 하나다.
- `return`은 예약어다.

예제: `examples/routes.aup`의 `distance`(인자를 받는 BFS)와 `reach`(첫 노드에서 닿는 노드 수와 가장 먼 거리).

검증(`tests/check.py`의 `computing`과 진단 8건, Pure의 `test-module`의 `view_bodies`와 `server_view_bodies`): `reach`가 무작위 그래프
20개의 모든 단계에서 모델과 같다. `pure-serve`에서 64연결이 동시에 물은 `distance` 1,280개가 모두 모델과 같다. 실행량과 메모리 예산을
넘는 본문은 `work`와 `memory`로 답한다.

### 시간

```text
state started = 0
state last = 0
input tick = every 1 second                 # ms, second, minute, hour (1 ms–24시간)
on tick {
  if started == 0 { started = now }         # now: 이 이벤트의 시각, 1970년부터 ms(UTC)
  last = now
}
view elapsed = floor((last - started) / 1000)
view clock = "{floor(last / 3600000) % 24}:{floor(last / 60000) % 60}:{floor(last / 1000) % 60}"
```

- **시간은 입력이다.** `now`는 핸들러에서만 읽는 이벤트의 시각이다. 호스트가 이벤트마다 정하고 스크립트는 아무 시각이나
  재현한다(`pure-run`의 `time MS`). 보기에서는 쓸 수 없다. 보기는 상태의 순수 함수라, 시계를 보이려면 타이머가 상태에 적는다.
- **타이머는 입력이다.** `input tick = every 1 second`는 호스트가 앱을 돌리는 동안 주기마다 틱을 넣는다. 틱은 키 이벤트처럼
  저장되는 이벤트다. 놓친 틱을 몰아서 넣지는 않는다.
- **시각을 나누는 셈.** `floor(x)`와 `x % y`(곱셈과 같은 우선순위)를 더했다. `%`의 결과는 나누는 수의 부호를 따른다(파이썬과
  같다). `-7 % 3`은 2다. 0으로 나누면 이벤트가 `divide`로 실패한다.

검증: 무작위 시각(정수·소수 ms, 앞뒤로)과 두 타이머의 틱, 키 3,600단계에서 상태와 보기가 파이썬 모델(`math.floor`, `%`)과 같다.
진단 9건을 더했다.

### 다시 빌드: aup watch

```sh
aup watch app.aup -o app.pmod     # 저장할 때마다 다시 빌드
vega tui app.pmod --watch         # 실행 중인 앱이 새 모듈로 바뀐다(상태는 이름으로 이어진다)
```

- **빌드는 읽은 바이트의 함수다.** 빌드는 연 경로와 찾아봤지만 없던 경로를 모두 기록하고, 거기서 읽은 바이트의
  digest를 함께 남긴다(소스, 쓴 라이브러리, 앞선 탐색 위치). 다시 빌드할 이유는 이 경로 중 하나가 다르게
  읽히는 것뿐이다. 파일 시각을 보지 않으므로 빌드와 기록 사이의 저장을 놓치지 않는다. 같은 바이트를 다시 저장하면
  아무 일도 없다. 오류가 난 라이브러리를 고치면 다시 빌드되고, 앞선 탐색 위치에 라이브러리가 새로 생겨도 다시
  빌드된다.
- **쓰는 중인 파일은 빌드하지 않는다.** 바뀐 내용은 50 ms 간격으로 두 번 연속 같게 읽힐 때 빌드한다. 편집기가
  제자리에 쓰는 도중의 빈 파일이나 앞부분도 컴파일될 수 있는데, 그런 모듈로 바꾸면 실행 중인 앱은 사라진 상태의
  값을 잃는다.
- **모듈은 통째로만 바뀐다.** 결과를 임시 파일에 쓰고 rename한다(`aup build`도 같다). 오류가 나면 마지막 모듈을
  그대로 두고, 결과마다 stderr에 한 줄(`aup: built app.pmod` 또는 진단)을 쓴다.

검증: 소스 수정, 라이브러리 수정, 라이브러리의 오류와 수정, 앞선 위치에 라이브러리가 생김, 같은 바이트 저장, 두
번에 나눠 쓴 저장 10회(앞부분만으로도 컴파일되는 소스)에서 빌드된 모듈이 매번 기대한 보기를 낸다. 두 번 읽기를
빼면 나눠 쓴 저장 검사가 8번 중 8번 실패한다.

### 걷기와 메시지의 행

```text
record Line {
  item = 0
  qty = 1                                # 빠진 필드는 기본값
  note: text
}
input order = message {
  customer: u32
  lines: list of Line                    # 메시지의 행: JSON에서는 객체의 배열
}

on order {
  for l in order.lines {                 # 행마다 한 번, 순서대로
    stock[l.item].left = stock[l.item].left - l.qty
    require stock[l.item].left >= 0      # 한 행이 실패하면 주문 전체가 실패한다
    log.append(Entry { item: l.item, qty: l.qty })
  }
  count = count + order.lines.count
}
on clean {
  for s in stock where s.left <= 0 {     # UPDATE … WHERE, DELETE … WHERE
    stock.remove(s)
  }
}
```

- **`for 항목 in 리스트 [where 조건] { … }`** 은 핸들러 안에서 리스트가 가진 항목마다 블록을 한 번씩 돈다(Pure의 EACH).
  항목은 그 리스트의 인덱스라서 `l.qty`로 읽고 `s.left = 0`으로 바꾸며 `stock.remove(s)`로 지운다. 지우면 다음 항목을
  건너뛰지 않는다. 걷는 리스트에 넣을 수는 없어서(다른 리스트나 `for` 뒤에는 된다) 걷기는 끝난다. 블록 안의 집계는 걷는
  항목을 읽을 수 있다(`sum(m.qty for m in order.lines where m.item == l.item)`). `for` 안의 `for`와 `reply`는 없다.
- **메시지의 행.** 메시지 필드의 타입으로 `list of 레코드`를 쓴다. 핸들러에서 `order.lines`는 그 메시지의 행을 담은 리스트다
  (`for`, 집계, `.count`·`.first`·`.last`). 그 핸들러 밖에서는 쓸 수 없다. 모듈에서는 `order.lines`라는 이름의 행 목록
  상태가 되고, 이벤트가 끝나면 비워지며 저장되지 않는다(Pure 008). HTTP로는 JSON 객체의 배열로 보내고, 행 필드 하나뿐인
  메시지는 행을 리스트의 저장 형식 바이트로 보내도 된다(대량 넣기). 출력은 행을 보내지 않는다.
- 이것으로 여러 행을 한꺼번에 바꾸는 트랜잭션(TPC-C의 주문처럼 행마다 재고를 줄이고 주문 행을 넣는 일)이 이벤트 하나다.
- 키 있는 리스트의 키를 범위로 묻는 `where`(`for l in lines where l.id >= start && l.id < start + 16`)와 집계, 표의 필터는 Pure가
  그 범위의 항목만 읽는다(키로 찾아서). 인덱스를 선언하지 않는다. 다만 집계와 걷기는 나머지 조건이 결함을 낼 수 없을 때만 그렇게
  하므로, 범위를 뜻으로 적으려면 **조각**을 쓴다: `lines[a .. b]`는 키가 a부터 b 앞까지인 항목들이다(`for l in lines[a .. b]`,
  `distinct(l.item for l in lines[a .. b] where stock[l.item].quantity < t)`). 조각 밖의 항목은 돌지 않으므로 그 조건이 결함을 낼
  수 있어도 된다. 수로 키를 둔 리스트에만 쓴다.

검증: 진단 9건(`for` 안의 `for`, 걷는 리스트에 넣기, `for` 안의 `reply`, 행을 보내는 출력, 핸들러 밖의 행, 값으로 쓴 행, 리스트가
아닌 것, 이미 선언된 항목 이름, 없는 레코드)과, 그룹 안의 행·`where`·걷는 항목을 읽는 집계·지우는 걷기를 쓴 앱을 Pure가
받아들이는 것. 실행은 Pure의 `walks`(모델과 대조한 주문·정리 3,000단계)와 `server_rows`(HTTP로 3만 행 한 요청, 무작위 주문
400개와 모델, 재시작)가 확인한다.

### let

```text
on order {
  let total = 0                          # 수
  for l in order.lines {
    let s = stock[l.item]                # 인덱스: 그 항목을 가리킨다
    s.left = s.left - l.qty
    total = total + l.qty * s.price      # let은 다시 줄 수 있다(같은 종류로)
  }
  revenue = revenue + total + sum(l.qty for l in order.lines where l.qty > total)
}
```

- **`let 이름 = 값`** 은 핸들러 안에서 수나 인덱스 하나를 블록 끝까지 이름 붙인다. 같은 종류의 값으로 다시 줄 수 있고(`total = …`),
  안쪽의 `for`와 집계에서 읽는다. 상태가 아니라 저장되지 않고 이벤트마다 새로 시작한다(Pure의 LOCAL_LOAD·LOCAL_STORE, 이벤트의 칸
  64개). 그래서 행마다 같은 항목을 다시 찾지 않고, 걷기 안에서 값을 모을 수 있다. text는 담지 않는다(text는 상태에 둔다).
- 이름은 선언된 이름, 블록 안의 다른 let, 걷는 항목과 겹칠 수 없다. 보기에는 let이 없다(보기는 식 하나다).

검증: 진단 9건과, 걷기 안의 합, 항목을 바꾸는 인덱스 let, 걷기 안의 let, 집계가 읽는 let, 가지마다의 let을 쓴 앱.

### 인자를 받는 보기

```text
view balance(aid: u32) = accounts[aid].balance
view richer(low: number) = count(a in accounts where a.balance > low)
view branch(b: u32, over: number) = each a in accounts where a.bid == b && a.balance > over {
  aid = a.aid
  share = a.balance / sum(x.balance for x in accounts where x.bid == b)
}
```

- **보기는 인자를 받는 함수다.** `view 이름(인자: 타입, …) = …`의 인자는 u32, i64, number, bool, bytes, text이고, 보기 안에서(표라면 그 필터와
  열, 그 안의 집계에서도) 이름으로 읽는다. HTTP로는 `GET /views/balance?aid=42`, 표는 `?b=1&over=0&from=20&count=20`이다. 등록한
  질의(`aup query`)도 인자를 받는다. 인자 이름은 상태나 다른 선언, 표의 행과 겹칠 수 없고 `from`·`count`는 쪽 나누기의 몫이다.
- 모듈에서는 `balance.args`라는 인자 입력이 보기를 가리키고, 보기는 메시지 핸들러가 필드를 읽듯 인자를 읽는다(Pure 008).
- 집계 안에서도 둘레의 인자와 메시지 필드를 읽는다. 그래서 핸들러의 `count(a in accounts where a.bid == pay.bid)`도 된다.

검증: 진단 8건(쪽 나누기 이름, 중복, 상태와 겹침, 표의 행과 겹침, 타입, 빈 괄호, 긴 이름, 보기 밖에서 쓴 인자)과 인자를 받는
보기들을 쓴 앱. HTTP는 Pure의 `server_arguments`가 모델과 대조한다.

구현은 C로 손수 쓴 재귀 하강 파서를 유지한다. 문법은 EBNF로 문서화하고, 이후 `aup fmt`와 언어 서버를 붙인다.

### 펼치기와 항목 전체

```text
record Card { title: text, lane = 0, note: text }
state cards: list of Card by title
output changed = message { title: text, lane: u32, note: text }
input remote = message like changed

on move.down {
  at.lane = at.lane + 1
  send changed { ..at }                  # 항목의 필드를 이름으로
}
on remote {
  if !cards.has(remote.title) { cards.add(Card { ..remote }) }
  else { cards[remote.title] = Card { ..remote } }   # 항목 전체: 모든 필드, 키는 그대로
}
```

- **`..source`** 는 레코드 값과 `send`의 중괄호 끝에 한 번 쓴다. 적지 않은 필드는 source의 같은 이름 필드에서 온다. source는
  항목(인덱스, `let`, `for`가 걷는 항목, `xs[키]`)이나 그 핸들러의 메시지다(그룹 안의 필드는 경로 이름으로 맞춘다). 레코드
  값에서 source에 없는 필드는 기본값이고, `send`에서는 지금처럼 모든 필드가 채워져야 한다(`send changed needs 'x'`). 적은
  필드가 먼저다: `send changed { note: "moved", ..at }`.
- **`xs[i] = Record { ... }`** 는 그 항목의 모든 필드를 바꾼다. 필드는 모두 주어야 한다(펼치기로 줘도 된다. 기본값으로 되돌리는
  일은 없다). 키 있는 리스트의 키는 바뀌지 않으므로, 값의 키가 항목의 키와 다르면 이벤트가 `limit`으로 실패하고 아무것도 바뀌지
  않는다.
- 둘 다 필드를 하나하나 적은 것과 같은 모듈을 낸다(검사가 바이트로 대조한다). 실행 비용이 따로 없다.
- 두 노드가 함께 쓰는 보드(Pure `docs/research/105-growing-a-shared-board.md`)에서 필드 하나를 더할 때 고칠 곳이 다섯에서 둘(레코드와
  출력)로 줄었다. 보내기와 적용은 필드를 이름으로 따라간다.

검증: 진단 7건(펼치기가 끝이 아님, 펼칠 수 없는 source, 핸들러 밖의 메시지, 그룹 안의 펼치기, 채워지지 않은 send 필드, 필드가
빠진 항목 전체, 레코드가 아닌 값). 펼치기와 항목 전체를 쓴 모듈과 필드를 적은 모듈이 바이트로 같다. `pure-run`에서 키가 다른 항목
전체 대입은 `limit`으로 실패하고 값을 바꾸지 않으며, 키가 같은 것은 바꾼다.

### 바이트, 비트, 수의 리스트 (Pure 027의 1단계)

Pure [027](/srv/git/pure.git)(`docs/design/027-computation-core.md`)의 1단계이고, 세상의 명세가 aup에서 처음 부딪치던 것 가운데 셋을
연다(Pure `docs/research/140-spec-suite.md`). `examples/sha256.aup`가 그것으로 쓴 SHA-256이다.

```text
input hash = message { data: bytes }
fn rotr(x, n) = (x >> n | x << (32 - n)) & 0xffffffff
on hash {
  let k: list of i64 = [0x428a2f98, 0x71374491, ...]        # 리스트의 처음 값
  let a, b = hash.data[0], hash.data[1]                      # 메시지 바이트의 그 자리(i64 0–255)
  a, b = b, a                                                # 값을 먼저 모두 계산하고 한 번에 준다
  let hex: list of i64
  hex.append(48 + (a >> 4))
  digest = text(bytes(hex))                                  # i64 리스트의 바이트
}
```

- **16진 리터럴** `0x…`는 어디서나 i64다.
- **비트 연산** `&`, `|`, `^`, `~`, `<<`, `>>`는 i64를 끝없는 2의 보수 위의 정수로 계산한다(파이썬의 정수와 같은 뜻).
  - `x << n`은 x·2^n이고, i64를 넘으면 이벤트가 `overflow`로 실패한다.
  - `x >> n`은 내림한 x / 2^n이다.
  - 시프트 수는 수나 i64이고, 0–63 밖이면 `limit`이다.
  - 우선순위는 파이썬과 같다: 비교 < `|` < `^` < `&` < 시프트 < `+ -` < `* / %` < 단항. 그래서 `e & f ^ ~e & g`는 `(e & f) ^ ((~e) & g)`다.
  - 수에 쓰면 진단이 변환을 권한다(`bit operations take i64s, not numbers: convert, as in i64(x)`).
- **자리의 바이트** `b[i]`는 바이트 값(메시지와 인자의 필드, fn의 매개변수, `bytes(t)`)의 그 자리 바이트를 i64로 준다.
  - 자리는 수나 i64다. 정수가 아니거나 값 밖이면 `limit`이다.
  - text는 바이트를 거쳐 읽는다(`bytes(t)[i]`). 글자로 읽기는 2단계다.
- **수의 리스트** `list of i64`(`number`, `bool`, `text`도)는 필드가 `value` 하나인 레코드의 리스트로 Pure에 간다.
  - `xs.append(5)`처럼 값 자체를 넣는다. `for x in xs`, 집계의 항목, `xs.first`, `xs.last`, `xs[i]`는 값이다.
  - `let xs: list of i64 = [1, 2, 3]`은 이벤트의 리스트를 그 값들로 시작한다. 리스트 리터럴은 거기에만 쓴다.
- **자리로 읽고 쓰기.** 키 없는 리스트의 `xs[i]`(레코드의 리스트면 `xs[i].field`)는 i번째 항목이다. `xs[i] = 값`은 그것을 바꾼다.
  그 자리에 항목이 없으면 `not_found`다(Pure의 INDEX_AT. 끝에서 멈추는 `xs.first + i`와 다르다).
- **`bytes(xs)`** 는 i64 리스트의 항목을 한 바이트씩 잇는다. 0–255 밖이면 `limit`이다.
- **한 번에 주기.**
  - `a, b = x, y`는 오른쪽을 모두 계산한 뒤 왼쪽에 한 번에 준다. 대상은 let과 수, bool, i64 상태다.
  - `let a, b = x, y`는 let 여럿을 만든다.
  - 줄은 쉼표 뒤에서 넘길 수 있다.

검증:
- 진단 12건: 수의 비트 연산, text나 수를 자리로 읽기, 잘못된 16진, i64 밖의 16진, 이름과 값의 수, 같은 이름 두 번, 한 번에 줄 수
  없는 상태, let에 text, 레코드 리스트의 `bytes`, 식 안의 리스트 리터럴.
- 받아들이는 꼴 하나: 위의 모든 꼴과 보기의 인자, fn 매개변수의 바이트.
- 무작위 비트 식 192개(파이썬의 정수와 i64의 넘침, 시프트 한도로) 사건 768개, 그중 실패 196개가 파이썬과 같다.
- 바이트와 리스트의 앱이 파이썬과 같다(`limit`, `not_found` 포함).
- `examples/sha256.aup`는 Pure의 검사가 FIPS 180-2의 예와 길이 0–129 등의 메시지로 `hashlib`와 대조한다(세 실행 길).

### 형식, 잇기와 자르기, 바이트와 text의 let, while, 단락 평가 (Pure 027의 2a)

`examples/json.aup`(JSON 텍스트인지 판정)과 다시 쓴 `examples/sha256.aup`가 이것으로 쓰였다.

```text
on check {
  let d = check.data                          # 바이트를 담는 let
  let out = ""                                # text를 담는 let
  let i = 0
  while i < d.bytes && d[i] != 0x2c {         # 왼쪽이 거짓이면 오른쪽은 계산하지 않는다
    out = out + text(d[i .. i + 1])           # 덧붙이기(제자리)
    i = i + 1
  }
  shown = "{i:5}|{h:08x}|{price:.2f}"         # 형식
}
```

- **형식** `{값:형식}`은 파이썬의 형식 미니 언어의 일부다: `[0][폭][.자릿수][d x X b o f]`.
  - `0`은 부호 뒤를 0으로 채운다. 폭은 255까지다.
  - `d x X b o`는 i64나 정수인 수를 그 진법으로 쓴다. 정수가 아닌 수면 이벤트가 `limit`으로 실패한다.
  - `f`는 수를 고정 자릿수로 쓴다(자릿수 0–30, 기본 6). −0은 0으로 쓴다.
  - 형식 없이 폭만 주면 지금의 글자를 왼쪽으로 채운다. bool은 폭만 받는다.
  - 파이썬의 `format()`과 같은 글자를 낸다(무작위 대조).
- **잇기** `a + b`는 text끼리, 바이트끼리 잇는다. 섞으면 진단이 나온다.
- **자르기** `b[a .. c]`는 a부터 c 앞까지의 바이트다.
  - text는 바이트 자리로 자르고, 글자 가운데를 자르면 `encoding`으로 실패한다.
  - 끝은 둘 다 수이거나 둘 다 i64이고, 정수가 아니거나 값 밖이면 `limit`이다.
- **`byte(n)`** 은 i64 0–255 하나의 바이트다.
- **바이트와 text의 let.**
  - `let s = …`, `s = …`로 쓰고, 핸들러마다 바이트 16개와 text 16개까지다.
  - `s = s + e`는 s를 복사하지 않고 덧붙인다(Pure의 BYTES_STORE 덧붙이기).
  - 담은 바이트는 이벤트의 메모리 몫에 센다. 한 번에 주기(`a, b = b, a`)는 수의 let과 상태에만 쓴다.
- **`while 조건 { }`** 은 0부터 2^53까지 세는 반복의 첫 줄에 `if !조건 { break }`를 둔 것이다. 끝나지 않는 반복은 실행량 예산이
  `work`로 끝낸다. `break`는 가장 안쪽의 `while`이나 `for`를 끝낸다.
- **`&&`와 `||`는 단락 평가다**(뜻이 바뀌었다).
  - 왼쪽이 답을 정하면 오른쪽은 계산하지 않는다. 그래서 `i < n && d[i] == x`처럼 범위를 먼저 볼 수 있다.
  - 식은 아무것도 바꾸지 않으므로, 전과 다른 것은 오른쪽이 실패할 수 있을 때뿐이다.
  - 오른쪽이 실패할 수 없으면(비교, 상태와 필드 읽기, 걷는 항목의 필드, 왼쪽이 이미 계산한 식) 컴파일러는 지금처럼 AND와 OR
    하나를 낸다. 그래서 Pure의 범위 찾기와 접기의 모양이 그대로다. 그 밖에서는 분기를 낸다.
  - Pure, aup, Vega의 소스 36개 가운데 모듈이 달라진 것은 이 단락 평가가 필요한 JSON 검사기 하나다. TPC-C의 필터는 오른쪽의 키 찾기를
    왼쪽이 이미 계산하므로 바이트까지 같다.

검증:
- 진단 14건: 형식, 폭, 자릿수, 형식과 타입, text의 `-`, 잇기의 타입, let의 타입, 한 번에 주기의 text, `while`의 bool, 리스트의
  자르기, 수의 자르기, let 수의 한도.
- 받아들이는 꼴 하나.
- 무작위 형식 144개, 사건 720개(그중 `limit` 171개)가 파이썬과 같다.
- 쉼표로 자르는 앱 하나가 67개의 입력에서 파이썬과 같다. 빈 입력도 단락 평가로 읽지 않는다.
- `examples/json.aup`는 Pure의 검사가 JSONTestSuite로 대조한다.

### 레코드 안의 레코드, 갈래, 리스트를 담은 필드, match (Pure 027의 2b)

`examples/json_tree.aup`(JSON 텍스트를 값의 트리로 읽고 너비 우선으로 돌아 다이제스트를 낸다)가 이것으로 쓰였다.

```text
variant Json {                         # 경우 하나를 갖는 값: 경우마다 값 하나(타입 하나) 또는 없음
  null
  num(number)
  str(text)
  arr(list of Json)                    # 리스트를 담은 경우: 갈래는 리스트를 거쳐 자기 자신을 담는다
  obj(list of Member)
}
record Member {
  name: text
  value: Json                          # 레코드 안의 갈래(레코드 안의 레코드도: at: Point)
}
on check {
  let kids: list of Json
  kids.append(Json.num(1))
  docs.append(Json.arr(kids))          # let의 리스트를 값으로(복사)
  docs.last.arr.append(Json.num(2))    # 항목의 필드에 담긴 리스트에 덧붙이기
  for d in docs {
    match d {
      num(x) => total = total + x
      arr(items) => { for y in items { ... } }
      _ => {}
    }
  }
}
```

- **펼치기.** 레코드나 갈래인 필드는 컴파일할 때 경로 이름의 필드로 펼쳐진다.
  - `at: Point` → `at.x`, `at.y`.
  - 갈래 필드는 그 이름의 필드가 경우 번호(i64)이고, 경우의 값이 `이름.경우`에 놓인다. 맨 위 갈래의 경우 번호는 `case`다.
  - 그래서 런타임에는 레코드 값이 따로 없다. 저장 상태도 경로 이름의 열로 그대로 저장된다.
  - 레코드가 리스트를 거치지 않고 자기 자신을 담으면 진단이 나온다. 펼친 필드는 Pure의 한도대로 16개까지다.
- **리스트를 담은 필드** `items: list of X`는 이벤트의 값이다. let의 리스트와 그 항목에만 둘 수 있다. 저장 상태와 메시지의 행은
  저장되는 리스트의 리스트가 생길 때(027의 3단계) 받는다.
  - 값으로 짓기: let의 리스트를 주면 복사(Pure의 LIST_VALUE), `[]`는 빈 것, 다른 항목의 리스트 필드는 그대로 나눠 가진다.
  - 덧붙이기: `x.items.append(…)`, `xs.last.items.append(…)`, `xs[i].items.append(…)`(APPEND_IN). 나눠 가진 리스트는 먼저
    제 것으로 복사되므로 다른 값은 바뀌지 않는다.
  - 읽기: `for y in x.items`, 접기, `x.items.count`·`.first`·`.last`·`[i]`. 이것들은 읽기 전용 레지스터에 묶여 읽힌다
    (LIST_BIND).
- **갈래의 경우를 짓기**: `Json.num(3)`, `Json.null`. 값이 있는 경우에는 값을 꼭 준다.
- **`match 항목 { 경우(이름) => 문장 또는 { 블록 } … _ => … }`**
  - 항목은 갈래의 리스트의 항목이거나, 레코드 안의 갈래 필드(`match m.value`)다.
  - 경우의 값은 이름으로 묶인다. 수와 text는 그 값, 리스트는 읽을 리스트, 레코드나 갈래는 그 필드(`p.x`, 안쪽 `match`)다.
  - 빠진 경우는 `_`가 없으면 진단이 나온다. 같은 경우를 두 번 쓰거나 `_` 뒤에 쓴 갈래도 진단이 나온다.
  - 갈래 필드를 그냥 읽으면 `match`하라는 진단이 나온다.
- **`xs[i]`** 는 키 없는 레코드 리스트의 i번째 항목(인덱스)이다.
- **`number(t)`** 는 JSON의 수로 쓴 text를 가장 가까운 수로 읽는다. 아니면 `limit`이다(Pure의 NUM_PARSE).

검증:
- 진단 16건: 자기 자신을 담는 레코드, 모르는 타입, 같은 경우, 값의 유무, 없는 경우, 빠진 경우, 두 번 쓴 경우, 갈래가 아닌
  match, 값 없는 경우의 이름, `_` 뒤의 갈래, 다른 레코드의 리스트, 저장 상태의 리스트 필드, 갈래 필드의 읽기, 16개를 넘는 펼침.
- 받아들이는 꼴 하나.
- 중첩 값의 앱이 기대값을 낸다. 같은 이벤트를 두 번 해도 같은 몫이 늘고, 실패한 이벤트는 아무것도 남기지 않는다.
- `examples/json_tree.aup`는 Pure의 검사가 JSONTestSuite와 무작위 문서 100개로 파이썬의 읽기와 대조한다.

### 스스로 부르는 fn, 블록, 여러 값, 값으로서의 레코드, 글자 (Pure 027의 2c)

`examples/json_tree.aup`가 이것으로 재귀 하강 파서와 깊이 우선 걷기로 다시 쓰였다.

```text
fn value(d, i) -> Json, number {      # 블록 본문의 fn. 스스로 부르면 결과의 타입을 적는다
  let items: list of Json              # let의 리스트는 부를 때마다 제 것이다
  let j = skip(d, i)
  if d[j] == 0x5b {
    let v, k = value(d, j + 1)         # 여러 값을 한 번에 받는다. v는 Json 값이다
    items.append(v)
    return Json.arr(items), k + 1      # 레코드나 갈래의 값을 돌려준다
  }
  ...
}
fn walk(x) {                           # 값을 주지 않는 fn: 부름은 문장이다
  match x {                            # 이름에 담긴 갈래의 값을 match한다
    arr(items) => { for y in items { walk(y) } }   # 항목은 값으로 넘어간다
    _ => {}
  }
}
on check {
  let root, end = value(bytes(text(check.data)), 0)
  walk(root)
  for ch in check.name { n = n + number(ch) }      # text의 글자(코드 포인트, i64)
}
```

- **fn이 Pure의 함수가 되는 때.** 블록 본문을 가졌거나, 부름의 고리가 자신을 지나면 Pure의 함수(CALL)로 돈다. 식 하나의 fn은
  지금처럼 인라인한다. 인자의 타입마다 한 벌을 만든다(단형화). 레코드 값을 받는 인라인 fn도 함수로 돈다.
- **결과.**
  - `return a, b`의 값들이 결과다. 값 없는 `return`과, 끝에 이르는 블록은 아무것도 주지 않는다.
  - 결과의 타입은 처음 컴파일된 `return`이 정한다. 그보다 먼저 자신을 부르면 `-> 타입, …`을 적어야 한다(진단).
  - 모든 `return`은 같은 타입을 준다. 결과가 있는 fn은 모든 길이 `return`으로 끝나야 한다.
  - 적은 결과의 타입은 `return`의 값이 바란다. 그래서 `-> i64`인 fn의 `return 1`은 i64다.
- **부름.** 식 안에서 쓰거나, 값을 주지 않으면 문장으로 쓴다(`walk(root)`). 여러 값은 `let a, b = f(x)`나 `a, b = f(x)`(let과 수,
  bool, i64 상태)로 받고, `return f(x)`로 그대로 넘긴다.
- **보기에서는 부르지 않는다.** 보기와 count·sum·max·min의 블록은 함수를 부르지 않는다(Pure가 그렇다). 핸들러와 함수가 부른다.
- **레코드의 값.**
  - `R { … }`, `V.case(x)`, `V.case`, 레코드 값을 주는 부름, 이름에 담긴 레코드 값은 그 필드들로 스택에 놓인다.
  - `let p = …`는 이름 하나에 레코드를 담는다. 필드는 `p.x`로 읽힌다(이름 `p.필드`의 let들).
  - 항목(`for y in ys`의 y, `m.value` 같은 레코드 필드의 경로)은 함수에 값으로 넘어간다. 함수는 제 리스트를 가지므로 항목의 자리가
    아니라 값을 받는다.
  - `match`는 이름에 담긴 갈래의 값도 받는다. 경우의 이름은 읽기 전용이다.
  - 리스트 값은 지역 칸에 담기고(Pure의 지역 칸 타입 4), `for y in items`가 레지스터에 묶어 걷는다.
- **도달하지 않는 코드.** `return`이나 `break` 뒤의 줄은 진단이 나온다. 모든 경우가 이름 붙은 `match`의 마지막 갈래는 묻지 않고
  탄다. `if`의 then이 돌아가거나 멈추면 else를 건너는 점프를 두지 않는다.
- **글자.**
  - `for ch in t`는 text의 글자를 코드 포인트(i64)로 돈다. t는 let, 매개변수, 상태, 레코드 값이나 메시지의 text 필드다. 돌기 전에
    제 이름에 담는다.
  - `char(n)`은 코드 포인트 하나의 text다. 서로게이트면 `encoding`, 0–0x10FFFF 밖이면 `limit`이다(Pure의 CHAR).

검증:
- 진단 13건: 결과를 모른 채 자신을 부름, 보기의 부름, 값 없이 끝날 수 있는 fn, 서로 다른 결과, 도달하지 않는 줄, 값을 주는 부름의
  문장, 여러 값을 이름 하나에, 이름과 값의 수, 모르는 타입, 커서 인자, 경우 이름에 대입, 갈래가 아닌 match, 접기 안의 부름.
- 받아들이는 꼴 둘.
- 함수의 앱이 파이썬과 같다. 무작위 트리 40개를 재귀로 짓고 합하고, 깊이와 잎 수를 여러 값으로 받는다. 무작위 text 60개의 글자 수,
  코드 포인트의 합, 넓은 글자 수, 그리고 `char`로 다시 지은 text다. 끝없는 재귀는 `memory`로 실패하고 아무것도 바꾸지 않는다.
- 모듈은 2b의 컴파일러와 바이트까지 같다(aup와 Pure의 aup 소스 49개). 다른 것은 `match`를 쓴 `json_tree.aup`뿐이다.
- 바뀐 진단 문구 셋: 레코드가 이제 값이라 `n.insert(at, T { … })`는 "expected text, not a record"다. let의 타입 목록에 리스트와
  레코드가 들었다. 자신을 부르는 식 fn은 이제 보기에서만 거절된다.

### 저장되는 중첩 (Pure 027의 3단계)

저장 상태와 transient 리스트의 레코드도 리스트를 담는다. 메시지의 행도 4a부터 담는다(행은 읽기만 한다: 행 안의 리스트에
덧붙이는 것은 거절한다).

```text
record Card {
  title: text
  tags: list of text
}
record Column {
  name: text
  cards: list of Card
}
state columns: list of Column          # 저장된다: 열, 그 카드, 그 태그
on add_card { columns[add_card.column].cards.append(Card { title: add_card.title, tags: [] }) }
on tag {
  let kept: list of Card
  let col = columns[tag.column]
  for c in col.cards { kept.append(c) }     # 같은 레코드의 항목은 그대로 덧붙는다
  kept[tag.card].tags.append(tag.tag)
  columns[tag.column].cards = kept          # 항목의 리스트 필드에 let의 리스트(값)
}
```

- **뜻은 2b와 같다.** 리스트는 값이다. 저장된 항목의 리스트 필드를 읽으면 이벤트의 리스트 값이 되고, 쓰면 그 값이 저장된다.
  `xs[i].items.append(…)`가 저장된 리스트의 항목에서도 된다.
- **새로 받는 꼴.** `xs[i].field = 리스트`(let의 리스트, `[]`, 리스트 값), 같은 레코드의 항목을 그대로 덧붙이기(`ys.append(y)`).
- **아직 안 되는 것.** `for c in xs[i].cards`(`let col = xs[i]`를 먼저), 접기 안에서 리스트 필드의 `.count`(레지스터를 묶어야 한다).

검증:
- 열과 카드와 태그의 앱(`BOARD_SOURCE`)을 무작위 조작 200걸음으로 모델과 대조한다. 메모리에서, 그리고 저장소에서 여덟 번 다시
  시작하며(그중 반은 묶음 저장) 돈다. 씨앗 여섯 개에서 모든 걸음의 실패, 상태, 리스트를 읽는 보기가 모델과 같다.

### 바이트를 담는 필드와 키별 문서 (Pure 027의 4b)

레코드 필드가 바이트를 담는다(`body: bytes`, 기본값은 `= "…"`의 바이트). 키별 문서(`bytes by key`, `message by key`)가 하던 일을
키 있는 리스트로 쓴다(`examples/documents.aup`).

```text
record Doc {
  id: text
  body: bytes
  revision: i64
}
state docs: list of Doc by id
on edit {
  if docs.has(edit.id) {
    let d = docs[edit.id]
    require d.revision == edit.base       # 보낸 쪽이 본 리비전이 아직 그 문서의 것이어야 한다
    require edit.start + edit.remove <= d.body.bytes
    docs[edit.id].body = d.body[0 .. edit.start] + edit.insert + d.body[edit.start + edit.remove .. d.body.bytes]
    docs[edit.id].revision = d.revision + 1
  } else {
    require edit.base == 0 && edit.start == 0 && edit.remove == 0
    docs.add(Doc { id: edit.id, body: edit.insert, revision: 1 })
  }
  reply edit.insert
}
```

- **뜻.** 바이트 필드는 text 필드처럼 읽고 쓰지만 UTF-8을 묻지 않는다. `x.body.bytes`가 길이다. text는 `bytes(t)`로만 그 자리에
  간다.
- **검증.** `documents_model`이 무작위 보기와 편집(묵은 리비전, 범위 밖, 아무 바이트, 한 번에 5,000바이트까지) 160걸음을 모델과
  대조한다. 메모리에서, 그리고 저장소에서 여섯 번 다시 시작하며(반은 묶음 저장) 돈다. 씨앗 넷에서 모든 걸음의 실패와 문서가 같다.
