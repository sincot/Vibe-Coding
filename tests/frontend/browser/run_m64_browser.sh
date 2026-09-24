#!/usr/bin/env bash
# M6.4 最终验收 A 真实浏览器运行脚本（Linux 端；不注册 CTest）。
#
# 启动隔离 oj_server（临时库 + 种子题 + 随机端口 + 静态托管 web/），再用真实 Chromium
# 执行 m64_browser_scenarios.js，输出逐项结果与截图。
#
# 运行器选择：
#   1. 若设置 PWCLI 或存在 playwright-cli，则用 playwright-cli open/run-code/close；
#   2. 否则若存在 playwright-core（默认 /tmp/opencode/pwcli/node_modules/playwright-core），
#      用内置 Node 驱动脚本直接启动 Chromium（执行同一场景文件）。
#
# 用法：
#   bash tests/frontend/browser/run_m64_browser.sh
# 可选：PWCLI=... / PWCLI_HEADED=1 / BROWSER_ARTIFACTS_DIR=... / OJ_BROWSER_PORT=...
#       PW_CORE_DIR=... / CHROME_PATH=... / PWCLI_BROWSER_LIBS=...
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SERVER="$ROOT/build/oj_server"
SCENARIO="$ROOT/tests/frontend/browser/m64_browser_scenarios.js"
CHROME_PATH="${CHROME_PATH:-$HOME/.cache/ms-playwright/chromium-1246/chrome-linux64/chrome}"
PW_CORE_DIR="${PW_CORE_DIR:-/tmp/opencode/pwcli/node_modules/playwright-core}"
PWCLI="${PWCLI:-}"
if [ -z "$PWCLI" ] && command -v playwright-cli >/dev/null 2>&1; then PWCLI="$(command -v playwright-cli)"; fi

[ -x "$SERVER" ] || { echo "未找到 $SERVER，请先构建：cmake --build $ROOT/build --parallel 1" >&2; exit 2; }
if [ -z "$PWCLI" ] && [ ! -d "$PW_CORE_DIR" ]; then
  echo "未找到 playwright-cli 或 playwright-core（$PW_CORE_DIR）；请安装其一。" >&2; exit 2
fi
if [ -n "${PWCLI_BROWSER_LIBS:-}" ]; then export LD_LIBRARY_PATH="${PWCLI_BROWSER_LIBS}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"; fi

TMP="$(mktemp -d /tmp/opencode/m64browser.XXXXXX)"
DB="$TMP/oj.db"; ADM_PW="M64BrowserPw123!"; PORT="${OJ_BROWSER_PORT:-$(( (RANDOM % 2000) + 23000 ))}"
export OJ_JWT_SECRET="m64-browser-verify-secret-0123456789"
export OJ_ADMIN_PASSWORD="$ADM_PW" OJ_JUDGE_WORKSPACE="${OJ_JUDGE_WORKSPACE:-/dev/shm}"
SRV=""
cleanup(){ [ -n "$SRV" ] && { kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; }; [ -n "${PWCLI:-}" ] && "$PWCLI" close-all >/dev/null 2>&1 || true; rm -rf "$TMP"; }
trap cleanup EXIT
"$SERVER" --db "$DB" --seed >"$TMP/seed.log" 2>&1
"$SERVER" --host 127.0.0.1 --port "$PORT" --db "$DB" --web "$ROOT/web" >"$TMP/server.log" 2>&1 &
SRV=$!
for _ in $(seq 1 80); do curl -sf "http://127.0.0.1:$PORT/api/health" >/dev/null 2>&1 && break; sleep 0.2; done
if ! curl -sf "http://127.0.0.1:$PORT/api/health" >/dev/null 2>&1; then
  echo "服务未成功启动，日志：$TMP/server.log" >&2; tail -20 "$TMP/server.log" >&2; exit 1
fi

OUT="$TMP/artifacts"; mkdir -p "$OUT"
if [ -n "$PWCLI" ]; then
  sed -e "s#__BASE__#http://127.0.0.1:$PORT#g" -e "s#__OUT__#$OUT#g" -e "s#__ADMIN_PW__#$ADM_PW#g" "$SCENARIO" > "$TMP/scenarios.js"
  cd "$TMP"
  OPEN_ARGS=("open" "about:blank" "--browser=chromium")
  [ "${PWCLI_HEADED:-0}" = "1" ] && OPEN_ARGS+=("--headed")
  "$PWCLI" "${OPEN_ARGS[@]}" > "$TMP/open.log" 2>&1
  "$PWCLI" run-code --filename="$TMP/scenarios.js" > "$TMP/result.log" 2>&1
  RC=$?
  node -e '
const fs=require("fs");const t=fs.readFileSync(process.argv[1],"utf8");const p=t.split(/^### Result\s*$/m);
if(p.length<2){console.error(t.slice(0,800));process.exit(1);}let v=JSON.parse(p[1].trim().split("\n")[0]);if(typeof v==="string")v=JSON.parse(v);
const f=(v.results||[]).filter(r=>!r.ok);console.log(`M6.4 真实浏览器：通过 ${(v.results||[]).length-f.length}/${(v.results||[]).length}`);
for(const x of f)console.log("  FAIL: "+x.name+" | "+(x.detail||""));for(const e of(v.errors||[]))console.log("  screrr: "+e);
process.exit(f.length||(v.errors||[]).length?1:0);' "$TMP/result.log"
  RC2=$?
else
  cat > "$TMP/runner.js" <<'NODE'
const fs=require('fs');const {chromium}=require(process.env.PW_CORE_DIR);
const [f,base,adm,out]=process.argv.slice(2);fs.mkdirSync(out,{recursive:true});
const content=fs.readFileSync(f,'utf8').split('__BASE__').join(base).split('__OUT__').join(out).split('__ADMIN_PW__').join(adm);
const fn=eval('('+content+')');
(async()=>{const b=await chromium.launch({executablePath:process.env.CHROME_PATH,headless:process.env.PWCLI_HEADED!=="1",args:['--no-sandbox','--disable-dev-shm-usage']});
const c=await b.newContext({viewport:{width:1280,height:800}});const pg=await c.newPage();
let raw;try{raw=await fn(pg);}catch(e){console.error('SCENARIO_THREW',e);await b.close();process.exit(2);}
let o=raw;if(typeof o==='string')o=JSON.parse(o);const R=o.results||[],E=o.errors||[],F=R.filter(r=>!r.ok);
for(const r of R)console.log((r.ok?'  [PASS] ':'  [FAIL] ')+r.name+(r.detail?' | '+r.detail:''));
console.log(`\nM6.4 真实浏览器：通过 ${R.length-F.length}/${R.length}`);for(const e of E)console.log('  screrr: '+e);
await b.close();process.exit(F.length||E.length?1:0);})();
NODE
  PW_CORE_DIR="$PW_CORE_DIR" CHROME_PATH="$CHROME_PATH" node "$TMP/runner.js" "$SCENARIO" "http://127.0.0.1:$PORT" "$ADM_PW" "$OUT"
  RC2=$?
fi

if [ -n "${BROWSER_ARTIFACTS_DIR:-}" ]; then
  mkdir -p "$BROWSER_ARTIFACTS_DIR"; cp -f "$OUT"/*.png "$BROWSER_ARTIFACTS_DIR"/ 2>/dev/null || true
  echo "截图已另存：$BROWSER_ARTIFACTS_DIR"
fi
exit "${RC2:-1}"
