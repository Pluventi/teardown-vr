# Crude Lua 5.1 static checks, no Lua runtime needed. usage: luacheck.py mod/TeardownVR/main.lua
#  - tokenizer (strings, long brackets, comments, numbers)
#  - block balance (function/if/for/while/do/repeat ... end/until), bracket balance
#  - main-chunk local count (limit 200) and upvalues per top-level function (limit 60)
#  - identifiers that are neither locals of the file nor API globals (script_defs) nor Lua builtins
import os, re, sys

path = sys.argv[1]
GAME = os.environ.get("TEARDOWN_DIR", r"C:\Program Files (x86)\Steam\steamapps\common\Teardown")
defs = open(os.path.join(GAME, "data", "script_defs.lua"), encoding="utf-8", errors="replace").read()
api = set(re.findall(r"^function ([A-Za-z_][A-Za-z0-9_]*)\(", defs, re.M))
src = open(path, encoding="utf-8").read()

toks = []  # (kind, value, line)
i, n, line = 0, len(src), 1
def longbr(s, i):
    m = re.match(r"\[(=*)\[", s[i:])
    if not m: return None
    close = "]" + m.group(1) + "]"
    j = s.find(close, i + len(m.group(0)))
    if j < 0: raise SystemExit("unterminated long bracket at %d" % i)
    return j + len(close)
while i < n:
    c = src[i]
    if c == "\n": line += 1; i += 1; continue
    if c.isspace(): i += 1; continue
    if src.startswith("--", i):
        e = longbr(src, i + 2)
        if e: line += src.count("\n", i, e); i = e; continue
        j = src.find("\n", i); i = n if j < 0 else j; continue
    if c in "\"'":
        j = i + 1
        while j < n and src[j] != c:
            if src[j] == "\\": j += 1
            if src[j] == "\n": raise SystemExit("newline in string line %d" % line)
            j += 1
        toks.append(("str", src[i:j + 1], line)); i = j + 1; continue
    if c == "[":
        e = longbr(src, i)
        if e: toks.append(("str", "<long>", line)); line += src.count("\n", i, e); i = e; continue
    m = re.match(r"0[xX][0-9a-fA-F]+|\d+\.?\d*(?:[eE][+-]?\d+)?|\.\d+(?:[eE][+-]?\d+)?", src[i:])
    if m and (c.isdigit() or (c == "." and i + 1 < n and src[i + 1].isdigit())):
        toks.append(("num", m.group(0), line)); i += len(m.group(0)); continue
    m = re.match(r"[A-Za-z_][A-Za-z0-9_]*", src[i:])
    if m: toks.append(("name", m.group(0), line)); i += len(m.group(0)); continue
    for op in ("...", "..", "==", "~=", "<=", ">="):
        if src.startswith(op, i): toks.append(("op", op, line)); i += len(op); break
    else:
        toks.append(("op", c, line)); i += 1

KW = set("and break do else elseif end false for function if in local nil not or repeat return then true until while".split())
stack = []
br = []
for k, v, ln in toks:
    if k == "op" and v in "({[": br.append((v, ln))
    elif k == "op" and v in ")}]":
        if not br or {"(": ")", "{": "}", "[": "]"}[br[-1][0]] != v: raise SystemExit("bracket mismatch line %d" % ln)
        br.pop()
    if k != "name": continue
    if v == "function": stack.append(["function", ln])
    elif v == "if": stack.append(["if", ln])
    elif v in ("for", "while"): stack.append(["loop", ln, False])
    elif v == "do":
        if stack and stack[-1][0] == "loop" and not stack[-1][2]: stack[-1][2] = True
        else: stack.append(["do", ln])
    elif v == "repeat": stack.append(["repeat", ln])
    elif v == "until":
        if not stack or stack[-1][0] != "repeat": raise SystemExit("until without repeat line %d" % ln)
        stack.pop()
    elif v == "end":
        if not stack or stack[-1][0] == "repeat": raise SystemExit("unexpected end line %d" % ln)
        stack.pop()
