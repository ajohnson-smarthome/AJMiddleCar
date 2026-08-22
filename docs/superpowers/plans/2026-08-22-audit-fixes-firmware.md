# Firmware Audit Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close every firmware-side finding of the 2026-08-22 link audit: session-lifecycle rules 1–4, the actuator-safety cluster, parser tightening, and the REST/robustness batch.

**Architecture:** Pure decisions stay in headers under `*_HOST_TEST` guards (the codebase's established seam); the one new module is `rt_glue.h`, which lifts rt_link.c's session side-effect *orderings* into pure functions over an effects table so they are host-tested the way `test_state.py` pins the same rules on the mock. Wire-visible behavior follows `docs/superpowers/specs/2026-08-22-audit-fix-decisions.md` exactly — the mock plan lands the identical rules.

**Tech Stack:** C11, plain `cc` host tests (`firmware/p4/test/Makefile`, assert-based, one binary per module), ESP-IDF 6.0.2 for the final device build, Python 3 for the contract generator.

**Spec:** `docs/superpowers/specs/2026-08-22-audit-fix-decisions.md`

## Global Constraints

- Work ONLY in the worktree `/Users/adamjohnson/VSCode/esp32-p4-car/.claude/worktrees/audit-fixes` (branch `audit-fixes`). All paths below are relative to it.
- Never hand-edit a generated file: `firmware/p4/main/cfg_table.inc`, `app/AJMiddleCar/Generated/CarAPI.swift`, `tools/mock_car/generated.py`, and the endpoints block of `docs/protocol.md` come from `contract/car-api.json` via `python3 tools/gen_contract.py`; `tools/check_contract.sh` fails on drift.
- Pure modules keep **zero ESP-IDF dependencies**; the IDF half of a header sits behind `#ifndef <MODULE>_HOST_TEST` (see `link.h`, `rt_link.h`, `ramp.h` for the pattern). Host tests are assert-based `main()`s compiled with `cc -I../main -Wall -Wextra -Werror -std=c11`.
- The shoot-through invariant: never both channels of a BTS7960 pair driven nonzero — `motors_plan` guarantees it in the targets, and after Task 6 the write path guarantees it at the chip.
- Wire/session semantics must land exactly as the decisions spec states (rules 1–9); the mock plan implements the same rules against the same names (`RT_SESSION_IDLE_MS` = schema `rt.session_idle_ms` = 10000). Do not improvise different constants or orderings.
- After every task: `make -C firmware/p4/test run` green, then `./tools/test-all.sh` green, then commit. Commit messages follow the repo's style (`fix(fw): …`, `feat(contract): …`, lowercase subject, body explains the why) and end with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`
- The firmware's REST-body changes (Tasks 10–12) intentionally diverge from the mock until the mock plan lands; `test-all.sh` stays green throughout because conformance runs against the mock's own (unchanged) behavior. Body-level conformance assertions arrive with the mock plan.

---

### Task 1: `session_idle_ms` joins the contract

**Files:**
- Modify: `contract/car-api.json` (rt section)
- Modify: `tools/gen_contract.py:106-109` (emit_c), `tools/gen_contract.py:144` (emit_swift)
- Modify: `tools/test_gen_contract.py` (TestSchema + the emitter tests)
- Regenerate: `firmware/p4/main/cfg_table.inc`, `app/AJMiddleCar/Generated/CarAPI.swift`, `tools/mock_car/generated.py` (via the generator, never by hand)

**Interfaces:**
- Consumes: nothing.
- Produces: `RT_SESSION_IDLE_MS` (C, value 10000), `CarContract.sessionIdleMs` (Swift), `RT["session_idle_ms"]` (Python) — Tasks 3–5 and the mock plan consume these.

- [ ] **Step 1: Write the failing tests**

In `tools/test_gen_contract.py`, add to `class TestSchema`, right after `test_rt_constants`:

```python
    def test_session_idle(self):
        rt = load()["rt"]
        self.assertEqual(rt["session_idle_ms"], 10000)
        # Mortality must be far outside the watchdog's world: a slow trip is a
        # trip, not a death.
        self.assertGreater(rt["session_idle_ms"], rt["watchdog_ms"] * 10)
```

Find the C-emitter test that asserts `#define RT_PORT 4210` (near line 154) and add one line to the same method:

```python
        self.assertIn("#define RT_SESSION_IDLE_MS 10000", out)
```

Find the Python-emitter test that asserts `self.ns["RT"]["port"] == 4210` (near line 224) and add:

```python
        self.assertEqual(self.ns["RT"]["session_idle_ms"], 10000)
```

If there is a Swift-emitter test class, add `self.assertIn("public static let sessionIdleMs = 10000", out)` to it; if there is none, skip — the Swift artifact is still covered by `check_contract.sh`.

- [ ] **Step 2: Run to verify they fail**

Run: `python3 tools/test_gen_contract.py 2>&1 | tail -5`
Expected: FAIL/ERROR with `KeyError: 'session_idle_ms'`.

- [ ] **Step 3: Implement**

In `contract/car-api.json`, after `"watchdog_ms": 300,` add:

```json
    "session_idle_ms": 10000,
```

In `tools/gen_contract.py`, in `emit_c` directly after the `RT_WATCHDOG_MS` line:

```python
    out.append(f'#define RT_SESSION_IDLE_MS {rt["session_idle_ms"]}')
```

In `emit_swift`, directly after the `watchdogMs` line:

```python
           f"    public static let sessionIdleMs = {rt['session_idle_ms']}",
```

Regenerate: `python3 tools/gen_contract.py`

- [ ] **Step 4: Verify**

Run: `python3 tools/test_gen_contract.py && bash tools/check_contract.sh && ./tools/test-all.sh 2>&1 | tail -3`
Expected: `contract: no drift` and `== all green ==`.

- [ ] **Step 5: Commit**

```bash
git add contract/car-api.json tools/gen_contract.py tools/test_gen_contract.py \
        firmware/p4/main/cfg_table.inc app/AJMiddleCar/Generated/CarAPI.swift \
        tools/mock_car/generated.py
git commit -m "feat(contract): sessions are mortal — session_idle_ms joins the schema

10 s of post-trip silence ends a session on every side. The constant is
generated so no implementation writes 10000 as a literal (audit rule 4).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: The parser speaks JSON — depth-1 keys, duplicate rejection, strict numbers

**Files:**
- Modify: `firmware/p4/main/control_proto.c` (value_of, parse_num, parse_u32, bye handling)
- Test: `firmware/p4/test/test_control_proto.c`

**Interfaces:**
- Consumes: `RT_KEY_*` from `cfg_table.inc` (unchanged).
- Produces: `control_parse_frame` — same signature `int control_parse_frame(const char *msg, size_t len, size_t max_len, control_frame_t *out)`, stricter acceptance. Tasks 3–5 and the mock plan rely on the rule-6 pinned outcomes.

- [ ] **Step 1: Write the failing tests**

In `firmware/p4/test/test_control_proto.c`, before the final `printf("test_control_proto: all passed\n");` line (run `grep -n "all passed" firmware/p4/test/test_control_proto.c` to find it), insert:

```c
    /* --- the audit's shared pinned frames (decisions spec, rule 6) ------------
       The mock pins these same bytes with these same outcomes; byte-identical
       datagrams drove the car and the mock differently before. */
    bad("{\"seq\":5,\"junk\":{\"t\":0.9},\"y\":0.5}"); /* nested t is not top-level t */
    bad("{\"seq\":7,\"t\":.5,\"y\":0}");               /* bare mantissa */
    bad("{\"seq\":8,\"t\":+1,\"y\":0}");               /* leading plus */
    bad("{\"seq\":9,\"t\":0.5,\"y\":0,\"t\":0.9}");    /* duplicate key: two commands */
    bad("{\"proto\":1.5,\"hello\":\"abcd1234\"}");     /* fractional proto, not v1 */
    bad("{\"seq\":01,\"t\":0,\"y\":0}");               /* leading zero */
    bad("{\"seq\":12,\"t\":0.5x,\"y\":0}");            /* trailing junk on a number */
    bad("{\"seq\":13,\"t\":0,\"y\":0,\"bye\":truex}"); /* trailing junk on a bool */
    ok("{\"seq\":10,\"t\":0.50,\"y\":-0.25}", 0.50f, -0.25f);
    control_frame_t pf = parse("{\"proto\":1,\"hello\":\"7f3a91c2\"}");
    assert(pf.has_proto && pf.proto == 1);
    control_frame_t p2 = parse("{\"proto\":2,\"hello\":\"7f3a91c2\"}");
    assert(p2.has_proto && p2.proto == 2);             /* integer future proto parses */
```

- [ ] **Step 2: Run to verify the new cases fail**

Run: `make -C firmware/p4/test test_control_proto && ./firmware/p4/test/test_control_proto`
Expected: FAIL on the first new `bad(...)` (the nested-`t` frame parses today).

- [ ] **Step 3: Implement**

In `firmware/p4/main/control_proto.c`:

**(a)** Replace the whole `value_of` function with a depth-tracking, duplicate-detecting scanner. It returns `0` found (sets `*val`/`*left`), `1` absent, `-1` duplicate:

```c
/* Point *val at the value for `key`, which must sit in key position at brace depth 1 —
   a key inside a nested object or inside a string value cannot match. Returns 0 and
   fills *val/*left when the key appears exactly once; 1 when it is absent; -1 when it
   appears twice, because a duplicated key is two instructions in one datagram and the
   car must not pick one (the mock's json.loads is made to refuse the same bytes). */
static int value_of(const char *msg, size_t len, const char *key,
                    const char **val, size_t *left) {
    size_t klen = strlen(key);
    int depth = 0;
    bool in_str = false;
    const char *found = NULL;
    for (size_t i = 0; i < len; i++) {
        char c = msg[i];
        if (in_str) {
            if (c == '\\') { i++; continue; }   /* skip the escaped character */
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '{') { depth++; continue; }
        if (c == '}') { depth--; continue; }
        if (c != '"') continue;
        /* A string opens at i. Is it `key`, in key position, at the top level? */
        if (depth == 1 && i + klen + 1 < len &&
            memcmp(msg + i + 1, key, klen) == 0 && msg[i + 1 + klen] == '"') {
            size_t q = i + klen + 2;
            while (q < len && is_ws(msg[q])) q++;
            if (q < len && msg[q] == ':') {
                q++;
                while (q < len && is_ws(msg[q])) q++;
                if (q >= len) return -1;         /* key with nothing after the colon */
                if (found != NULL) return -1;    /* second occurrence */
                found = msg + q;
                *left = len - q;
                i = q - 1;                       /* resume scanning at the value */
                continue;
            }
        }
        in_str = true;                           /* an ordinary string: skip its body */
    }
    if (found == NULL) return 1;
    *val = found;
    return 0;
}
```

**(b)** Add the two shape validators and the token-boundary check, right after `token()`:

```c
/* JSON's number grammar and nothing more. strtof/strtoull are laxer than the wire:
   they took ".5", "+1", "0123" and "1.5"-truncated-to-1, and the mock's json.loads
   refused every one of them — byte-identical datagrams drove the two cars apart. */
static bool token_ends(const char *p, size_t n, size_t k) {
    if (k >= n) return true;
    char c = p[k];
    return is_ws(c) || c == ',' || c == '}';
}

static bool json_int_shape(const char *s) {
    size_t i = (s[0] == '-') ? 1u : 0u;
    if (s[i] == '\0') return false;
    if (s[i] == '0' && s[i + 1] != '\0') return false;   /* leading zero */
    for (; s[i] != '\0'; i++) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

static bool json_num_shape(const char *s) {
    size_t i = (s[0] == '-') ? 1u : 0u;
    if (s[i] < '0' || s[i] > '9') return false;          /* "+1", ".5", "-", "" */
    if (s[i] == '0' && s[i + 1] != '\0' &&
        s[i + 1] != '.' && s[i + 1] != 'e' && s[i + 1] != 'E') return false;
    i++;
    while (s[i] >= '0' && s[i] <= '9') i++;
    if (s[i] == '.') {
        i++;
        if (s[i] < '0' || s[i] > '9') return false;
        while (s[i] >= '0' && s[i] <= '9') i++;
    }
    if (s[i] == 'e' || s[i] == 'E') {
        i++;
        if (s[i] == '+' || s[i] == '-') i++;
        if (s[i] < '0' || s[i] > '9') return false;
        while (s[i] >= '0' && s[i] <= '9') i++;
    }
    return s[i] == '\0';
}
```

**(c)** In `parse_num`, after the `token(...)` call succeeds, insert before the `strtof`:

```c
    if (!token_ends(p, n, (size_t)k) || !json_num_shape(tmp)) return -1;
```

**(d)** In `parse_u32`, after its `token(...)` call succeeds, insert before the `strtoull`:

```c
    /* A u32 (seq, proto) must be the whole value: parse_u32's digit-only token used to
       stop at the '.' of "1.5" and accept the truncation, so a fractional proto was
       adopted as version 1. */
    if (!token_ends(p, n, (size_t)k) || !json_int_shape(tmp)) return -1;
```

**(e)** In `control_parse_frame`, adapt every lookup to the new `value_of` contract. Replace the four single lookups and the bye/axis lookups so a `-1` (duplicate) fails the frame; pattern for each:

```c
    int r;
    r = value_of(msg, len, RT_KEY_PROTO, &v, &left);
    if (r < 0) return -1;
    if (r == 0) {
        if (parse_u32(v, left, &f.proto) != 0) return -1;
        f.has_proto = true;
    }
```

(same shape for `RT_KEY_SEQ`, `RT_KEY_HELLO`, `RT_KEY_BYE`; for the axes:)

```c
    const char *vt = NULL, *vy = NULL;
    size_t left_t = 0, left_y = 0;
    r = value_of(msg, len, RT_KEY_THROTTLE, &vt, &left_t);
    if (r < 0) return -1;
    int ry = value_of(msg, len, RT_KEY_YAW, &vy, &left_y);
    if (ry < 0) return -1;
    if (vt != NULL || vy != NULL) {
        if (vt == NULL || vy == NULL) return -1;
        if (parse_num(vt, left_t, &f.t) != 0) return -1;
        if (parse_num(vy, left_y, &f.y) != 0) return -1;
        f.has_ty = true;
    }
```

**(f)** In the bye branch, add the boundary check to the literal forms:

```c
        if (left >= 4 && memcmp(v, "true", 4) == 0 && token_ends(v, left, 4)) f.bye = true;
        else if (left >= 5 && memcmp(v, "false", 5) == 0 && token_ends(v, left, 5)) f.bye = false;
        else if (parse_num(v, left, &b) == 0) f.bye = (b != 0.0f);
        else return -1;
```

- [ ] **Step 4: Run the full C suite**

Run: `make -C firmware/p4/test run`
Expected: PASS. If a pre-existing `ok(...)`/`bad(...)` case now disagrees, read it against spec rule 6 before touching it — the existing whitespace-tolerant and key-order cases must keep passing; only the newly-illegal spellings may flip, and none of the existing cases use them.

- [ ] **Step 5: Commit**

```bash
git add firmware/p4/main/control_proto.c firmware/p4/test/test_control_proto.c
git commit -m "fix(fw): the control parser speaks JSON, not almost-JSON

Depth-1 key matching (a nested {\"t\":..} drove the car), duplicate keys
refused, and number tokens held to JSON grammar so \".5\", \"+1\", \"0123\"
and a proto of 1.5 are dropped instead of guessed at. The mock's json.loads
refused all of these already — byte-identical datagrams now mean the same
thing on both implementations (audit rules 5-6).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: Session rules, pure half — the gate survives a trip, dead sids, mortality

**Files:**
- Modify: `firmware/p4/main/rt_link.h` (rt_session_t helpers, classify, new dead-sid ring, new idle predicate)
- Modify: `firmware/p4/main/rt_link.c` (mechanical call-site updates only — glue rework is Task 5)
- Test: `firmware/p4/test/test_rt_session.c`

**Interfaces:**
- Consumes: `RT_SESSION_IDLE_MS` (Task 1), `CONTROL_SID_MAX`, `watchdog_stale`.
- Produces (Tasks 5 and test files rely on these exact signatures):
  - `void rt_session_adopt(rt_session_t *s, const char *sid, uint32_t now_ms)` — now stamps `last_feed_ms` so mortality has a start point.
  - `rt_action_t rt_session_classify(const rt_session_t *s, const rt_dead_sids_t *dead, bool from_owner, const control_frame_t *f)` — new second parameter, NULL allowed (no dead-sid rule applied).
  - `void rt_session_trip(rt_session_t *s)` — clears `armed` only; `have_seq`/`last_seq` survive.
  - `bool rt_session_idle(const rt_session_t *s, uint32_t now_ms)`.
  - `typedef struct { char sid[4][CONTROL_SID_MAX]; uint8_t next; } rt_dead_sids_t;` with `void rt_dead_note(rt_dead_sids_t *d, const char *sid)` and `bool rt_dead_known(const rt_dead_sids_t *d, const char *sid)`.

- [ ] **Step 1: Write the failing tests**

Rewrite the affected regions of `firmware/p4/test/test_rt_session.c`:

**(a)** Every `act()` call gains the dead-sid argument. Change the helper:

```c
static rt_action_t act(const rt_session_t *s, const rt_dead_sids_t *dead,
                       bool from_owner, const char *msg) {
    control_frame_t f = frame(msg);
    return rt_session_classify(s, dead, from_owner, &f);
}
```

and mechanically update every existing call site to pass `NULL` as the new second argument (e.g. `act(&s, NULL, false, HELLO_A)`).

**(b)** Every `rt_session_adopt(&s, SID)` call gains a time (use distinct values: `rt_session_adopt(&s, SID_A, 500);` etc.), and directly after the first adoption add:

```c
    assert(s.last_feed_ms == 500);   /* mortality counts from adoption, not from 0 */
```

**(c)** Replace the trip block's sequence-gate assertions (the lines asserting `!s.have_seq` after `rt_session_trip` and the `seq:3` re-acceptance) with:

```c
    /* The sequence gate SURVIVES the trip (audit rule 1). The old rationale — "silence
       already proved the stream dead, and a desynchronised gate would drop every
       genuine frame" — missed that a resuming same-session stream carries monotonically
       newer seqs and passes the kept gate anyway, while the cleared gate accepted one
       network-delayed pre-dropout duplicate as the resumed stream: it re-armed the
       watchdog, outranked and aborted a retreat in progress, and drove the car on
       stale stick values for the whole 300 ms grant. */
    assert(s.have_seq && s.last_seq == 500);
    assert(act(&s, NULL, true, "{\"seq\":3,\"t\":0.2,\"y\":0}") == RT_DROP);
    assert(act(&s, NULL, true, "{\"seq\":501,\"t\":0.2,\"y\":0}") == RT_COMMAND);
```

**(d)** Append before the final `printf`:

```c
    /* --- dead sids: a stale hello cannot evict a live driver (rule 3) -------- */
    rt_dead_sids_t dead = {0};
    rt_dead_note(&dead, "deadbee1");
    assert(rt_dead_known(&dead, "deadbee1"));
    assert(!rt_dead_known(&dead, "7f3a91c2"));
    rt_dead_note(&dead, "deadbee2");
    rt_dead_note(&dead, "deadbee3");
    rt_dead_note(&dead, "deadbee4");
    rt_dead_note(&dead, "deadbee5");                 /* capacity 4: the oldest falls out */
    assert(!rt_dead_known(&dead, "deadbee1"));
    assert(rt_dead_known(&dead, "deadbee5"));

    rt_session_t live = {0};
    rt_session_adopt(&live, "0b17ac55", 1000);
    rt_dead_note(&dead, "deadsid1");
    /* With a live session, a dead session's replayed hello is answered, not adopted. */
    assert(act(&live, &dead, false, "{\"proto\":1,\"hello\":\"deadsid1\"}") == RT_REPLY);
    /* A fresh sid still wins — last hello wins is the documented model. */
    assert(act(&live, &dead, false, HELLO_A) == RT_ADOPT);
    /* With NO live session a dead sid may re-adopt: there is nobody to protect, and
       refusing would wedge a client whose session idled out mid-handshake. */
    rt_session_t empty = {0};
    assert(act(&empty, &dead, false, "{\"proto\":1,\"hello\":\"deadsid1\"}") == RT_ADOPT);
    /* The live session's own repeat is a repeat, even if its sid is in the ring. */
    rt_dead_note(&dead, "0b17ac55");
    assert(act(&live, &dead, true, "{\"proto\":1,\"hello\":\"0b17ac55\"}") == RT_REPLY);

    /* --- mortality: a session that stops commanding eventually ends (rule 4) -- */
    rt_session_t m = {0};
    assert(!rt_session_idle(&m, 999999));            /* no session, nothing to end */
    rt_session_adopt(&m, "7f3a91c2", 2000);
    assert(!rt_session_idle(&m, 2000 + RT_SESSION_IDLE_MS));       /* exactly at: alive */
    assert(rt_session_idle(&m, 2000 + RT_SESSION_IDLE_MS + 1));    /* past: dead */
    rt_session_command(&m, 7, 5000);
    assert(!rt_session_idle(&m, 5000 + 100));        /* armed: the watchdog's world */
    assert(!rt_session_idle(&m, 5000 + RT_SESSION_IDLE_MS + 1));   /* armed ≠ idle */
    rt_session_trip(&m);
    assert(!rt_session_idle(&m, 5000 + RT_SESSION_IDLE_MS));
    assert(rt_session_idle(&m, 5000 + RT_SESSION_IDLE_MS + 1));
    rt_session_bye(&m);
    assert(!rt_session_idle(&m, 5000 + 2 * RT_SESSION_IDLE_MS));   /* bye'd ≠ idle */
```

- [ ] **Step 2: Run to verify it fails**

Run: `make -C firmware/p4/test test_rt_session 2>&1 | tail -5`
Expected: compile FAILURE (`rt_dead_sids_t` undeclared, wrong arity on `rt_session_classify`/`rt_session_adopt`).

- [ ] **Step 3: Implement in `rt_link.h`**

**(a)** After the `rt_session_t` typedef, add the ring:

```c
/* The sids of recently ended sessions — ended by a goodbye, by eviction, or by idling
 * out. A hello carrying one of these is answered but not re-adopted while a live
 * session exists: UDP duplicates a hello as happily as a command, and an un-gated
 * replay of a dead session's handshake evicted the live driver for the ~3 s the app
 * takes to notice and re-hello. Four is deep enough for every stale duplicate a real
 * network holds; the app never reuses a sid, so a collision is a replay by definition. */
#define RT_DEAD_SIDS 4
typedef struct {
    char    sid[RT_DEAD_SIDS][CONTROL_SID_MAX];
    uint8_t next;
} rt_dead_sids_t;

static inline void rt_dead_note(rt_dead_sids_t *d, const char *sid) {
    snprintf(d->sid[d->next], CONTROL_SID_MAX, "%s", sid);
    d->next = (uint8_t)((d->next + 1) % RT_DEAD_SIDS);
}

static inline bool rt_dead_known(const rt_dead_sids_t *d, const char *sid) {
    for (int i = 0; i < RT_DEAD_SIDS; i++) {
        if (d->sid[i][0] != '\0' && strcmp(d->sid[i], sid) == 0) return true;
    }
    return false;
}
```

**(b)** Change `rt_session_classify` — new parameter and one new rule, ordered after the live-repeat check:

```c
static inline rt_action_t rt_session_classify(const rt_session_t *s,
                                              const rt_dead_sids_t *dead,
                                              bool from_owner,
                                              const control_frame_t *f) {
    if (f->has_hello) {
        if (!f->has_proto || f->proto != RT_PROTO) return RT_REPLY;
        if (s->have_owner && from_owner && strcmp(s->sid, f->sid) == 0) return RT_REPLY;
        /* A dead session's replayed hello must not evict a live driver. With no live
           session there is nobody to protect, so the sid may return — refusing it
           would wedge a client whose session idled out mid-handshake. */
        if (s->have_owner && dead != NULL && rt_dead_known(dead, f->sid)) return RT_REPLY;
        return RT_ADOPT;   /* a different sid, or a different address: last hello wins */
    }
    if (!s->have_owner || !from_owner) return RT_DROP;   /* not our driver */
    /* Every app->car datagram except a hello carries seq — a goodbye included. One
       without it bypasses replay protection, so it is not acted on. The parser refuses
       these too; the rule is restated here because the ordering test below is
       meaningless without it, and this module does not get to assume its input was
       filtered. */
    if (!f->has_seq) return RT_DROP;
    /* Replay protection is what leaving TCP buys: a reordered or duplicated command
       costs one dropped datagram instead of blocking the queue behind a retransmission. */
    if (s->have_seq && !control_seq_newer(f->seq, s->last_seq)) return RT_DROP;
    if (f->bye)     return RT_BYE;
    if (f->has_ty)  return RT_COMMAND;
    return RT_DROP;
}
```

**(c)** `rt_session_adopt` gains the clock (update its comment's last line too):

```c
static inline void rt_session_adopt(rt_session_t *s, const char *sid, uint32_t now_ms) {
    s->have_owner   = true;
    s->have_seq     = false;
    s->armed        = false;
    s->last_feed_ms = now_ms;   /* mortality (rt_session_idle) counts from here */
    snprintf(s->sid, sizeof(s->sid), "%s", sid);
}
```

**(d)** `rt_session_trip` keeps the gate. Replace function and comment:

```c
/* The watchdog tripped. Ownership of the *channel* is deliberately kept: a stream that
 * resumes after a dropout is the same session. The sequence gate is ALSO kept — a
 * resuming stream carries monotonically newer seqs and passes it, while clearing it
 * accepted one network-delayed pre-dropout duplicate as the resumed stream: re-armed
 * watchdog, aborted retreat, stale stick values held for a whole grant. (The old text
 * here argued the opposite from a desync that cannot happen to a well-behaved client;
 * a session whose counter really is broken ends at RT_SESSION_IDLE_MS.) */
static inline void rt_session_trip(rt_session_t *s) {
    s->armed = false;
}
```

**(e)** Add the mortality predicate after `rt_session_lost`:

```c
/* Pure: has an unarmed session gone without an accepted command for so long that it is
 * dead rather than merely quiet? Armed sessions belong to the watchdog; this begins
 * where the trip ends, and it is what stops the car pushing telemetry to a vanished
 * address forever — and what bounds the window in which a stale datagram could ever
 * find an owner to impersonate. */
static inline bool rt_session_idle(const rt_session_t *s, uint32_t now_ms) {
    return s->have_owner && !s->armed &&
           watchdog_stale(s->last_feed_ms, now_ms, RT_SESSION_IDLE_MS);
}
```

- [ ] **Step 4: Mechanical call-site updates in `rt_link.c`**

Only what compiles — the behavioral rework is Task 5:
- `adopt()`: `rt_session_adopt(&s_ses, sid, now_ms());`
- `on_datagram()`: add a file-scope `static rt_dead_sids_t s_dead;` next to `s_ses`, and pass it: `switch (rt_session_classify(&s_ses, &s_dead, s_ses.have_owner && same_peer(&s_owner, from), &f)) {`

- [ ] **Step 5: Run the suite**

Run: `make -C firmware/p4/test run`
Expected: PASS, all binaries.

- [ ] **Step 6: Commit**

```bash
git add firmware/p4/main/rt_link.h firmware/p4/main/rt_link.c firmware/p4/test/test_rt_session.c
git commit -m "fix(fw): the seq gate survives a trip; dead sids; sessions are mortal

Rule 1: a post-trip gate reset accepted one delayed pre-dropout duplicate as
the resumed stream — it re-armed the watchdog, aborted the retreat and drove
on stale stick values. Rule 3: a replayed hello from a dead session evicted
the live driver for ~3 s. Rule 4: a vanished owner kept telemetry (and the
stale-frame window) alive forever; now a session ends after RT_SESSION_IDLE_MS
without an accepted command. Pure half + tests; the task-side wiring is the
glue seam commit.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Arbiter timing and checked releases

**Files:**
- Modify: `firmware/p4/main/link.h` (LINK_TICK_MS, LINK_HOLD_RT_MS, link_release_must decl)
- Modify: `firmware/p4/main/link.c` (TICK_MS → LINK_TICK_MS, link_release_must impl)
- Modify: `firmware/p4/main/ota_api.c`, `firmware/p4/main/calib_api.c`, `firmware/p4/main/car.c`, `firmware/p4/main/main.c`, `firmware/p4/main/recovery.c` (release call sites)
- Test: `firmware/p4/test/test_link.c`

**Interfaces:**
- Consumes: `RT_WATCHDOG_MS` (generated).
- Produces: `LINK_TICK_MS` (20u, pure section of link.h), `LINK_HOLD_RT_MS == RT_WATCHDOG_MS + LINK_TICK_MS`, `bool link_release_must(link_src_t src)` (IDF section) — Task 5's effects table and every API handler use it.

- [ ] **Step 1: Write the failing test**

In `firmware/p4/test/test_link.c`, inside `main()` after `ctl_vocabulary();`, add:

```c
    /* The RT grant must outlive the watchdog deadline by one actuator tick: with the
       two equal, the grant's >= lapsed the target to zero up to a tick before the
       trip's > declared the loss, so every trip began from motors already at rest —
       and a frame arriving exactly on the deadline dipped the duty with no trip at
       all. link.h's own comment claimed this ordering could not happen. */
    assert(LINK_HOLD_RT_MS == (uint32_t)RT_WATCHDOG_MS + LINK_TICK_MS);
```

- [ ] **Step 2: Run to verify it fails**

Run: `make -C firmware/p4/test test_link 2>&1 | tail -3`
Expected: compile FAILURE — `LINK_HOLD_RT_MS`/`LINK_TICK_MS` are behind `#ifndef LINK_HOST_TEST` today (and the hold equals `RT_WATCHDOG_MS`).

- [ ] **Step 3: Implement**

In `firmware/p4/main/link.h`: delete the `LINK_HOLD_RT_MS`/`LINK_HOLD_CALIB_MS` block from the `#ifndef LINK_HOST_TEST` section and place this immediately BEFORE `#ifndef LINK_HOST_TEST` (pure constants may live outside the guard):

```c
/* The actuator task's beat, public because the RT hold is defined against it. */
#define LINK_TICK_MS 20u

/* How long each source's grant holds without being refreshed. The RT hold is one
 * actuator tick PAST the control watchdog's deadline, and necessarily so: the trip
 * must be declared before the grant lapses, or the car coasts to a stop before the
 * loss is noticed and the retreat starts from rest instead of from the path it was
 * on. rt_link checks silence on a beat of its own, so one tick of slack covers the
 * scheduling gap; the RT_COMMAND_HZ stream refreshes the grant far inside it. */
#define LINK_HOLD_RT_MS     ((uint32_t)RT_WATCHDOG_MS + LINK_TICK_MS)
#define LINK_HOLD_CALIB_MS  600u   /* one identification pulse */
```

In the IDF section of `link.h`, after `link_release`'s declaration add:

```c
/* link_release, insisted upon: one retry a tick later, then a loud log. A false
 * return leaves `src`'s grant standing — for a sticky top-rank source (SAFE after a
 * goodbye) that is an actuator nothing else can ever take, so no caller may drop the
 * result on the floor. */
bool link_release_must(link_src_t src);
```

In `firmware/p4/main/link.c`: delete `#define TICK_MS 20` and replace both uses (`vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_MS))`, `xSemaphoreTake(s_lock, pdMS_TO_TICKS(TICK_MS))`) with `LINK_TICK_MS`; the `ramp_max_up_per_tick(ramp_get_ms(), TICK_MS)` use becomes `LINK_TICK_MS` too. Then add after `link_release`:

```c
bool link_release_must(link_src_t src) {
    if (link_release(src)) return true;
    vTaskDelay(1);   /* the lock is held across a memcpy, never across a wait */
    if (link_release(src)) return true;
    ESP_LOGE(TAG, "%s could not release the actuator — the grant is stuck",
             link_src_name(src));
    return false;
}
```

Sweep the call sites — every `link_release(` outside link.c becomes `link_release_must(`:
- `ota_api.c`: all five failure-path releases plus none on success (grep `link_release` — five sites).
- `calib_api.c`: the post-pulse `link_release(LINK_SRC_CALIB)`.
- `car.c` (`car_init`): `link_release(LINK_SRC_SAFE)`.
- `main.c` (console `mix 0 0`): `link_release(LINK_SRC_CONSOLE)`.
- `recovery.c`: both `link_release(LINK_SRC_RECOVER)` sites in `recovery_on_link_lost` and the one at the end of `retreat_task`.
(rt_link.c's sites are rewired in Task 5.)

- [ ] **Step 4: Run the suite**

Run: `make -C firmware/p4/test run`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add firmware/p4/main/link.h firmware/p4/main/link.c firmware/p4/main/ota_api.c \
        firmware/p4/main/calib_api.c firmware/p4/main/car.c firmware/p4/main/main.c \
        firmware/p4/main/recovery.c firmware/p4/test/test_link.c
git commit -m "fix(fw): the trip precedes the lapse, and releases are checked

The RT grant now holds RT_WATCHDOG_MS plus one actuator tick, so the loss is
declared before the target falls — link.h promised that ordering and the >=/>
pair broke it on every trip. link_release_must retries once and logs, because
a silently failed release of sticky SAFE left an actuator nothing could take.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: The glue seam — session side effects become host-tested

**Files:**
- Create: `firmware/p4/main/rt_glue.h`
- Modify: `firmware/p4/main/rt_link.c` (adopt/on_bye/check_silence rewired; new check_idle)
- Test: `firmware/p4/test/test_rt_glue.c` (new), `firmware/p4/test/Makefile`

**Interfaces:**
- Consumes: `rt_session_*`, `rt_dead_*`, `rt_session_idle` (Task 3), `link_src_t` (link.h pure half), `link_release_must` (Task 4, via the firmware's effects table).
- Produces:
  - `rt_effects_t` — `{ void *ctx; bool (*stop_safe)(void*); bool (*release_safe)(void*); bool (*release_rt)(void*); void (*forget)(void*); void (*on_link_lost)(void*); link_src_t (*owner)(void*); }`
  - `bool rt_glue_adopt(rt_session_t*, rt_dead_sids_t*, const char *sid, uint32_t now, const rt_effects_t*)`
  - `rt_bye_result_t rt_glue_bye(rt_session_t*, rt_dead_sids_t*, const rt_effects_t*)` with `RT_BYE_PLAIN | RT_BYE_UNDER_STICKY | RT_BYE_STOP_REFUSED`
  - `bool rt_glue_silence(rt_session_t*, uint32_t now, const rt_effects_t*)` (true = tripped)
  - `bool rt_glue_idle(rt_session_t*, rt_dead_sids_t*, uint32_t now, const rt_effects_t*)` (true = session ended)

- [ ] **Step 1: Write the failing test**

Create `firmware/p4/test/test_rt_glue.c`:

```c
/* The session lifecycle's SIDE EFFECTS, in order. test_rt_session pins what the flag
 * machine decides; this pins what the task then does to the world — the orderings the
 * cutover plan specifies, which had no host test while test_state.py pinned the same
 * rules on the mock. The recorder is the world: every effect appends its name. */
#define RT_LINK_HOST_TEST
#define LINK_HOST_TEST
#include "../main/rt_glue.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char       log[8][16];
    int        n;
    bool       stop_ok, release_ok;
    link_src_t owner;
} rec_t;

static void note(rec_t *r, const char *what) {
    snprintf(r->log[r->n], sizeof(r->log[0]), "%s", what);
    if (r->n < 7) r->n++;
}
static bool fx_stop(void *c)      { note(c, "stop");      return ((rec_t *)c)->stop_ok; }
static bool fx_rel_safe(void *c)  { note(c, "rel_safe");  return ((rec_t *)c)->release_ok; }
static bool fx_rel_rt(void *c)    { note(c, "rel_rt");    return true; }
static void fx_forget(void *c)    { note(c, "forget"); }
static void fx_lost(void *c)      { note(c, "lost"); }
static link_src_t fx_owner(void *c) { return ((rec_t *)c)->owner; }

static rec_t R;
static const rt_effects_t FX = { &R, fx_stop, fx_rel_safe, fx_rel_rt,
                                 fx_forget, fx_lost, fx_owner };

static void reset(link_src_t owner) {
    memset(&R, 0, sizeof(R));
    R.stop_ok = R.release_ok = true;
    R.owner = owner;
}
static void expect(int i, const char *what) {
    if (i >= R.n || strcmp(R.log[i], what) != 0) {
        printf("FAIL effect[%d] = '%s', want '%s'\n", i, i < R.n ? R.log[i] : "(none)", what);
        assert(0);
    }
}

int main(void) {
    rt_session_t   s;
    rt_dead_sids_t dead;

    /* --- adopt: evicted sid recorded, SAFE released, path forgotten, then adopt --- */
    memset(&s, 0, sizeof(s)); memset(&dead, 0, sizeof(dead));
    reset(LINK_SRC_NONE);
    rt_session_adopt(&s, "11111111", 100);
    rt_glue_adopt(&s, &dead, "22222222", 200, &FX);
    expect(0, "rel_safe"); expect(1, "forget"); assert(R.n == 2);
    assert(rt_dead_known(&dead, "11111111"));      /* the evicted session is dead */
    assert(!rt_dead_known(&dead, "22222222"));
    assert(s.have_owner && strcmp(s.sid, "22222222") == 0 && s.last_feed_ms == 200);

    /* --- plain goodbye: stop, forget, release, session over, sid dead ------------ */
    reset(LINK_SRC_RT);
    assert(rt_glue_bye(&s, &dead, &FX) == RT_BYE_PLAIN);
    expect(0, "stop"); expect(1, "forget"); expect(2, "rel_safe"); assert(R.n == 3);
    assert(!s.have_owner && !s.armed && !s.have_seq);
    assert(rt_dead_known(&dead, "22222222"));

    /* --- goodbye during an OTA: the sticky hold is NOT grabbed or released -------
       Grabbing SAFE over OTA destroyed "nothing commands the motors during a flash":
       SAFE outranked OTA, the release then left owner NONE, and anything could drive
       for the rest of the write — with esp_restart() landing on a moving car. */
    memset(&s, 0, sizeof(s));
    rt_session_adopt(&s, "33333333", 300);
    reset(LINK_SRC_OTA);
    assert(rt_glue_bye(&s, &dead, &FX) == RT_BYE_UNDER_STICKY);
    expect(0, "forget"); assert(R.n == 1);         /* no stop, no release */
    assert(!s.have_owner);
    assert(rt_dead_known(&dead, "33333333"));

    /* --- goodbye during a wizard pulse: same rule, CALIB self-terminates ---------- */
    memset(&s, 0, sizeof(s));
    rt_session_adopt(&s, "44444444", 400);
    reset(LINK_SRC_CALIB);
    assert(rt_glue_bye(&s, &dead, &FX) == RT_BYE_UNDER_STICKY);
    expect(0, "forget"); assert(R.n == 1);

    /* --- goodbye whose stop is refused is reported, not swallowed ---------------- */
    memset(&s, 0, sizeof(s));
    rt_session_adopt(&s, "55555555", 500);
    reset(LINK_SRC_RT);
    R.stop_ok = false;
    assert(rt_glue_bye(&s, &dead, &FX) == RT_BYE_STOP_REFUSED);
    expect(0, "stop"); expect(1, "forget"); expect(2, "rel_safe");

    /* --- silence: revoke the dead grant, then recovery, then disarm-only trip ---- */
    memset(&s, 0, sizeof(s));
    rt_session_adopt(&s, "66666666", 1000);
    rt_session_command(&s, 41, 1000);
    reset(LINK_SRC_RT);
    assert(!rt_glue_silence(&s, 1000 + RT_WATCHDOG_MS, &FX));      /* in time */
    assert(R.n == 0);
    assert(rt_glue_silence(&s, 1000 + RT_WATCHDOG_MS + 1, &FX));
    expect(0, "rel_rt"); expect(1, "lost"); assert(R.n == 2);
    assert(s.have_owner && !s.armed);
    assert(s.have_seq && s.last_seq == 41);        /* rule 1: the gate survived */

    /* --- mortality: the tripped session ends, its path and sid go with it -------- */
    reset(LINK_SRC_NONE);
    assert(!rt_glue_idle(&s, &dead, 1000 + RT_SESSION_IDLE_MS, &FX));
    assert(R.n == 0);
    assert(rt_glue_idle(&s, &dead, 1001 + RT_SESSION_IDLE_MS, &FX));
    expect(0, "forget"); assert(R.n == 1);
    assert(!s.have_owner && !s.have_seq);
    assert(rt_dead_known(&dead, "66666666"));

    printf("test_rt_glue: all passed\n");
    return 0;
}
```

Add to `firmware/p4/test/Makefile`: `test_rt_glue` in the `all:` list and the `run:` chain, plus:

```make
# The lifecycle's side effects over a recording effects table — the impure orderings
# rt_link.c executes, pinned the way test_state.py pins them on the mock.
test_rt_glue: test_rt_glue.c ../main/rt_glue.h ../main/rt_link.h ../main/link.h
	$(CC) $(CFLAGS) -o $@ test_rt_glue.c $(LDLIBS)
```

- [ ] **Step 2: Run to verify it fails**

Run: `make -C firmware/p4/test test_rt_glue 2>&1 | tail -3`
Expected: compile FAILURE — `rt_glue.h` does not exist.

- [ ] **Step 3: Create `firmware/p4/main/rt_glue.h`**

```c
#ifndef RT_GLUE_H
#define RT_GLUE_H

#include "rt_link.h"
#include "link.h"

/* The session lifecycle's side effects, in the order the cutover plan specifies, over
 * an effects table instead of the live modules. rt_link.c supplies the real table; the
 * host test supplies a recorder. This is the seam the audit found missing: adopt,
 * goodbye and the trip each order calls into link/recovery/car, the mock pins those
 * orderings in test_state.py, and the firmware's copies had no test at all — so a
 * reorder (release SAFE before forgetting the path, say) shipped silently. */

typedef struct {
    void *ctx;                             /* the recorder in tests; NULL in firmware */
    bool (*stop_safe)(void *ctx);          /* car_stop(LINK_SRC_SAFE) */
    bool (*release_safe)(void *ctx);       /* link_release_must(LINK_SRC_SAFE) */
    bool (*release_rt)(void *ctx);         /* link_release_must(LINK_SRC_RT) */
    void (*forget)(void *ctx);             /* recovery_forget() */
    void (*on_link_lost)(void *ctx);       /* recovery_on_link_lost() */
    link_src_t (*owner)(void *ctx);        /* link_owner() */
} rt_effects_t;

/* Adopt `sid`: the evicted session's sid (if any, and different) is recorded dead, a
 * SAFE left by a previous goodbye is released, the previous driver's path is thrown
 * away, and only then does the session change hands. Returns the release's verdict so
 * the caller can log; the effects table's release already retries and logs itself. */
static inline bool rt_glue_adopt(rt_session_t *s, rt_dead_sids_t *dead,
                                 const char *sid, uint32_t now,
                                 const rt_effects_t *fx) {
    if (s->have_owner && strcmp(s->sid, sid) != 0) rt_dead_note(dead, s->sid);
    bool released = fx->release_safe(fx->ctx);
    fx->forget(fx->ctx);
    rt_session_adopt(s, sid, now);
    return released;
}

typedef enum {
    RT_BYE_PLAIN,          /* stopped, forgotten, released — the common case */
    RT_BYE_UNDER_STICKY,   /* OTA or CALIB holds the actuator: hands off it */
    RT_BYE_STOP_REFUSED,   /* the SAFE stop did not land; the grant lapse still stops */
} rt_bye_result_t;

/* A goodbye. When OTA or the wizard's pulse holds the actuator, the SAFE grab-and-
 * release is skipped entirely: the motors are already stopped (OTA) or deliberately
 * moving (CALIB), and grabbing SAFE over a sticky OTA destroyed "nothing may command
 * the motors during a flash" — SAFE outranked it, the release left owner NONE, and the
 * end-of-flash esp_restart() could land on a car someone had started driving again.
 * Clearing the breadcrumbs is what actually suppresses the retreat, and that happens
 * in every case; so does ending the session and recording its sid dead. */
static inline rt_bye_result_t rt_glue_bye(rt_session_t *s, rt_dead_sids_t *dead,
                                          const rt_effects_t *fx) {
    link_src_t o = fx->owner(fx->ctx);
    rt_bye_result_t r;
    if (o == LINK_SRC_OTA || o == LINK_SRC_CALIB) {
        r = RT_BYE_UNDER_STICKY;
    } else {
        r = fx->stop_safe(fx->ctx) ? RT_BYE_PLAIN : RT_BYE_STOP_REFUSED;
    }
    fx->forget(fx->ctx);
    if (r != RT_BYE_UNDER_STICKY) fx->release_safe(fx->ctx);
    rt_dead_note(dead, s->sid);
    rt_session_bye(s);
    return r;
}

/* The silence check: on a trip, revoke the dead stream's grant explicitly (waiting for
 * it to lapse at this same instant would refuse the retreat's first step), hand off to
 * recovery, and disarm. The sequence gate survives — see rt_session_trip. */
static inline bool rt_glue_silence(rt_session_t *s, uint32_t now, const rt_effects_t *fx) {
    if (!rt_session_lost(s, now)) return false;
    fx->release_rt(fx->ctx);
    fx->on_link_lost(fx->ctx);
    rt_session_trip(s);
    return true;
}

/* Mortality: a session that has not commanded for RT_SESSION_IDLE_MS ends. Its path
 * goes with it (a retreat must never retrace a dead driver's drive), its sid is
 * recorded dead, and the telemetry push stops because have_owner is what gates it.
 * The resuming client says hello again; the app already does after its 3 s stall. */
static inline bool rt_glue_idle(rt_session_t *s, rt_dead_sids_t *dead, uint32_t now,
                                const rt_effects_t *fx) {
    if (!rt_session_idle(s, now)) return false;
    fx->forget(fx->ctx);
    rt_dead_note(dead, s->sid);
    s->have_owner = false;
    s->have_seq   = false;
    return true;
}

#endif /* RT_GLUE_H */
```

- [ ] **Step 4: Run the glue test**

Run: `make -C firmware/p4/test test_rt_glue && ./firmware/p4/test/test_rt_glue`
Expected: `test_rt_glue: all passed`.

- [ ] **Step 5: Rewire `rt_link.c`**

- Add `#include "rt_glue.h"` (keep the existing includes; `link.h` is already there).
- Add after `static rt_session_t s_ses;` (s_dead exists from Task 3):

```c
/* The real effects table — every entry a one-line adapter, so the orderings above it
   are exactly the host-tested ones in rt_glue.h. */
static bool fx_stop_safe(void *c)    { (void)c; return car_stop(LINK_SRC_SAFE); }
static bool fx_release_safe(void *c) { (void)c; return link_release_must(LINK_SRC_SAFE); }
static bool fx_release_rt(void *c)   { (void)c; return link_release_must(LINK_SRC_RT); }
static void fx_forget(void *c)       { (void)c; recovery_forget(); }
static void fx_on_lost(void *c)      { (void)c; recovery_on_link_lost(); }
static link_src_t fx_owner(void *c)  { (void)c; return link_owner(); }
static const rt_effects_t FX = { NULL, fx_stop_safe, fx_release_safe, fx_release_rt,
                                 fx_forget, fx_on_lost, fx_owner };
```

- `adopt()` becomes:

```c
static void adopt(const struct sockaddr_in *from, const char *sid) {
    s_owner = *from;
    /* Ordering and rationale live in rt_glue_adopt, where they are host-tested. */
    if (!rt_glue_adopt(&s_ses, &s_dead, sid, now_ms(), &FX)) {
        ESP_LOGE(TAG, "adopt could not release a leftover SAFE grant");
    }
    log_peer("session adopted from", from);
}
```

- `on_bye()` becomes:

```c
static void on_bye(void) {
    switch (rt_glue_bye(&s_ses, &s_dead, &FX)) {
    case RT_BYE_PLAIN:
        ESP_LOGI(TAG, "goodbye — stopping, and not retreating");
        break;
    case RT_BYE_UNDER_STICKY:
        /* OTA or the wizard owns the motors; the goodbye ends the session and the
           breadcrumbs, and keeps its hands off the sticky grant — grabbing SAFE over
           a flash re-opened the actuator for the rest of the write. */
        ESP_LOGI(TAG, "goodbye during %s — session over, actuator untouched",
                 link_src_name(link_owner()));
        break;
    case RT_BYE_STOP_REFUSED:
        ESP_LOGE(TAG, "goodbye stop was not applied — %s holds the actuator",
                 link_src_name(link_owner()));
        break;
    }
}
```

- `check_silence()` becomes:

```c
static void check_silence(void) {
    if (!rt_glue_silence(&s_ses, now_ms(), &FX)) return;
    s_trips++;
    ESP_LOGW(TAG, "no control frame for >%dms — the driver is gone", RT_WATCHDOG_MS);
}
```

- Add `check_idle()` and call it from the task loop right after `check_silence();`:

```c
static void check_idle(void) {
    if (!rt_glue_idle(&s_ses, &s_dead, now_ms(), &FX)) return;
    ESP_LOGI(TAG, "session idle for >%dms — over; the next driver says hello",
             RT_SESSION_IDLE_MS);
}
```

- [ ] **Step 6: Run everything**

Run: `make -C firmware/p4/test run && ./tools/test-all.sh 2>&1 | tail -3`
Expected: all green.

- [ ] **Step 7: Commit**

```bash
git add firmware/p4/main/rt_glue.h firmware/p4/main/rt_link.c \
        firmware/p4/test/test_rt_glue.c firmware/p4/test/Makefile
git commit -m "fix(fw): a goodbye keeps its hands off a sticky hold — and the glue is tested

rt_glue.h lifts adopt/goodbye/trip/mortality side-effect orderings into pure
functions over an effects table, host-tested with a recorder the way the mock
pins the same rules. The behavioral fix rides in it: a bye during an OTA or a
wizard pulse no longer grabs SAFE over the sticky grant and release it to
NONE — which left the motors commandable for the rest of a flash, with
esp_restart() at the end of it (audit's top firmware finding).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: Two-pass PCA9685 writes — falls land before rises

**Files:**
- Modify: `firmware/p4/main/link.h` (link_plan_writes, link_rise_safe — pure)
- Modify: `firmware/p4/main/link.c` (link_task write loop)
- Test: `firmware/p4/test/test_link.c`; touch `firmware/p4/test/test_rt_glue.c` (one `#define RAMP_HOST_TEST` line)

**Interfaces:**
- Consumes: `ramp_step`, `ramp_max_up_per_tick` (ramp.h pure half).
- Produces: `uint8_t link_plan_writes(const uint16_t cur[8], const uint16_t tgt[8], uint16_t max_up, uint16_t next[8], uint8_t order[8])` and `bool link_rise_safe(uint16_t mate_cur, uint16_t duty)`.

- [ ] **Step 1: Write the failing test**

In `firmware/p4/test/test_link.c`: the file will now need ramp.h's pure half — add `#define RAMP_HOST_TEST` above the existing `#define LINK_HOST_TEST`. `firmware/p4/test/test_rt_glue.c` reaches link.h through rt_glue.h, so add the same `#define RAMP_HOST_TEST` line above its `#define LINK_HOST_TEST` too — without it that binary stops compiling the moment link.h includes ramp.h. Then add to test_link.c's `main()`:

```c
    /* --- write ordering: within a pair, the fall lands before the rise -----------
       A single ascending pass wrote a reversal's rising channel while its pair-mate
       still held the old duty on the chip — both BTS7960 inputs driven for the I2C
       gap, and for >=20 ms per tick while the fall's write kept failing. */
    {
        uint16_t cur[8] = { 2000, 0, 0, 1500, 0, 0, 0, 0 };
        uint16_t tgt[8] = { 0, 4095, 0, 1500, 0, 0, 300, 0 };
        uint16_t next[8];
        uint8_t  order[8];
        uint8_t  n = link_plan_writes(cur, tgt, 4095, next, order);
        assert(n == 3);
        assert(order[0] == 0);                    /* the fall (ch0: 2000 -> 0) first */
        assert(order[1] == 1 && order[2] == 6);   /* rises after, ascending */
        assert(next[0] == 0 && next[1] == 4095 && next[6] == 300);
        assert(next[3] == 1500);                  /* unchanged channel: no write */

        /* Bounded rise still ramps; fall is instant. */
        uint16_t cur2[8] = { 0, 1000, 0, 0, 0, 0, 0, 0 };
        uint16_t tgt2[8] = { 500, 0, 0, 0, 0, 0, 0, 0 };
        n = link_plan_writes(cur2, tgt2, 100, next, order);
        assert(n == 2 && order[0] == 1 && order[1] == 0);
        assert(next[1] == 0 && next[0] == 100);

        /* At boot the shadow is SHADOW-unknown (0xFFFF): everything "falls" to its
           target, so the zeroing writes are ordered first by construction. */
        uint16_t cur3[8] = { 0xFFFF, 0xFFFF, 0, 0, 0, 0, 0, 0 };
        uint16_t tgt3[8] = { 0 };
        n = link_plan_writes(cur3, tgt3, 4095, next, order);
        assert(n == 2 && order[0] == 0 && order[1] == 1 && next[0] == 0);
    }
    /* A rise may not land while the pair-mate holds ANY duty on the chip — the
       unknown boot shadow counts as driving. */
    assert(link_rise_safe(0, 4095));
    assert(link_rise_safe(2000, 0));      /* writing a zero is always safe */
    assert(!link_rise_safe(2000, 4095));
    assert(!link_rise_safe(0xFFFF, 1));
```

- [ ] **Step 2: Run to verify it fails**

Run: `make -C firmware/p4/test test_link 2>&1 | tail -3`
Expected: compile FAILURE — `link_plan_writes` undeclared.

- [ ] **Step 3: Implement**

In `firmware/p4/main/link.h`, add `#include "ramp.h"` after `#include "contract.h"`, and place in the pure section (before `#ifndef LINK_HOST_TEST`):

```c
/* Pure: plan one actuator tick. next[] receives every channel's post-ramp duty;
 * order[] receives the channels that need writing — every falling channel first, then
 * the rises — and the count is returned. Channel pairs are (0,1)(2,3)(4,5)(6,7), one
 * BTS7960 each (motors.h): a single ascending pass wrote a reversal's rise before its
 * pair-mate's fall, driving both bridge inputs for the I2C gap between them. */
static inline uint8_t link_plan_writes(const uint16_t cur[8], const uint16_t tgt[8],
                                       uint16_t max_up, uint16_t next[8],
                                       uint8_t order[8]) {
    uint8_t n = 0;
    for (uint8_t ch = 0; ch < 8; ch++) {
        next[ch] = ramp_step(cur[ch], tgt[ch], max_up);
        if (next[ch] < cur[ch]) order[n++] = ch;
    }
    for (uint8_t ch = 0; ch < 8; ch++) {
        if (next[ch] > cur[ch]) order[n++] = ch;
    }
    return n;
}

/* Pure: may a channel be driven to `duty` while its pair-mate's last-written duty is
 * `mate_cur`? Writing zero is always safe; a nonzero rise needs the mate at zero on
 * the chip. The boot shadow's unknown value is nonzero, so it counts as driving. */
static inline bool link_rise_safe(uint16_t mate_cur, uint16_t duty) {
    return duty == 0 || mate_cur == 0;
}
```

In `firmware/p4/main/link.c`, replace the write loop in `link_task` (the `uint16_t up = ...` line through the end of the `for (uint8_t ch = ...)` loop) with:

```c
        uint16_t up = ramp_max_up_per_tick(ramp_get_ms(), LINK_TICK_MS);
        uint16_t next[8];
        uint8_t  order[8];
        uint8_t  writes = link_plan_writes(s_current, tgt, up, next, order);
        bool wrote = false, failed = false;
        esp_err_t last_err = ESP_OK;
        for (uint8_t k = 0; k < writes; k++) {
            uint8_t ch = order[k];
            /* The mate's fall is ordered before this rise; if that write failed the
               mate still shows its old duty here, and the rise waits with it rather
               than driving both inputs of one bridge. */
            if (!link_rise_safe(s_current[ch ^ 1], next[ch])) continue;
            wrote = true;
            esp_err_t e = pca9685_set_pwm(ch, next[ch]);
            if (e == ESP_OK) {
                s_current[ch] = next[ch];      /* shadow follows the chip, not our intent */
            } else {
                /* Deliberately leave s_current alone. It still differs from the target,
                   so the next tick retries — where updating it first would have left the
                   firmware believing a spinning motor was stopped, forever. */
                failed = true;
                last_err = e;
            }
        }
```

- [ ] **Step 4: Run the suite**

Run: `make -C firmware/p4/test run`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add firmware/p4/main/link.h firmware/p4/main/link.c firmware/p4/test/test_link.c \
        firmware/p4/test/test_rt_glue.c
git commit -m "fix(fw): falls land before rises — never both bridge inputs driven

The write loop walked channels 0..7 with no knowledge of pairing, so a
direction flip wrote the rising channel while its pair-mate still held the
old duty on the chip; a failing fall write stretched that overlap to every
tick the fault lasted. The plan is now pure and host-tested (falls first,
rises gated on the mate's shadow reading zero), and the loop just executes it.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: Bus recovery paced by the clock, not by bloated ticks

**Files:**
- Modify: `firmware/p4/main/link.c` (the failure-pacing block at the end of link_task)

**Interfaces:**
- Consumes: `now_ms()` (link.c-local), `pca9685_bus_recover`.
- Produces: nothing new — behavior only.

- [ ] **Step 1: Implement**

There is no host seam for this block (it is the task loop against the real driver); the
verification is the suite staying green plus the final device build. Replace the
`static uint32_t s_fail_ticks;` block at the end of `link_task` with:

```c
        /* A wedged bus fails eight channels fifty times a second — but each failing
           write BLOCKS for up to two 50 ms I2C timeouts, so a "tick" under the exact
           fault pca9685_bus_recover exists for (SDA held low) runs 100-800 ms, and
           pacing by tick count turned "speak and recover once a second" into once
           per 10-40 s while the motors held their last duty. Pace by the clock. */
        static bool     s_failing;
        static uint32_t s_recover_at;
        if (failed) {
            uint32_t fnow = now_ms();
            if (!s_failing) {
                s_failing = true;
                s_recover_at = fnow + 1000;      /* first attempt after ~1 s of failure */
            } else if ((int32_t)(fnow - s_recover_at) >= 0) {
                ESP_LOGE(TAG, "PCA9685 write failing (%s) — resetting the I2C bus",
                         esp_err_to_name(last_err));
                pca9685_bus_recover();
                s_recover_at = fnow + 1000;
            }
        } else {
            s_failing = false;
        }
```

- [ ] **Step 2: Run the suite (compile sanity comes with Task 14's device build)**

Run: `make -C firmware/p4/test run && ./tools/test-all.sh 2>&1 | tail -3`
Expected: all green.

- [ ] **Step 3: Commit**

```bash
git add firmware/p4/main/link.c
git commit -m "fix(fw): bus recovery paced by wall time, not by 100-800ms 'ticks'

Fifty failing ticks was a second only when failures were fast NACKs; on a
wedged bus each write blocks up to two 50 ms timeouts and the only lever the
firmware has fired every 10-40 s instead — with the motors holding their last
duty throughout.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 8: The retreat consumes its history, and reads its config under the lock

**Files:**
- Modify: `firmware/p4/main/recovery.h` (recovery_seg_ms — pure), `firmware/p4/main/recovery.c` (snapshot_consume, retreat_task, recovery_on_link_lost)
- Test: `firmware/p4/test/test_recovery.c`

**Interfaces:**
- Consumes: nothing new.
- Produces: `RECOVER_SEG_MAX_MS` (250) and `uint32_t recovery_seg_ms(uint32_t newer_ts, uint32_t older_ts)` in recovery.h's pure section.

- [ ] **Step 1: Write the failing test**

In `firmware/p4/test/test_recovery.c`, add to `main()`:

```c
    /* One replay segment: the gap between breadcrumb timestamps, capped. The gap is
       drive time only while frames flowed at RT_COMMAND_HZ; across a refusal gap (a
       wizard pulse, an OTA hold) or the open tail it is dead air, and crediting it
       replayed a crawl as seconds of reverse. */
    assert(recovery_seg_ms(1100, 1000) == 100);
    assert(recovery_seg_ms(1000, 1000) == 0);
    assert(recovery_seg_ms(5000, 1000) == RECOVER_SEG_MAX_MS);
    assert(recovery_seg_ms(250 + 7, 7) == 250);
    assert(recovery_seg_ms(10, 0xFFFFFF00u) == RECOVER_SEG_MAX_MS);  /* rollover-safe */
```

- [ ] **Step 2: Run to verify it fails**

Run: `make -C firmware/p4/test test_recovery 2>&1 | tail -3`
Expected: compile FAILURE — `recovery_seg_ms` undeclared.

- [ ] **Step 3: Implement**

In `firmware/p4/main/recovery.h`, next to `recovery_evict`:

```c
// Pure (host-tested): one replay segment's duration, from the gap between two
// breadcrumb timestamps, capped at RECOVER_SEG_MAX_MS. Rollover-safe like the rest.
#define RECOVER_SEG_MAX_MS 250u
static inline uint32_t recovery_seg_ms(uint32_t newer_ts, uint32_t older_ts) {
    uint32_t d = newer_ts - older_ts;
    return d > RECOVER_SEG_MAX_MS ? RECOVER_SEG_MAX_MS : d;
}
```

In `firmware/p4/main/recovery.c`:

**(a)** Delete `#define TAIL_MS 400` (superseded by the segment cap). Replace `snapshot()` with:

```c
// Snapshot the in-window samples newest→oldest AND consume them: the ring is cleared
// and the liveness sequence bumped, *seq receiving the post-bump value — so the replay
// that follows aborts on the NEXT bump (a resumed stream, a goodbye, a new session),
// not on its own consumption. Consuming is the fix for the double retrace: the
// retreat's own motion is never recorded, so a second trip inside window_ms used to
// replay distance the first retreat had already covered, on top of it.
static int snapshot_consume(sample_t *out, uint32_t now, uint32_t *seq) {
    int n = 0;
    taskENTER_CRITICAL(&s_mux);
    uint16_t win = s_window_ms;
    for (int k = 0; k < s_count; k++) {
        int idx = (s_head - 1 - k + MAX_SAMPLES) % MAX_SAMPLES;  // newest → oldest
        if (recovery_evict(s_buf[idx].ts, now, win)) break;
        out[n++] = s_buf[idx];
    }
    s_head  = 0;
    s_count = 0;
    s_seq++;
    *seq = s_seq;
    taskEXIT_CRITICAL(&s_mux);
    return n;
}
```

**(b)** In `retreat_task`: `int n = snapshot(snap, t_loss, &snap_seq);` becomes `int n = snapshot_consume(snap, t_loss, &snap_seq);`, and the duration lines become:

```c
            uint32_t dur = (i == 0)
                ? recovery_seg_ms(t_loss, snap[0].ts)
                : recovery_seg_ms(snap[i - 1].ts, snap[i].ts);
```

(delete the `if (i == 0 && dur > TAIL_MS) dur = TAIL_MS;` line).

**(c)** Replace `recovery_on_link_lost` so the flag is read where every other reader reads it — under the lock:

```c
void recovery_on_link_lost(void) {
    bool enabled;
    taskENTER_CRITICAL(&s_mux);
    enabled = s_enabled;
    taskEXIT_CRITICAL(&s_mux);
    TaskHandle_t task = s_task;   /* written once in recovery_init, before rt_link exists */
    if (!enabled || task == NULL) {   // feature off → plain stop (old watchdog behavior)
        car_stop(LINK_SRC_RECOVER);
        link_release_must(LINK_SRC_RECOVER);
        return;
    }
    xTaskNotifyGive(task);
}
```

- [ ] **Step 4: Run the suite**

Run: `make -C firmware/p4/test run && ./tools/test-all.sh 2>&1 | tail -3`
Expected: all green.

- [ ] **Step 5: Commit**

```bash
git add firmware/p4/main/recovery.h firmware/p4/main/recovery.c firmware/p4/test/test_recovery.c
git commit -m "fix(fw): a retreat consumes the path it replays, and caps each segment

The ring survived the replay, so a second trip inside window_ms re-retraced
distance the first retreat had already covered; and an inter-sample gap that
was really a refusal window or the open tail was credited to the older sample
as drive time. Consume-at-snapshot plus a 250 ms per-segment cap close both;
recovery_on_link_lost now reads s_enabled under the same lock its writer uses.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 9: `api_util` — one error envelope, one body reader

**Files:**
- Create: `firmware/p4/main/api_util.h`, `firmware/p4/main/api_util.c`
- Modify: `firmware/p4/main/cfg_api.c` (drop its private copies), `firmware/p4/main/CMakeLists.txt`

**Interfaces:**
- Consumes: `esp_http_server.h`.
- Produces (Tasks 10–13 use these):
  - `esp_err_t api_reply_error(httpd_req_t *req, const char *status, const char *field, const char *msg)` — sends `{"error":"<msg>","field":"<field>"}` as `application/json` with the given status line.
  - `esp_err_t api_reply_ok(httpd_req_t *req)` — sends `{"ok":true}` as `application/json`.
  - `int api_read_body(httpd_req_t *req, char *buf, size_t n)` — loops until Content-Length is satisfied (3 timeout grace), NUL-terminates, returns length or -1.

No host test is possible (esp_http_server): this task is a pure extraction of code that
exists and works in cfg_api.c today, verified by the suite staying green and Task 14's
device build.

- [ ] **Step 1: Create `firmware/p4/main/api_util.h`**

```c
#ifndef API_UTIL_H
#define API_UTIL_H

#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* The REST surface's shared plumbing. One error shape for every endpoint —
 * {"error":"...","field":"..."}, field "" when the fault is with the body as a whole —
 * because the car and the mock answering different shapes let a client work in the
 * simulator and fail to parse the hardware. And one body reader, because "read the
 * whole body, however TCP split it" was fixed in cfg_api and the single-recv copy in
 * calib_api kept truncating segmented bodies into a 400 that blamed the field names. */

esp_err_t api_reply_error(httpd_req_t *req, const char *status, const char *field,
                          const char *msg);

/* {"ok":true} with the JSON content type — the documented success body. */
esp_err_t api_reply_ok(httpd_req_t *req);

/* Read the whole body into buf (NUL-terminated). Returns the length, or -1 when the
 * body is absent, too long for buf, or the socket gave up. */
int api_read_body(httpd_req_t *req, char *buf, size_t n);

#endif /* API_UTIL_H */
```

- [ ] **Step 2: Create `firmware/p4/main/api_util.c`**

Move (do not rewrite) `reply_error` and `read_body` from `cfg_api.c`, renamed:

```c
#include "api_util.h"
#include <stdio.h>

esp_err_t api_reply_error(httpd_req_t *req, const char *status, const char *field,
                          const char *msg) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\",\"field\":\"%s\"}", msg, field);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

esp_err_t api_reply_ok(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

int api_read_body(httpd_req_t *req, char *buf, size_t n) {
    if (req->content_len <= 0 || (size_t)req->content_len >= n) return -1;
    size_t got = 0;
    int timeouts = 0;
    while (got < (size_t)req->content_len) {
        int r = httpd_req_recv(req, buf + got, (size_t)req->content_len - got);
        if (r > 0) { got += (size_t)r; timeouts = 0; continue; }
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
        return -1;
    }
    buf[got] = '\0';
    return (int)got;
}
```

- [ ] **Step 3: Adopt in `cfg_api.c` and register the file**

- In `cfg_api.c`: add `#include "api_util.h"`; delete its static `reply_error` and `read_body`; rename every `reply_error(` call to `api_reply_error(` and every `read_body(` call to `api_read_body(`; replace the success tail (`httpd_resp_set_type(...); return httpd_resp_sendstr(req, "{\"ok\":true}");`) with `return api_reply_ok(req);`. Keep the comment that explains the segmented-body history — move it onto the `api_read_body` implementation if it would otherwise be deleted.
- In `firmware/p4/main/CMakeLists.txt`: add `"api_util.c"` to the SRCS list.

- [ ] **Step 4: Run the suite**

Run: `make -C firmware/p4/test run && ./tools/test-all.sh 2>&1 | tail -3`
Expected: all green.

- [ ] **Step 5: Commit**

```bash
git add firmware/p4/main/api_util.h firmware/p4/main/api_util.c \
        firmware/p4/main/cfg_api.c firmware/p4/main/CMakeLists.txt
git commit -m "refactor(fw): the REST envelope and body reader move to api_util

Extraction only: cfg_api's reply_error/read_body become api_reply_error/
api_read_body (+api_reply_ok) so calib_api and ota_api can stop answering a
different shape than the one protocol.md documents and the mock serves.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 10: calib_api — looped body read, JSON envelope, integral-only values

**Files:**
- Modify: `firmware/p4/main/calib_api.c`

**Interfaces:**
- Consumes: `api_reply_error`, `api_reply_ok`, `api_read_body` (Task 9); `link_release_must` (Task 4, already swept).
- Produces: wire behavior per spec rules 7–8 — success `{"ok":true}`, errors `{"error","field"}`, 409 `{"error":"actuator busy","field":""}`.

No host seam (esp_http_server); verified by suite + device build; body-level conformance
assertions arrive with the mock plan.

- [ ] **Step 1: Implement**

In `firmware/p4/main/calib_api.c`:

**(a)** Delete the file's private `read_body` (the single-recv one — the exact bug cfg_api documents having fixed) and `#include "api_util.h"`.

**(b)** `calib_spin` becomes:

```c
static esp_err_t calib_spin(httpd_req_t *req) {
    char b[32];
    if (api_read_body(req, b, sizeof(b)) < 0) {
        return api_reply_error(req, "400 Bad Request", "", "bad body");
    }
    cJSON *j = cJSON_Parse(b);
    cJSON *jp = cJSON_GetObjectItemCaseSensitive(j, "pair");
    cJSON *jd = cJSON_GetObjectItemCaseSensitive(j, "dir");
    if (!cJSON_IsNumber(jp) || !cJSON_IsNumber(jd) ||
        jp->valuedouble != (double)jp->valueint ||
        jd->valuedouble != (double)jd->valueint) {
        cJSON_Delete(j);
        return api_reply_error(req, "400 Bad Request", "", "need integer {pair,dir}");
    }
    int pair = jp->valueint, dir = jd->valueint;
    cJSON_Delete(j);
    if (pair < 0 || pair > 3) {
        return api_reply_error(req, "400 Bad Request", "pair", "pair 0..3");
    }
    if (dir != 0 && dir != 1) {
        return api_reply_error(req, "400 Bad Request", "dir", "dir 0|1");
    }
    ESP_LOGI(TAG, "spin pair %d %s", pair, dir ? "fwd" : "rev");
    if (!car_spin_pair((uint8_t)pair, dir != 0)) {
        /* 409 is the honest code — the request is fine, the actuator is taken. IDF's
           httpd_err_code_t has no 409, so the status line is set directly. */
        return api_reply_error(req, "409 Conflict", "", "actuator busy");
    }
    /* The grant lapses on its own after LINK_HOLD_CALIB_MS, so the pulse ends whether
       or not this handler is still here. The delay is only so the reply lands after
       the wheel has stopped, which is what the wizard's next step assumes. */
    vTaskDelay(pdMS_TO_TICKS(LINK_HOLD_CALIB_MS));
    link_release_must(LINK_SRC_CALIB);
    return api_reply_ok(req);
}
```

**(c)** `calib_save` — same treatment: `api_read_body(req, b, sizeof(b)) < 0` for the read; every `httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "<msg>")` becomes `api_reply_error(req, "400 Bad Request", "<field>", "<msg>")` with fields `""` for body-shape errors, `"wheels"` for the array-shape error, and `"pair"`/`"sign"` where a wheel entry is at fault; the per-wheel number check gains the same integral guard:

```c
        if (!cJSON_IsNumber(jp) || !cJSON_IsNumber(js) ||
            jp->valuedouble != (double)jp->valueint ||
            js->valuedouble != (double)js->valueint) {
            cJSON_Delete(j);
            return api_reply_error(req, "400 Bad Request", "pair", "wheel needs integer {pair,sign}");
        }
```

and the final `return httpd_resp_sendstr(req, "ok");` becomes `return api_reply_ok(req);` (the invalid-calibration rejection becomes `api_reply_error(req, "400 Bad Request", "wheels", "invalid calibration")`).

- [ ] **Step 2: Run the suite**

Run: `make -C firmware/p4/test run && ./tools/test-all.sh 2>&1 | tail -3`
Expected: all green.

- [ ] **Step 3: Commit**

```bash
git add firmware/p4/main/calib_api.c
git commit -m "fix(fw): calib endpoints speak the documented JSON envelope, whole-body reads

protocol.md promised {\"ok\":true} and {\"error\",\"field\"} everywhere; these
two answered plain-text ok and httpd's text errors, so a client written
against the doc (or the mock) failed on hardware. The single-recv body read —
the bug cfg_api documents having fixed — goes with it, and fractional
pair/dir/sign values are refused instead of silently truncated (audit rules
7-8).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 11: ota_api — the JSON envelope

**Files:**
- Modify: `firmware/p4/main/ota_api.c`

**Interfaces:**
- Consumes: `api_reply_error`, `api_reply_ok` (Task 9).
- Produces: `/ota` success body `{"ok":true}`; every error `{"error","field":""}` with its existing status.

- [ ] **Step 1: Implement**

In `firmware/p4/main/ota_api.c`, add `#include "api_util.h"` and replace the response calls only (the control flow, the sticky grant, the recv loop and the reboot stay exactly as they are):

- `httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "actuator busy")` → `api_reply_error(req, "500 Internal Server Error", "", "actuator busy")` (keep the `return`).
- `... "image too small")` → `api_reply_error(req, "400 Bad Request", "", "image too small")`.
- `... "image too large")` → `api_reply_error(req, "400 Bad Request", "", "image too large")`.
- `... "no ota partition")` → `api_reply_error(req, "500 Internal Server Error", "", "no ota partition")`.
- `... "ota begin failed")` → `api_reply_error(req, "500 Internal Server Error", "", "ota begin failed")`.
- `... "recv error")` → `api_reply_error(req, "400 Bad Request", "", "recv error")`.
- `... "ota write failed")` → `api_reply_error(req, "500 Internal Server Error", "", "ota write failed")`.
- `... "image invalid")` → `api_reply_error(req, "400 Bad Request", "", "image invalid")`.
- `... "set boot failed")` → `api_reply_error(req, "500 Internal Server Error", "", "set boot failed")`.
- Success: `if (httpd_resp_sendstr(req, "ok") != ESP_OK)` → `if (api_reply_ok(req) != ESP_OK)`.

- [ ] **Step 2: Run the suite**

Run: `make -C firmware/p4/test run && ./tools/test-all.sh 2>&1 | tail -3`
Expected: all green.

- [ ] **Step 3: Commit**

```bash
git add firmware/p4/main/ota_api.c
git commit -m "fix(fw): /ota answers the documented JSON envelope

Same rule as the calib endpoints: {\"ok\":true} on success, {\"error\",\"field\"}
on failure, application/json throughout — matching protocol.md, the mock,
and now the rest of this firmware (audit rule 8).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 12: cfg_api — integral-only values, and a 500 when apply/persist fails

**Files:**
- Modify: `firmware/p4/main/cfg_api.c` (integral check, set/save result handling, bindings)
- Modify: `firmware/p4/main/ramp.h`, `firmware/p4/main/ramp.c` (ramp_set_ms → bool, ramp_save → esp_err_t)
- Modify: `firmware/p4/main/car.h`, `firmware/p4/main/car.c` (car_save_trim → esp_err_t)
- Modify: `firmware/p4/main/recovery.h`, `firmware/p4/main/recovery.c` (recovery_save → esp_err_t)
- Modify: `firmware/p4/main/wheel.h`, `firmware/p4/main/wheel.c` (wheel_save → esp_err_t)
- Modify: `firmware/p4/main/dims.h`, `firmware/p4/main/dims.c` (dims_save → esp_err_t)

**Interfaces:**
- Consumes: `cfg_json_save` (already returns esp_err_t — today every caller discards it).
- Produces: `bool ramp_set_ms(uint16_t ms)`; `esp_err_t ramp_save(void)`, `esp_err_t car_save_trim(void)`, `esp_err_t recovery_save(void)`, `esp_err_t wheel_save(void)`, `esp_err_t dims_save(void)`; cfg_api binding typedefs `typedef bool (*cfg_set_fn)(const int32_t *in); typedef esp_err_t (*cfg_save_fn)(void);`.

- [ ] **Step 1: Implement the signatures**

- `ramp.h`: `bool ramp_set_ms(uint16_t ms);` and `esp_err_t ramp_save(void);` (both are in the `#ifndef RAMP_HOST_TEST` section — the pure half does not change, so test_ramp is untouched).
- `ramp.c`:

```c
bool ramp_set_ms(uint16_t ms) {
    if (ms > RAMP_MS_MAX) ms = RAMP_MS_MAX;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        /* Unreachable while the lock guards one u16 — but a timed-out set that then
           answered ok:true persisted the OLD value under a success reply. */
        ESP_LOGE(TAG, "ramp lock busy — %u not applied", ms);
        return false;
    }
    s_ramp_ms = ms;
    if (s_lock) xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "ramp_ms = %u", ms);
    return true;
}

esp_err_t ramp_save(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"ramp_ms\":%u}", ramp_get_ms());
    return cfg_json_save("ramp", buf);
}
```

- `car_save_trim`, `recovery_save`, `wheel_save`, `dims_save`: change the return type to `esp_err_t` in header and implementation and `return cfg_json_save(...)` instead of discarding it (each is a two-line change; update each header's comment from "Persist" to "Persist … and say whether it landed").

- [ ] **Step 2: Implement in cfg_api.c**

**(a)** Bindings:

```c
typedef void      (*cfg_get_fn)(int32_t *out);
typedef bool      (*cfg_set_fn)(const int32_t *in);
typedef esp_err_t (*cfg_save_fn)(void);
```

with the five set wrappers returning bool (`ramp_set_v` forwards `ramp_set_ms`'s result; the other four end with `return true;` — their setters cannot fail):

```c
static bool ramp_set_v(const int32_t *v) { return ramp_set_ms((uint16_t)v[0]); }
static bool trim_set_v(const int32_t *v) { car_set_trim((int8_t)v[0]); return true; }
static bool recover_set_v(const int32_t *v) {
    recovery_set_config(v[0] != 0, (uint16_t)v[1]);
    return true;
}
static bool wheel_set_v(const int32_t *v) {
    wheel_params_t w = { (uint16_t)v[0], (uint16_t)v[1], (uint16_t)v[2], (uint8_t)v[3] };
    wheel_set(&w);
    return true;
}
static bool dims_set_v(const int32_t *v) {
    dims_params_t d = { (uint16_t)v[0], (uint16_t)v[1] };
    dims_set(&d);
    return true;
}
```

**(b)** In `cfg_post`, after the `cJSON_IsNumber` check, add the integral guard:

```c
        /* cJSON's valueint TRUNCATES a fractional number, so {"trim_pct":25.7} used to
           apply as 25 under a 200 while the generated validator (the mock, conformance)
           answered 400 "must be an integer" for the same bytes. Same rule both sides. */
        if (it->valuedouble != (double)it->valueint) {
            cJSON_Delete(j);
            return api_reply_error(req, "400 Bad Request", f->name, "not an integer");
        }
```

**(c)** Replace the tail of `cfg_post`:

```c
    if (!b->set(vals)) {
        return api_reply_error(req, "500 Internal Server Error", "", "could not apply");
    }
    if (b->save() != ESP_OK) {
        return api_reply_error(req, "500 Internal Server Error", "", "could not persist");
    }
    return api_reply_ok(req);
```

- [ ] **Step 3: Run the suite**

Run: `make -C firmware/p4/test run && ./tools/test-all.sh 2>&1 | tail -3`
Expected: all green (test_ramp/test_wheel exercise only the pure halves; if a host test
fails to compile it is referencing a changed signature — fix the test's expectation to
the new signature, nothing else).

- [ ] **Step 4: Commit**

```bash
git add firmware/p4/main/cfg_api.c firmware/p4/main/ramp.h firmware/p4/main/ramp.c \
        firmware/p4/main/car.h firmware/p4/main/car.c firmware/p4/main/recovery.h \
        firmware/p4/main/recovery.c firmware/p4/main/wheel.h firmware/p4/main/wheel.c \
        firmware/p4/main/dims.h firmware/p4/main/dims.c
git commit -m "fix(fw): config POSTs refuse fractions and stop lying about persistence

valueint truncation applied 25 for a posted 25.7 where the generated
validator 400s — one wire, two behaviors. And a set/save failure answered
ok:true with the old value in NVS; the five save paths now report, and
cfg_post answers 500 when apply or persist fails.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 13: The small-robustness batch, and the comment contracts

**Files:**
- Modify: `firmware/p4/main/status_api.c` (overflow → 500; radio-pair comment)
- Modify: `firmware/p4/main/cfg_api.c` (cfg_get brace reserve)
- Modify: `firmware/p4/main/http_server.c` (GET / identity)
- Modify: `firmware/p4/main/telemetry.c` (volatile s_push_seq)
- Modify: `firmware/p4/main/calibration.c` (volatile s_valid)
- Modify: `firmware/p4/main/main.c` (parse_mix isfinite)
- Modify: `firmware/p4/main/car.h`, `firmware/p4/main/link.h` (watchdog comment contracts)

**Interfaces:** consumes `api_reply_error` (Task 9), `CAR_DEVICE_ID` (identity.h); produces nothing new.

- [ ] **Step 1: Implement**

**(a)** `status_api.c` — replace the clamp after the big snprintf:

```c
    if (n < 0 || n >= (int)sizeof(buf)) {
        /* Same rule as the hello reply: truncated identity JSON parses as a different
           car (or as nothing), and shipping it under a 200 hides exactly that. Only
           reachable if a future field outgrows the buffer — then this is the symptom. */
        ESP_LOGE(TAG, "/status does not fit its buffer");
        return api_reply_error(req, "500 Internal Server Error", "", "status too long");
    }
```

(add `#include "api_util.h"`), and extend the comment on `s_radio_fw`/`s_radio_ok`:

```c
// Written once by read_radio_version() — which status_api_start runs BEFORE registering
// the handler — then only read, so the cross-task safety is ordering, not a lock.
```

**(b)** `cfg_api.c` `cfg_get` — reserve the closing brace's byte; the per-field guard becomes:

```c
        if (w < 0 || (size_t)(n + w) >= sizeof(buf) - 1) {   /* -1: the brace's byte */
            return api_reply_error(req, "500 Internal Server Error", "", "response too long");
        }
```

**(c)** `http_server.c` — add `#include "identity.h"` and `#include "esp_app_desc.h"`; the handler becomes:

```c
// There is no web UI: GET / answers "<device> <fw>" so a stray browser (or a script)
// learns what this device is — the same one-line identity the mock serves, because
// protocol.md calls this endpoint an identity and only the mock was honoring that.
static esp_err_t root_get_handler(httpd_req_t *req) {
    char line[64];
    snprintf(line, sizeof(line), "%s %s\n", CAR_DEVICE_ID,
             esp_app_get_description()->version);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, line);
}
```

**(d)** `telemetry.c` — the counter inside `telemetry_gather` moves to file scope as
`static volatile uint32_t s_push_seq;` (above the function), with:

```c
/* Bumped on the rt_link task, read by the httpd task through /status. volatile like
   s_frames/s_trips beside it: aligned u32 loads are atomic on this target, but the
   cross-task access is a fact worth declaring, not an ISA accident worth inheriting. */
```

**(e)** `calibration.c` — `static volatile bool s_valid = false;` with a one-line comment (`written by the httpd task on save, read at 5 Hz on rt_link`).

**(f)** `main.c` — add `#include <math.h>` and extend `parse_mix`'s guard:

```c
    // Reject out-of-range console input early with an error (car_drive also clamps).
    // NaN fails every comparison below, so without isfinite `mix nan nan` was accepted
    // and rode a formally-undefined float->uint16 cast to a lucky stop.
    if (!isfinite(*t) || !isfinite(*y)) return -1;
    if (*t < -1.0f || *t > 1.0f || *y < -1.0f || *y > 1.0f) return -1;
```

**(g)** The comment contracts. In `car.h`, replace the paragraph "Returns false when a
higher-priority source holds the actuator. Nothing was applied in that case, and the
caller must not treat the command as a live frame — the control watchdog is fed only on
a true return." with:

```c
// Returns false when a higher-priority source holds the actuator. Nothing was applied
// in that case, and the caller must not treat the command as APPLIED: the breadcrumb
// history records only true returns, because a refused command never moved the car.
// The control watchdog is a different matter — rt_link feeds it on every parsed
// in-session command, accepted or refused, deliberately: a stream refused by a wizard
// pulse or an OTA hold is still a live stream, and tripping into a retreat mid-wizard
// is worse than the refusal. (The plan, the mock and its tests all pin this.)
```

In `link.h`, replace the equivalent sentence on `link_set` ("…the caller must not treat
the command as applied — the control watchdog is fed only on a true return.") with:

```c
/* Ask to set the eight duties. Returns false when a higher-priority source holds the
 * actuator: nothing was written, and the caller must not treat the command as applied.
 * (Only the breadcrumb recorder keys on this return; the control watchdog is fed
 * upstream on every parsed in-session command, refused ones included — see car.h.) */
```

- [ ] **Step 2: Run the suite**

Run: `make -C firmware/p4/test run && ./tools/test-all.sh 2>&1 | tail -3`
Expected: all green (test_contract_wire compiles telemetry.h only — the .c change is
invisible to it).

- [ ] **Step 3: Commit**

```bash
git add firmware/p4/main/status_api.c firmware/p4/main/cfg_api.c \
        firmware/p4/main/http_server.c firmware/p4/main/telemetry.c \
        firmware/p4/main/calibration.c firmware/p4/main/main.c \
        firmware/p4/main/car.h firmware/p4/main/link.h
git commit -m "fix(fw): the robustness batch the audit filed under 'info'

/status overflow becomes a 500 instead of truncated JSON under a 200; cfg_get
reserves its closing brace; GET / answers the identity line protocol.md
promises; the cross-task statics say volatile out loud; the console refuses
mix nan; and car.h/link.h stop promising a watchdog contract the code has
never kept — the real rule (fed on every parsed in-session command) is now
written where the next maintainer will read it.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 14: Full verification — host suite plus the device build

**Files:** none modified.

- [ ] **Step 1: The whole host suite, conformance required**

Run: `CONFORMANCE=required ./tools/test-all.sh`
Expected: `== all green ==` with conformance actually running (the worktree's venv exists).

- [ ] **Step 2: The device build**

The toolchain is on this machine (`~/esp/esp-idf-v6.0.2`, used for the 2026-08-20 bench
bring-up). A fresh worktree has no `build/`, so this configures from `sdkconfig.defaults`
and compiles everything — several minutes.

Run: `bash -c 'source tools/env-p4.sh && cd firmware/p4 && idf.py build' 2>&1 | tail -15`
Expected: `Project build complete.` (with the usual binary-size summary). No hardware is
needed — this is a compile, not a flash.

If the export script or toolchain is missing on this machine, record the exact failure in
the task report and stop — do NOT try to install ESP-IDF; the orchestrator decides.

- [ ] **Step 3: Commit anything the build regenerated**

`idf.py` must not have modified tracked files; `git status --short` should show only
untracked `firmware/p4/build/` (gitignored) and `sdkconfig` (gitignored). If a tracked
file changed, stop and report — nothing in this plan expects that.

- [ ] **Step 4: Report**

Summarize: tasks completed, test counts, build result, and the two behaviors that now
deliberately diverge from the mock until the mock plan lands (REST envelopes on
calib/ota, the rule-6 pinned frames).

---

## Self-review notes (author)

- Spec coverage: rules 1–9 land in Tasks 1–13 (rule 10–12 are docs/mock-side; rule 11's
  IDEAS entry belongs to the docs plan). Every firmware finding in the audit's scope maps
  to a task: A1→5, A2→6, A3→7, A4→3, A5→13g, A6→4, A7→4+5, A8→3, A10→8, A12→8, B1→2,
  B2→10+12, B3→9+10, B4→13, B5→12, B6→13, B7→13, B8→13, B9→13, E5→5, plus contract
  rule 4→1.
- Type consistency: `rt_session_classify(s, dead, from_owner, f)`, `rt_session_adopt(s,
  sid, now)`, `rt_glue_*` signatures, `link_release_must`, `api_*` names are spelled
  identically in every task that uses them.
- Known deliberate divergences until the mock plan lands: REST bodies (calib/ota
  envelope), parser strictness (rule 6), trip-gate/dead-sid/mortality (rules 1, 3, 4),
  bye-under-sticky (rule 2). `test-all.sh` stays green throughout because conformance
  exercises the mock against its own tables.