if stack: raise SystemExit("unclosed block(s): %s" % stack[-3:])
if br: raise SystemExit("unclosed bracket(s): %s" % br[-3:])
print("blocks and brackets balanced, %d tokens" % len(toks))

# main-chunk locals: 'local' at depth 0 (track depth again)
depth = 0
mainLocals = []
fnRanges = []   # (name, startTok, endTok) of top-level functions
cur = None
j = 0
st = []
while j < len(toks):
    k, v, ln = toks[j]
    if k == "name":
        if v == "local" and not st:
            if toks[j + 1][1] == "function":
                mainLocals.append(toks[j + 2][1])
            else:
                q = j + 1
                while True:
                    mainLocals.append(toks[q][1])
                    if toks[q + 1][1] == ",": q += 2
                    else: break
        if v == "function":
            if not st:
                nm = toks[j + 1][1] if toks[j + 1][0] == "name" else "(anon)"
                if toks[j - 1][1] == "=": nm = toks[j - 2][1]
                cur = [nm, j]
            st.append("function")
        elif v == "if": st.append("if")
        elif v in ("for", "while"): st.append("loop0")
        elif v == "do":
            if st and st[-1] == "loop0": st[-1] = "loop"
            else: st.append("do")
        elif v == "repeat": st.append("repeat")
        elif v == "until": st.pop()
        elif v == "end":
            st.pop()
            if not st and cur:
                fnRanges.append((cur[0], cur[1], j)); cur = None
    j += 1
print("main-chunk locals: %d (Lua 5.1 limit 200)" % len(mainLocals))
ml = set(mainLocals)
worst = []
for nm, a, b in fnRanges:
    names = set()
    for q in range(a, b):
        k, v, ln = toks[q]
        if k == "name" and v in ml and not (q > 0 and toks[q - 1][1] in (".", ":")):
            names.add(v)
    worst.append((len(names), nm))
worst.sort(reverse=True)
print("max upvalues (approx, upper bound) per top-level function (limit 60):", worst[:6])

# unknown identifiers (crude): names not keywords, not API, not locals anywhere, not fields
builtins = set("math string table ipairs pairs tostring tonumber type pcall error select unpack next print assert setmetatable getmetatable rawget rawset _G os".split())
alllocals = set()
for q, (k, v, ln) in enumerate(toks):
    if k == "name" and v == "local":
        r = q + 1
        if toks[r][1] == "function": alllocals.add(toks[r + 1][1]); continue
        while True:
            alllocals.add(toks[r][1])
            if toks[r + 1][1] == ",": r += 2
            else: break
    if k == "name" and v == "function":
        # parameters
        r = q + 1
        while toks[r][1] != "(": r += 1
        r += 1
        while toks[r][1] != ")":
            if toks[r][0] == "name": alllocals.add(toks[r][1])
            r += 1
    if k == "name" and v == "for":
        r = q + 1
        while toks[r][1] not in ("=", "in"):
            if toks[r][0] == "name": alllocals.add(toks[r][1])
            r += 1
globalsAssigned = set()
for q, (k, v, ln) in enumerate(toks):
    if k == "name" and q + 1 < len(toks) and toks[q + 1][1] == "=" and not (q > 0 and toks[q - 1][1] in (".", ":", "local", ",")):
        globalsAssigned.add(v)
unknown = {}
for q, (k, v, ln) in enumerate(toks):
    if k != "name" or v in KW: continue
    if q > 0 and toks[q - 1][1] in (".", ":"): continue
    if q + 1 < len(toks) and toks[q + 1][1] == "=" and q > 0 and toks[q - 1][1] in ("{", ","): continue  # table field
    if v in api or v in builtins or v in alllocals or v in globalsAssigned: continue
    unknown.setdefault(v, ln)
print("unknown identifiers:", unknown)
