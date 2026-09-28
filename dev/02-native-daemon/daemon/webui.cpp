// webui.cpp — 内置网页控制台（单文件，零依赖）

#include "webui.h"

namespace autod {

const std::string& WebUiHtml() {
    // 用原始字符串字面量：HTML 里有大量引号和反斜杠，
    // 逐个转义既难写又难读。分隔符用 )HTML" 避免和内容冲突。
    static const std::string kHtml = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>autod 控制台</title>
<style>
  :root { --bg:#111; --fg:#eee; --dim:#888; --accent:#4a9; --warn:#c55; }
  * { box-sizing: border-box; }
  /* body 用列布局撑满视口，main 拿 flex:1 —— 比 calc(100vh - 46px) 稳：
     header 会换行、鉴权条会出现，写死减多少迟早对不上。 */
  body { margin:0; background:var(--bg); color:var(--fg); height:100vh;
         display:flex; flex-direction:column; overflow:hidden;
         font:13px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif; }
  header { padding:8px 12px; background:#1b1b1b; border-bottom:1px solid #333;
           display:flex; gap:12px; align-items:center; flex-wrap:wrap; }
  header h1 { font-size:15px; margin:0; font-weight:600; }
  .dim { color:var(--dim); }
  main { display:flex; gap:10px; padding:10px; align-items:stretch;
         flex:1 1 auto; min-height:0; overflow:hidden; }
  /* 画面区**抢走所有剩余宽度**，面板固定宽。
     早先这里是 flex-wrap + 画面写死 max-width:min(90vw,560px)，
     而面板 flex:1 —— 结果一块 1280x720 的横屏设备被压成 560px 宽，
     面板反而占了大半屏。横屏设备最需要的就是宽度。 */
  .screen { flex:1 1 auto; min-width:0; position:relative; background:#000;
            border:1px solid #333; }
  /* 画面用 image-rendering:pixelated 保持原始比例 —— 默认的平滑
     会让小屏截图糊成一团，坐标也难对准。

     width/height 都是 auto + 同时给 max-width/max-height：
     替换元素会按**保持宽高比**的方式缩到能塞进这个盒子 —— 也就是
     "尽可能大但不裁切"。宽高比是流决定的（横屏 16:9、竖屏 9:16），
     所以不能写死尺寸。 */
  #screen { display:block; width:auto; height:auto;
            max-width:100%; max-height:100%;
            image-rendering:pixelated; cursor:crosshair;
            touch-action:none; user-select:none; background:#000; }
  /* 画布要能在 .screen 里居中。.screen 是 flex 容器，
     min-height:0 是为了让 max-height:100% 真的生效（flex 子项的默认
     min-height:auto 会把盒子撑开，百分比高度就失去参照）。 */
  .screen { display:flex; align-items:center; justify-content:center;
            min-height:0; overflow:hidden; }
  .panel { flex:0 0 300px; width:300px; display:flex; flex-direction:column;
           gap:10px; max-height:100%; overflow-y:auto; }
  /* 面板收起 —— 横屏设备默认收，把宽度全让给画面 */
  body.nopanel .panel { display:none; }
  body.nopanel .screen { flex:1 1 100%; }

  /* 铺满模式：画面**保持宽高比**放大到显示区里能完整放下的最大值 ——
     不裁切、不溢出、也不变形。用的就是 object-fit:contain。

     ⚠️ 这里**故意**让浏览器做缩放，而不是用 JS 算好尺寸写上去。
        曾经用 JS 写过一版（--fw/--fh），失败模式正是用户报的那个：
        JS 没跑到 / 自定义属性没生效时，width 退回 auto，
        再被基类的 max-width:100% + max-height:100% **分别**钳一下 ——
        两个方向各钳各的，比例就没了，画面被拉伸。
        object-fit:contain 最坏情况也只是留黑边，永远不会变形。

     ⚠️ 代价：元素盒子是整个显示区，黑边那块也归 canvas 收事件。
        所以 toScreen() 走 contentRect() 算真正画着画面的那块矩形，
        点在矩形外的一律当没点（见 toScreen 的调用方）。 */
  body.fillmode #screen { width:100%; height:100%; object-fit:contain; }
  .card { background:#1b1b1b; border:1px solid #333; border-radius:6px;
          padding:10px; }
  .card h2 { font-size:12px; margin:0 0 8px; color:var(--dim);
             font-weight:600; text-transform:uppercase; letter-spacing:.5px; }
  /* 可收起的卡片头。整行可点，不只小三角 —— 手机上点一个小图标很难命中 */
  .card h2.clickable { cursor:pointer; user-select:none; display:flex;
                       align-items:center; gap:6px; margin-bottom:0; }
  .card h2.clickable .arrow { font-size:10px; transition:transform .15s;
                              display:inline-block; color:#666; }
  .card.collapsed h2.clickable .arrow { transform:rotate(-90deg); }
  .card.collapsed .body { display:none; }
  .card h2.clickable + .body { margin-top:8px; }
  .row { display:flex; gap:6px; flex-wrap:wrap; }
  button { background:#2a2a2a; color:var(--fg); border:1px solid #444;
           border-radius:4px; padding:6px 10px; font-size:12px; cursor:pointer; }
  button:hover { background:#333; }
  button:active { background:var(--accent); color:#000; }
  input[type=text] { flex:1; background:#0d0d0d; color:var(--fg);
                     border:1px solid #444; border-radius:4px; padding:6px 8px;
                     font-size:12px; min-width:0; }
  #status { font-size:12px; min-height:1.4em; }
  .ok { color:var(--accent); } .err { color:var(--warn); }
  #clip { white-space:pre-wrap; word-break:break-all; font-size:12px;
          max-height:120px; overflow:auto; background:#0d0d0d; padding:6px;
          border-radius:4px; border:1px solid #333; }
  #coord { position:absolute; right:4px; bottom:4px; background:rgba(0,0,0,.7);
           padding:2px 6px; border-radius:3px; font-size:11px; }
  .app { display:flex; align-items:center; gap:6px; padding:3px 0;
         border-bottom:1px solid #262626; }
  .app:last-child { border-bottom:none; }
  .app .pkg { flex:1; min-width:0; overflow:hidden; text-overflow:ellipsis;
              white-space:nowrap; }
  .app button { padding:2px 7px; font-size:11px; }
  #apps { max-height:240px; overflow:auto; }
  #apps input[type=text] { margin-bottom:6px; }
  .app button { padding:2px 7px; font-size:11px; }
  #logbox { margin-top:6px; height:220px; overflow:auto; background:#0d0d0d;
            border:1px solid #333; border-radius:4px; padding:5px;
            font:11px/1.45 ui-monospace,Menlo,Consolas,monospace;
            white-space:pre-wrap; word-break:break-all; }
  .lg-I { color:#9cf; } .lg-W { color:#fc6; } .lg-E { color:#f88; }
  .lg-D { color:#888; } .lg-T { color:#666; }
</style>
</head>
<body>
<header>
  <h1>autod 控制台</h1>
  <span class="dim" id="meta">连接中…</span>
  <!-- 服务对外开关。放在最显眼的位置：它是"这台设备还能不能被控制"
       的总闸，而不是某个功能的设置。 -->
  <label id="svcwrap" style="margin-left:auto; display:flex; align-items:center;
         gap:6px; font-size:12px; color:#4a9">
    <input type="checkbox" id="svcsw" onchange="toggleService(this.checked)">
    <span id="svctext">服务对外可用</span>
  </label>
  <!-- 面板开关。横屏设备最缺宽度，收起面板就能让画面占满整屏。 -->
  <button id="panelsw" onclick="togglePanel()" title="收起/展开右侧控制面板"
          style="font-size:12px; padding:3px 8px">面板</button>
  <!-- 铺满开关。默认"适应"：保持设备宽高比，四周可能有黑边。
       "铺满"则拉伸填满整个显示区 —— 没有黑边，代价是画面按显示区
       比例轻微变形（设备 16:9 而窗口 16:10 时约 11%）。 -->
  <button id="fillsw" onclick="toggleFill()"
          title="铺满整个显示区（会按显示区比例拉伸）/ 恢复保持比例"
          style="font-size:12px; padding:3px 8px">适应</button>
</header>

<!-- 令牌条。开启鉴权后才需要填，平时隐藏（display:none）——
     平时摆一个用不上的输入框只会让人以为哪里要授权。 -->
<div id="authbar" style="display:none; padding:6px 12px; background:#3a2a12;
     border-bottom:1px solid #553; align-items:center; gap:6px;">
  <span style="font-size:12px; color:#dda">此服务需要访问令牌</span>
  <input type="text" id="tokeninput" placeholder="粘贴令牌" style="flex:1; min-width:0">
  <button onclick="saveToken()">保存</button>
</div>

<main>
  <div class="screen">
    <canvas id="screen"></canvas>
    <div id="coord"></div>
  </div>

  <div class="panel">
    <div class="card">
      <h2>状态</h2>
      <div id="status" class="dim">就绪</div>
      <!-- 服务端**实际**在按什么节奏抓帧。
           本页要 10fps 不代表设备只被拉 10fps —— 抓帧节奏取所有订阅者的
           最高需求，另一个客户端挂着 60fps 就会把整机拉满。
           没有这一行的话，"我明明只要 10 帧为什么设备这么烫"只能靠 curl。 -->
      <div id="cadence" class="dim" style="font-size:11px;margin-top:4px"></div>
      <div id="dbg" class="dim" style="font-size:11px;margin-top:4px"></div>
      <div class="row" style="margin-top:8px">
        <button onclick="refresh()">刷新状态</button>
        <button onclick="setFps(0)">暂停</button>
        <button onclick="setFps(10)">10</button>
        <button onclick="setFps(20)">20</button>
        <button onclick="setFps(30)">30</button>
        <button onclick="setFps(60)">60</button>
      </div>
      <div class="row" style="margin-top:6px">
        <span class="dim" style="font-size:11px;align-self:center">画质</span>
        <button onclick="setCodec('jpeg',70)">JPEG 快</button>
        <button onclick="setCodec('jpeg',90)">JPEG 清</button>
        <button onclick="setCodec('webp',75)">WebP</button>
        <button onclick="setCodec('png',1)">PNG 无损</button>
        <button id="btn-h264" onclick="setCodec('h264',75)"
                title="H.264：帧比 JPEG 小两个数量级，需要浏览器支持 WebCodecs">H.264</button>
        </div>
        <div class="row" style="margin-top:6px">
          <span class="dim" style="font-size:11px;align-self:center">质量</span>
          <input type="range" id="qslider" min="1" max="100" value="75"
                 oninput="onQualitySlide(this.value)"
                 style="flex:1;min-width:60px;accent-color:#4a9eff">
          <span id="qval" class="dim" style="font-size:11px;width:2.5em;
                text-align:right">75</span>
      </div>
      <div class="row" style="margin-top:6px">
        <label style="font-size:12px; display:flex; align-items:center; gap:5px">
          <input type="checkbox" id="skipsw" checked
                 onchange="setSkipUnchanged(this.checked)">
          画面停止检测（静止时不重发）
        </label>
      </div>

      <!-- 分辨率（降采样宽度）。中途可改，不用重连 —— 走 WS 的 maxWidth 命令。
           注意它和「触控坐标空间」是两回事：图变小了，但坐标仍按
           触控范围算，否则点击会错位（见 toScreen 那段）。 -->
      <div class="row" style="margin-top:6px">
        <span class="dim" style="font-size:11px;align-self:center">分辨率</span>
        <button id="mw0"   onclick="setMaxWidth(0)">原始</button>
        <button id="mw720" onclick="setMaxWidth(720)">720</button>
        <button id="mw480" onclick="setMaxWidth(480)">480</button>
        <button id="mw360" onclick="setMaxWidth(360)">360</button>
        <span id="mwval" class="dim" style="font-size:11px"></span>
      </div>

      <!-- 屏幕方向。0/90/180/270 —— 真的转设备方向（应用会重新布局）。
           有的设备转不动（ROM 没有旋转支持），那时服务端会退到换显示尺寸，
           并在应答里如实说走了哪条路；下面那行会把结果显示出来。 -->
      <div class="row" style="margin-top:6px">
        <span class="dim" style="font-size:11px;align-self:center">方向</span>
        <button onclick="rotate('0')"   title="竖屏">0°</button>
        <button onclick="rotate('90')"  title="横屏">90°</button>
        <button onclick="rotate('180')" title="倒竖">180°</button>
        <button onclick="rotate('270')" title="倒横">270°</button>
        <button onclick="rotate('free')" title="跟随传感器">自动</button>
      </div>
      <div id="rotmsg" class="dim" style="font-size:11px;margin-top:4px"></div>
    </div>

    <div class="card">
      <h2>按键</h2>
      <div class="row">
        <button onclick="key('home')">Home</button>
        <button onclick="key('back')">返回</button>
        <button onclick="key('appswitch')">最近</button>
        <button onclick="key('menu')">菜单</button>
        <button onclick="key('enter')">回车</button>
        <button onclick="key('power')">电源</button>
        <button onclick="key('volumeup')">音量+</button>
        <button onclick="key('volumedown')">音量−</button>
        <button onclick="key('up')">↑</button>
        <button onclick="key('down')">↓</button>
        <button onclick="key('left')">←</button>
        <button onclick="key('right')">→</button>
        <button onclick="key('backspace')">⌫</button>
        <button onclick="key('tab')">Tab</button>
        <button onclick="key('space')">空格</button>
      </div>
      <div class="row" style="margin-top:8px">
        <input type="text" id="keytext" placeholder="输入键名或键码，回车发送"
               onkeydown="if(event.key==='Enter'){key(this.value);this.value='';}">
      </div>
      <div class="dim" style="margin-top:6px;font-size:11px">
        键名见 /api/v1/describe；也可直接填数字键码。长按用
        <button style="padding:1px 5px;font-size:11px"
                onclick="keyLong()">长按最后输入的键</button>
      </div>
    </div>

    <div class="card">
      <h2>剪贴板</h2>
      <div id="clip" class="dim">（点「读剪贴板」查看）</div>
      <div class="row" style="margin-top:8px">
        <input type="text" id="cliptext" placeholder="要写入的文本">
        <button onclick="clipSet()">写入</button>
        <button onclick="clipGet()">读取</button>
      </div>
    </div>

    <div class="card" id="card-apps">
      <h2 class="clickable" onclick="toggleCard('card-apps')">
        <span class="arrow">▼</span>应用
      </h2>
      <div class="body">
      <div class="row">
        <label style="font-size:12px"><input type="checkbox" id="appSys"> 含系统应用</label>
        <button onclick="loadApps()">刷新列表</button>
        <button onclick="$('apkfile').click()">上传安装 APK</button>
        <input type="file" id="apkfile" accept=".apk,application/vnd.android.package-archive"
               style="display:none" onchange="uploadApk(this)">
        <label style="font-size:12px;align-self:center">
          <input type="checkbox" id="apkReplace" checked>
          覆盖安装
        </label>
      </div>
      <div id="apkmsg" class="dim" style="font-size:12px;margin-top:5px"></div>
      <div id="apps" class="dim" style="margin-top:6px;font-size:12px">
        （点「刷新列表」加载）
      </div>
      </div>
    </div>

    <div class="card" id="card-files">
      <h2 class="clickable" onclick="toggleCard('card-files')">
        <span class="arrow">▼</span>文件
      </h2>
      <div class="body">
        <div class="row">
          <button onclick="fsGoto(fsRoot)" title="跳到 app 能读到的存储根">根目录</button>
          <button onclick="fsGoto(fsUp())">上级</button>
          <button onclick="fsLoad()">刷新</button>
          <button onclick="fsMkdir()">新建目录</button>
        </div>
        <!-- 当前路径。绝对路径 —— 加了存储根之后"相对谁"不再唯一。 -->
        <div id="fsPath" class="dim"
             style="font-size:11px;margin-top:5px;word-break:break-all"></div>
        <div id="fsList" class="dim" style="margin-top:5px;font-size:12px;
             max-height:230px;overflow:auto"></div>
        <div id="fsMsg" class="dim" style="font-size:12px;margin-top:5px"></div>
      </div>
    </div>

    <div class="card" id="card-running">
      <h2 class="clickable" onclick="toggleCard('card-running')">
        <span class="arrow">▼</span>运行中的应用
      </h2>
      <div class="body">
      <div class="row">
        <button onclick="loadRunning()">刷新</button>
        <label style="font-size:12px;align-self:center">
          <input type="checkbox" id="runAuto" onchange="setRunAuto(this.checked)">
          自动刷新
        </label>
        <label style="font-size:12px;align-self:center">
          <input type="checkbox" id="runSys" onchange="loadRunning()">
          含系统应用
        </label>
      </div>
      <div id="running" class="dim" style="margin-top:6px;font-size:12px">
        （点「刷新」加载）
      </div>
      </div>
    </div>

    <div class="card">
      <h2>日志</h2>
      <div class="row">
        <button onclick="logClear()">清屏</button>
        <button onclick="logHistory()">读历史文件</button>
        <label style="font-size:12px;align-self:center">
          <input type="checkbox" id="logAuto" checked
                 onchange="setLogAuto(this.checked)">
          实时
        </label>
      </div>
      <div id="logbox"></div>
    </div>

    <div class="card">
      <h2>电源</h2>
      <div class="row">
        <button onclick="power('reboot')">重启</button>
        <button onclick="power('shutdown')">关机</button>
        <button onclick="power('reboot-recovery')">重启到 recovery</button>
      </div>
      <div class="dim" style="margin-top:6px;font-size:11px">
        设备会立即执行，请先确认没有未保存的工作。
      </div>
    </div>

    <div class="card">
      <h2>操作提示</h2>
      <div class="dim" style="font-size:12px">
        在画面上：<b>单击</b>=点击 · <b>按住不动</b>=长按 ·
        <b>按住拖动</b>=拖拽
      </div>
    </div>
  </div>
</main>

<script>
const $ = (id) => document.getElementById(id);
// 默认 30。
//
// ⚠️ 这里原来是 5，而按钮最高只到 10 —— 服务端明明能跑 30fps，
//    网页却一直在放幻灯片。实测踩过：命令行 curl 测出来 30fps，
//    但页面上"卡到爆炸"，就是因为这个默认值。
//    帧率不该由前端偷偷限死，服务端会按自己的能力截断。
let fps = 30;
// 编码格式与质量。
//
// ⚠️ 这两个必须在**所有使用者之前**声明。用 let 声明的变量在声明前
//    处于 TDZ，读它会抛 ReferenceError；而 streamUrl() 里有
//    `'&format=' + codec`，它在 try 里被调用 —— 于是异常被吞掉、
//    WebSocket 根本没创建，页面上只显示"画面流断开"，
//    看不出任何原因。实测就是这么坑了半天。
let codec = 'jpeg';

// ── 画质拖动条 ──
//
// ⚠️ 范围**跟着格式走**，不能写死。
//
//    PNG 的 quality 是 zlib 压缩级别（1-9），JPEG/WebP 是 1-100，
//    H.264 那边会被换算成码率。写死 1-100 的话：
//      - 选 PNG 时拖到 75，服务端只认 9，用户看到的数字和实际不符
//      - 拖到 1 再切回 JPEG 就变成"质量 1"，画面糊成马赛克
//
//    范围从 /api/v1/params 的 quality 段拿（服务端是唯一权威）。
let qualityRange = {
  jpeg: { min: 1, max: 100, default: 75 },
  webp: { min: 1, max: 100, default: 80 },
  png:  { min: 1, max: 9,   default: 1 },
  h264: { min: 1, max: 100, default: 75 },
};

function syncQualitySlider() {
  const r = qualityRange[codec] || { min: 1, max: 100, default: 75 };
  const el = document.getElementById('qslider');
  const lab = document.getElementById('qval');
  if (!el) return;

  // 换格式时把当前值钳进新范围，而不是留着越界的旧值
  if (!(quality >= r.min && quality <= r.max)) {
    quality = r.default;
  }
  el.min = r.min;
  el.max = r.max;
  el.value = quality;
  if (lab) lab.textContent = quality;
}

// 拖动时的节流句柄
let qTimer = null;
let qPending = null;

function onQualitySlide(v) {
  const q = parseInt(v, 10);
  if (isNaN(q)) return;
  quality = q;
  const lab = document.getElementById('qval');
  if (lab) lab.textContent = q;

  // ⚠️ 必须节流。拖动时 oninput 每秒能触发几十次，每次都发一条
  //    WebSocket 消息的话，服务端要不停地重配编码器（H.264 还要
  //    重建），画面会一顿一顿的，而且用户根本感觉不到中间那些值。
  qPending = q;
  if (qTimer) return;
  qTimer = setTimeout(() => {
    qTimer = null;
    const val = qPending;
    qPending = null;
    if (streamReady && streamWs) {
      streamWs.send(JSON.stringify({ t: 'quality', v: val }));
    }
  }, 120);
}

// ── H.264 / WebCodecs ────────────────────────────────────────────────────
//
// H.264 的帧比 JPEG 小两个数量级（P 帧几十~几百字节 vs 11 KB），
// 但它**必须由 WebCodecs 解码**，而那需要 Chrome/WebView 94+。
//
// 设备上的 WebView 是 91 —— 也就是说在**设备本机**打开这个控制台
// 用不了 H.264；从桌面浏览器打开可以（那才是常规用法）。
//
// 所以要**探测 + 回退**：探测不到就禁用按钮，别让用户选到一个永远
// 黑屏的选项；中途发现不支持也要退回去。
const hasWebCodecs = (typeof VideoDecoder !== 'undefined' &&
                      typeof EncodedVideoChunk !== 'undefined');

let h264Dec = null;        // VideoDecoder 实例
let h264Ready = false;     // 配置成功、可以喂数据了
let h264Ts = 0;            // 时间戳（WebCodecs 要求单调递增）

function stopH264() {
  if (h264Dec) {
    try { h264Dec.close(); } catch (e) {}
    h264Dec = null;
  }
  h264Ready = false;
  h264Ts = 0;
}

// 一段 Annex-B 里有没有 IDR（NAL 类型 5）？
//
// WebCodecs 的 EncodedVideoChunk 必须标明 key/delta，而服务端发的是
// 裸 Annex-B，没有额外元信息 —— 只能自己扫 NAL 头。
function hasIdr(buf) {
  const d = new Uint8Array(buf);
  for (let i = 0; i + 4 < d.length; i++) {
    if (d[i] === 0 && d[i + 1] === 0 &&
        (d[i + 2] === 1 || (d[i + 2] === 0 && d[i + 3] === 1))) {
      const off = (d[i + 2] === 1) ? i + 3 : i + 4;
      if (off < d.length && (d[off] & 0x1F) === 5) return true;
    }
  }
  return false;
}

// 配置 H.264 解码器。
//
// **异步**，因为要用 VideoDecoder.isConfigSupported 真探一次 ——
// 光看 `typeof VideoDecoder` 是不够的：
//   Firefox 从 130 起有 VideoDecoder，但 H.264 的解码能力依赖平台，
//   在 Linux 上可能是 unsupported。Chromium 同理（受编译开关影响）。
//   不探就 configure 的话，报错信息是 "not supported"，而用户看到的
//   只是黑屏。
async function startH264(codecStr) {
  stopH264();
  if (!hasWebCodecs) return false;

  let cfg = { codec: codecStr, optimizeForLatency: true };
  if (sw > 0 && sh > 0) { cfg.codedWidth = sw; cfg.codedHeight = sh; }

  try {
    const sup = await VideoDecoder.isConfigSupported(cfg);
    dbg('isConfigSupported(' + codecStr + ') = ' + JSON.stringify(sup.supported));
    if (!sup.supported) {
      setStatus('这个浏览器不支持 H.264 解码（' + codecStr +
                '）—— 可能是平台没有 H.264 解码器', true);
      return false;
    }
  } catch (e) {
    dbg('isConfigSupported 抛异常: ' + e);
    return false;
  }

  try {
    h264Dec = new VideoDecoder({
      output: (frame) => {
        if (cvs.width !== frame.displayWidth || cvs.height !== frame.displayHeight) {
          cvs.width = frame.displayWidth;
          cvs.height = frame.displayHeight;
          sw = cvs.width; sh = cvs.height;
          updateMeta();
        }
        ctx.drawImage(frame, 0, 0);
        frame.close();     // 不 close 会攒着不放，几秒就吃满内存
        ++streamFrames;
      },
      error: (e) => {
        dbg('VideoDecoder 错误: ' + e);
        h264Ready = false;
        setStatus('H.264 解码器出错，退回 JPEG', true);
        fallbackToJpeg();
      }
    });
    // optimizeForLatency：别为了重排攒缓冲。画面流要的是低延迟。
    h264Dec.configure(cfg);
    h264Ready = true;
    dbg('H.264 解码器已配置: ' + codecStr);
    return true;
  } catch (e) {
    dbg('VideoDecoder configure 失败: ' + e);
    stopH264();
    return false;
  }
}

// 退回 JPEG 并重连。只在确认不支持时调，避免来回切。
let fallingBack = false;
function fallbackToJpeg() {
  if (fallingBack || codec !== 'h264') return;
  fallingBack = true;
  codec = 'jpeg';
  stopH264();
  stopStream();
  setTimeout(() => { fallingBack = false; startStream(); pushStreamParams(); }, 400);
}
let quality = 75;
// 降采样宽度。设备屏幕往往比展示区域大得多，全分辨率纯属浪费带宽。
let maxW = 720;
let sw = 0, sh = 0;          // **流**的画面尺寸（降采样后）
let dispW = 0, dispH = 0;    // 屏幕逻辑尺寸（/config 报的，二者不是一回事）
let streamKey = 0;

// 画面流的诊断信息。写进 DOM 而不是 console.log ——
// WebView shell 不转发 console.log，只有未捕获异常才进 logcat，
// 所以"流为什么断了"在那边完全看不到。
let streamDbg = '';
function dbg(t) {
  streamDbg = t;
  const el = $('dbg');
  if (el) el.textContent = streamDbg;
}

// 服务端抓帧节奏。
//
// 和顶栏那个 shownFps 是**两回事**：那个是本页实际收到多少帧，
// 这个是设备被拉到了多快。两个数不一致本身就是要看的信息 ——
// 服务端跑得比本页需求高，说明有别的客户端在拉，或者本页在丢帧。
function renderCadence(c) {
  const el = $('cadence');
  if (!el) return;
  if (!c || !c.running) {
    el.textContent = '抓帧  空闲（没有订阅者，一次都没在抓）';
    el.style.color = '';
    return;
  }
  const n = (c.subscriberList || []).length;
  let t = '抓帧  服务端 ' + c.activeFps + 'fps · ' + n + ' 个订阅'
        + ' · ' + c.lastCaptureMs + 'ms';
  if (c.captureWidth > 0) t += ' · 宽 ' + c.captureWidth;

  // 服务端比本页需求高 → 有别人在拉，或者本页跟不上。
  // 这正是"看不出是谁在拉"要提醒的那种情况，标黄。
  const overshoot = c.activeFps > fps + 1;
  if (overshoot) {
    t += '  ⚠ 高于本页 ' + fps + 'fps';
    el.style.color = '#e0a020';
  } else {
    el.style.color = '';
  }
  el.textContent = t;
}

function setStatus(t, err) {
  const el = $('status');
  el.textContent = t;
  el.className = err ? 'err' : 'ok';
}

// ── 状态行 ──
//
// 把三样东西拼在一起：设备信息、实际收到的帧率、触控往返延迟。
// 后两个才是用户"感觉卡不卡"的直接依据 —— 服务端说 30fps 不等于
// 客户端真收到了 30 帧。
let metaInfo = '';
let lastCap = null, lastPid = 0, lastProto = '';
// ⚠️ 每次都**重算** metaInfo，不能"第一次算好之后只拼后半段"。
//
//    早先的写法是 `updateMeta(r)` 传 r 时算一次 metaInfo，之后不带参数
//    调用（画面尺寸变了就会调）就复用旧值 —— 于是顶栏一直显示**旧的**
//    设备尺寸。实测：流已经是 1280x720，顶栏还写着 720x405（那是首次
//    渲染时的降采样尺寸），看着像设备尺寸不对。
//
//    和之前 sw/sh 被屏幕尺寸覆盖是同一类问题：**派生值必须跟着源走**。
function updateMeta(r) {
  if (r) {
    const cap = r.runtime ? r.runtime.capture : r.capture;
    lastCap = cap;
    lastPid = r.runtime.pid;
    lastProto = r.runtime.protocolVersion;
  }
  if (lastCap) {
    // 画面尺寸优先（sw/sh 是**流**的尺寸，降采样后可能小于屏幕）；
    // 还没有画面时退回屏幕逻辑尺寸。
    metaInfo = (sw || lastCap.primaryWidth || '?') + '×' + (sh || lastCap.primaryHeight || '?')
             + ' · ' + lastCap.backend
             + ' · pid ' + lastPid
             + ' · 协议 v' + lastProto;
  }
  const stream = streamReady ? (' · ' + shownFps + 'fps') : ' · 画面流断开';
  const touch  = wsReady ? (' · ' + rtt + 'ms') : ' · 触控流断开';
  $('meta').textContent = metaInfo + stream + touch;
}

function refresh() {
  pollCadence();
  api('/config').then(d => {
    const r = d.runtime;
    const cap = r.capture;
    // ⚠️ 不要用**屏幕尺寸**覆盖 sw/sh —— 那是**流的尺寸**。
    //
    //    降采样之后两者不一样：maxWidth=360 时流是 360 宽，而屏幕是 720。
    //    覆盖了的话，控制台显示的尺寸每 10 秒（refresh 的周期）就会跳回
    //    720 一次，toScreen 的兜底值也会跟着错。
    //    这是实测抓到的：改到 480 之后读数变成了 720。
    dispW = cap.primaryWidth || dispW;
    dispH = cap.primaryHeight || dispH;
    // 还没有画面时用屏幕尺寸占位，有画面了就一切以流为准
    if (!sw || !sh) { sw = dispW; sh = dispH; }
    updateMeta(d);
  }).catch(e => setStatus('取状态失败：' + e, true));
}

// ── 访问令牌 ──
//
// 存在 localStorage 里：开启鉴权后每次打开页面都要重新粘贴令牌的话
// 没人受得了。服务端只对 /api/ 下的请求校验，网页本身不校验 ——
// 所以页面能打开、再由脚本补上令牌。
let token = localStorage.getItem('autod_token') || '';

function authHeaders(extra) {
  const h = Object.assign({}, extra || {});
  if (token) h['Authorization'] = 'Bearer ' + token;
  return h;
}

// 网页的 WebSocket 不能自定义请求头，所以令牌走查询参数。
// 服务端两种都收（见 HttpServer::CheckAuth）。
function withToken(url) {
  if (!token) return url;
  return url + (url.indexOf('?') >= 0 ? '&' : '?') + 'token=' + encodeURIComponent(token);
}

function api(path, opts) {
  const o = Object.assign({}, opts || {});
  o.headers = authHeaders(o.headers);
  return fetch('/api/v1' + path, o).then(r => {
    if (r.status === 503) {
      // 服务被关了。这不是错误，是用户自己按的 —— 把开关状态同步过来，
      // 而不是弹一堆失败提示。
      serving = false;
      updateServiceUi('服务已关闭对外能力');
      throw new Error('服务已关闭');
    }
    if (r.status === 401) {
      // 令牌缺失或不对 —— 把输入条亮出来，别让用户对着一个
      // 什么都点不动的页面猜
      $('authbar').style.display = 'flex';
      token = '';
      localStorage.removeItem('autod_token');
      throw new Error('需要访问令牌');
    }
    return r.json();
  });
}

function saveToken() {
  token = $('tokeninput').value.trim();
  localStorage.setItem('autod_token', token);
  $('authbar').style.display = token ? 'none' : 'flex';
  setStatus(token ? '令牌已保存' : '令牌已清除');
  // 两条长连接要重连才会带上新令牌
  if (streamWs) { try { streamWs.close(); } catch (e) {} streamWs = null; }
  if (ws) { try { ws.close(); } catch (e) {} ws = null; }
  refresh();
  startStream();
  connectTouch();
}

// ── 实时画面（WebSocket）──
//
// 画面走 WebSocket + canvas，不走 MJPEG 的 <img>：
//
//   MJPEG 让浏览器把一条 multipart 流当成"会不断更新的图"，
//   省事但可控性差 —— 解码和绘制都排在浏览器的图片管道里，
//   帧率只能靠重设 src 来改，而且没法测延迟。
//
//   WebSocket 收到的是二进制图片帧，用 createImageBitmap 解码到
//   canvas。解码在 worker 线程上，绘制是同步的，链路更短。
//   客户端还能反过来控制流（改帧率/画质不用重连）。
//
// MJPEG 那条留着（/api/v1/stream 不带 Upgrade 头就是它），
// 用来嵌到别的页面或者调试最省事，也是下面断线时的兜底。

const cvs = $('screen');
const ctx = cvs.getContext('2d', { alpha: false, desynchronized: true });

let streamWs = null;
let streamReady = false;
let streamFrames = 0;
let lastFpsAt = performance.now();
let shownFps = 0;

function streamUrl() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  return withToken(proto + '//' + location.host + '/api/v1/stream'
       + '?fps=' + fps + '&format=' + codec + '&quality=' + quality
       + '&maxWidth=' + maxW + '&skipUnchanged=' + (skipUnchanged ? 1 : 0));
}

function startStream() {
  if (fps <= 0) { stopStream(); return; }
  if (streamReady) { pushStreamParams(); return; }
  if (streamWs) return;                      // 正在连接

  let url;
  try {
    url = streamUrl();
  } catch (e) {
    // 构造 URL 就失败 —— 多半是某个参数没定义（TDZ 或拼错）。
    // 必须留下痕迹：只 setStatus 的话会被随后的触控流连接覆盖掉，
    // 用户看到的只是"画面流断开"，无从下手。
    dbg('url 构造失败: ' + e);
    setStatus('画面流地址构造失败：' + e, true);
    return;
  }
  try { streamWs = new WebSocket(url); }
  catch (e) { dbg('new WebSocket 失败: ' + e); setStatus('画面流建立失败：' + e, true); return; }
  // ⚠️ 必须是 arraybuffer，不能是 blob。
  //
  //    EncodedVideoChunk 的 data 要 **BufferSource**：
  //      - 传 Blob 直接抛 TypeError
  //      - new Uint8Array(blob) 得到的是**空数组**（不是内容）——
  //        于是 hasIdr() 永远返回 false，所有帧都被标成 delta
  //
  //    两条加在一起 = H.264 在任何浏览器里都开不了（实测就是这样）。
  //
  //    JPEG 那边 createImageBitmap 收不了 ArrayBuffer，
  //    所以在解码处包一层 Blob —— 一次分配，比在这里 await 转换安全：
  //    await 会让 onmessage 交错执行，H.264 的帧是有顺序依赖的。
  streamWs.binaryType = 'arraybuffer';

  streamWs.onopen = () => {
    streamReady = true;
    dbg('open');
    setStatus('画面流已连接');
    updateMeta();
  };
  streamWs.onclose = (e) => {
    streamReady = false;
    streamWs = null;
    // 把关闭码打进控制台。WebSocket 断了但页面上只会显示一句
    // "断开"，光看那个分不清是被服务端关的、握手失败、还是网络问题。
    dbg('close code=' + e.code + ' clean=' + e.wasClean + ' r=' + (e.reason || ''));
    updateMeta();
    if (fps > 0) setTimeout(startStream, 1500);   // 自动重连
  };
  streamWs.onerror = (e) => {
    streamReady = false;
    dbg('error ' + (e && e.message ? e.message : '(无消息)'));
  };

  streamWs.onmessage = async (ev) => {
    // 文本消息是控制信息，二进制才是图
    if (typeof ev.data === 'string') {
      dbg('text ' + ev.data.slice(0, 60));
      let m; try { m = JSON.parse(ev.data); } catch (e) { return; }
      if (m.t === 'size') {
        // 服务端在尺寸**变化时**发这条 —— 第一帧之前、以及之后每次
        // 改 maxWidth / 设备转屏 / 改分辨率都会发。
        // 客户端据此建/改 canvas，否则得等图到了才知道多大。
        if (cvs.width !== m.w || cvs.height !== m.h) {
          cvs.width = m.w; cvs.height = m.h;
          sw = m.w; sh = m.h;
          updateMeta();
          syncMaxWidthButtons();
          applyPanelForAspect();
          // ⚠️ 画面尺寸变了，**触控坐标空间多半也变了** —— 服务端转屏时会
          //    重建注入器让坐标范围跟着显示走。必须重取，否则之后的点击
          //    全按旧空间换算（转屏前能点中，转屏后全偏）。
          //
          //    放在这里而不是只放在 rotate() 里：旋转可能是**别人**触发的
          //    —— 另一个客户端调 API、上游程序调、甚至 adb 改的分辨率。
          //    页面只认服务端的通知，不认"是不是我按的按钮"。
          loadTouchRange();
        }
      } else if (m.t === 'codec') {
        // 服务端在**第一帧之前**告诉 codec 串 —— WebCodecs 必须要它，
        // 而各设备的 profile/level 不同，不能写死。
        if (!(await startH264(m.codec))) {
          setStatus('这个浏览器不支持 H.264（需要 WebCodecs），已退回 JPEG', true);
          dbg('没有 WebCodecs，退回 JPEG');
          fallbackToJpeg();
        } else {
          setStatus('画面流 H.264（' + m.codec + '）');
        }
      } else if (m.t === 'hello') {
        setStatus('画面流 ' + m.format + ' @' + m.fps + 'fps');
      }
      return;
    }

    // H.264：喂给 WebCodecs，不走 createImageBitmap
    if (h264Ready && h264Dec) {
      try {
        h264Dec.decode(new EncodedVideoChunk({
          type: hasIdr(ev.data) ? 'key' : 'delta',
          timestamp: (h264Ts += 33333),   // 微秒；单调递增即可
          data: ev.data                   // ArrayBuffer（见 binaryType 那处）
        }));
      } catch (e) {
        if (streamFrames === 0) dbg('H.264 decode 失败: ' + e);
      }
      return;
    }
    // 选了 H.264 但解码器还没起来（codec 消息没到 / 配置失败）：
    // 这些是裸 H.264 字节，当图片解只会报错 —— 直接丢。
    if (codec === 'h264') return;

    // 二进制帧：解码到 canvas
    if (streamFrames === 0) {
      dbg('binary ' + (ev.data.size || ev.data.byteLength) + 'B cIB='
          + (typeof createImageBitmap));
    }
    try {
      const bmp = await createImageBitmap(new Blob([ev.data]));
      if (cvs.width !== bmp.width || cvs.height !== bmp.height) {
        cvs.width = bmp.width; cvs.height = bmp.height;
        sw = bmp.width; sh = bmp.height;
      }
      ctx.drawImage(bmp, 0, 0);
      bmp.close();                 // 不 close 会攒着不放，几分钟就吃满内存
      ++streamFrames;
    } catch (e) {
      // 解码失败只丢这一帧。但如果是 API 不可用，会每帧都失败 ——
      // 那时候页面就是一片黑，而没有任何提示。所以记一次。
      if (streamFrames === 0) dbg('decode failed: ' + e);
    }
  };
}

function pushStreamParams() {
  if (!streamReady) return;
  streamWs.send(JSON.stringify({t: 'fps', v: fps}));
  streamWs.send(JSON.stringify({t: 'quality', v: quality}));
  streamWs.send(JSON.stringify({t: 'format', v: codec}));
}

function stopStream() {
  stopH264();
  if (streamWs) { try { streamWs.close(); } catch (e) {} streamWs = null; }
  streamReady = false;
  setStatus('画面流已暂停');
  updateMeta();
}

// 实际收到的帧率。服务端说 30fps 不等于客户端真收到了 30 帧 ——
// 网络丢包、解码跟不上都会让它更低，而这个数才是用户看到的。
setInterval(() => {
  const now = performance.now();
  const dt = (now - lastFpsAt) / 1000;
  if (dt > 0) shownFps = Math.round(streamFrames / dt);
  streamFrames = 0;
  lastFpsAt = now;
  updateMeta();
}, 1000);

function setFps(v) {
  fps = v;
  if (v <= 0) { stopStream(); return; }
  startStream();
  pushStreamParams();
  setStatus('流帧率：' + v + ' fps');
}


// 没有 WebCodecs 就把 H.264 按钮置灰。
//
// 只在点击时报错是不够的 —— 用户会以为"这个选项应该有画面"，
// 点几下才发现不行。置灰 + title 说明是更清楚的表达。
// ⚠️ **不自动置灰**。
//
//    能探测到"这个浏览器没有 VideoDecoder"，但那不该成为**禁止**用户
//    尝试的理由：
//      - 探测本身可能不准（不同版本/前缀/实验开关）
//      - 用户可能知道自己环境里有什么（比如准备换浏览器再点）
//      - 大不了就是放不出来，页面会明确说出原因
//
//    替他做决定，比让他自己试一次更糟。按钮旁边给个提示就够了。
if (!hasWebCodecs) {
  const b = document.getElementById('btn-h264');
  if (b) {
    b.title = '这台浏览器的 WebCodecs 不可用（VideoDecoder 未定义），'
            + '点了可能无法播放 —— H.264 需要 Chrome/WebView 94+。'
            + '也可以用 ?format=h264 强制启动。';
  }
}

// ── URL 参数 ──
//
// 给一个**从地址栏直接启动 H.264** 的方式：
//
//   http://<设备>:8088/?format=h264
//   http://<设备>:8088/?format=h264&fps=30&quality=75
//   http://<设备>:8088/?format=webp&fps=15
//
// 比"打开页面再找按钮点"可靠 —— 尤其设备内 WebView 里页面很长、
// 按钮要滚很久才看得到的时候。
function applyUrlParams() {
  let q;
  try { q = new URLSearchParams(location.search); } catch (e) { return; }

  const wantFmt = q.get('format');
  if (wantFmt) {
    const f = wantFmt.toLowerCase();
    if (['jpeg', 'jpg', 'webp', 'png', 'h264', 'auto'].indexOf(f) >= 0) {
      codec = (f === 'jpg') ? 'jpeg' : f;
    }
  }
  const wantFps = parseInt(q.get('fps') || '', 10);
  if (!isNaN(wantFps) && wantFps >= 1 && wantFps <= 60) fps = wantFps;

  const wantQ = parseInt(q.get('quality') || '', 10);
  if (!isNaN(wantQ) && wantQ >= 1 && wantQ <= 100) {
    quality = wantQ;
  } else if (wantFmt) {
    // ⚠️ 换了格式必须配套调整 quality —— 各格式的量纲完全不同：
    //      PNG 是 zlib 级别 1-9，JPEG/WebP 是 1-100，
    //      而 H.264 那边 quality 会被换算成码率。
    //
    //    不调的话就会出现"URL 指定 h264、但 quality 还是页面默认的 1"
    //    → 码率算出 35kbps → 钳到下限 200kbps
    //    → 720x1480 在这个码率下几乎没有输出，客户端一帧都收不到。
    //    （实测踩过：hello 收到了、codec 消息也发了，就是没有画面。）
    quality = (codec === 'png') ? 1 : (codec === 'webp') ? 80 : 75;
  }

  const wantW = parseInt(q.get('maxWidth') || '', 10);
  if (!isNaN(wantW) && wantW >= 0) maxW = wantW;

  if (q.get('skipUnchanged') === '0') skipUnchanged = false;
  if (q.get('skipUnchanged') === '1') skipUnchanged = true;

  if (wantFmt || q.get('fps') || q.get('quality')) {
    dbg('URL 参数: format=' + codec + ' fps=' + fps + ' quality=' + quality);
  }

  syncQualitySlider();
}

// 从服务端取每种格式的 quality 范围。
//
// 不在前端写死：范围是服务端定的（PNG 走 zlib 级别、H.264 换算码率），
// 前端写死就会和实际不符。拿不到就用内置兜底，不影响使用。
// 抓帧节奏单独轮询。
//
// 只在打开页面时取一次是不够的 —— "设备现在被拉到多快"随时在变，
// 而这一行存在的意义就是**实时**看出有没有别人在拉。
// 2 秒一次：够及时，又不会把 /params 变成负担（几百字节）。
function pollCadence() {
  fetch(withToken('/api/v1/params'))
    .then(r => r.json())
    .then(d => { if (d) renderCadence(d.capture); })
    .catch(() => {});   // 取不到就保持上一次的值，别刷错误
}

function loadQualityRange() {
  fetch(withToken('/api/v1/params'))
    .then(r => r.json())
    .then(d => {
      if (!d) return;
      renderCadence(d.capture);
      if (!d.quality) return;
      for (const k of Object.keys(d.quality)) {
        const v = d.quality[k];
        if (v && typeof v.min === 'number' && typeof v.max === 'number') {
          qualityRange[k] = { min: v.min, max: v.max,
                              default: (typeof v.default === 'number')
                                       ? v.default : v.min };
        }
      }
      syncQualitySlider();
      dbg('quality 范围: ' + JSON.stringify(qualityRange[codec]));
    })
    .catch(e => dbg('取 quality 范围失败: ' + e));
}

function setCodec(c, q) {
  // 不拦。浏览器不支持就放不出来，那时候会给出明确原因并退回 JPEG ——
  // 但"让不让试"这个决定权在用户手里。
  if (c === 'h264' && !hasWebCodecs) {
    dbg('注意：这个浏览器没有 VideoDecoder，H.264 很可能放不出来');
  }
  codec = c; quality = q;
  syncQualitySlider();     // 换了格式，范围也要跟着换
  startStream();
  pushStreamParams();
  setStatus('编码：' + c.toUpperCase() + ' 质量 ' + q);
}

// ── 流式触控 ──
//
// 为什么用 WebSocket 而不是每个手势一个 POST：
//
//   一次 POST 要 TCP 往返 + HTTP 头解析 + 分发。拖拽时每个移动点都这么
//   来一遍，手感就是"一顿一顿"的；而且服务端只能等整个手势发完才知道
//   轨迹，做不到实时。
//
//   WebSocket 建一次连接，之后每个触控点就是一个几字节的帧。
//   实测往返延迟中位 1.0ms（HTTP POST 要 5-15ms）。
//
// 三个原语：down / move / up。按下就发 down，指针动了就发 move，
// 抬起发 up —— 服务端收到立刻注入，不再等"整个手势"。

let ws = null;
let wsReady = false;
let rtt = 0;
let seq = 0;
const pendingPings = new Map();

function wsUrl() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  return withToken(proto + '//' + location.host + '/api/v1/touch');
}

function connectTouch() {
  try { ws = new WebSocket(wsUrl()); }
  catch (e) { setStatus('触控流建立失败：' + e, true); return; }

  ws.onopen = () => {
    wsReady = true;
    setStatus('触控流已连接');
    // 立刻发一个心跳，页面上能马上看到延迟
    ping();
  };
  ws.onclose = () => {
    wsReady = false;
    // 自动重连。断线时用户还在拖拽的话，重连后状态是干净的
    // （设备侧那条手势已经在 up 或超时里结束了）。
    setTimeout(connectTouch, 1500);
  };
  ws.onerror = () => { wsReady = false; };
  ws.onmessage = (ev) => {
    let m;
    try { m = JSON.parse(ev.data); } catch (e) { return; }
    if (m.t === 'pong') {
      const sent = pendingPings.get(m.s);
      if (sent !== undefined) {
        rtt = Math.round(performance.now() - sent);
        pendingPings.delete(m.s);
        updateMeta();
      }
      return;
    }
    if (m.ok === false) setStatus('触控失败：' + (m.error || '未知'), true);
  };
}

function ping() {
  if (!wsReady) return;
  const s = ++seq;
  pendingPings.set(s, performance.now());
  if (pendingPings.size > 16) {         // 防止断线期间无限堆积
    const first = pendingPings.keys().next().value;
    pendingPings.delete(first);
  }
  send({t: 'ping', s: s});
}

function send(obj) {
  if (!wsReady) return false;
  ws.send(JSON.stringify(obj));
  return true;
}

setInterval(ping, 2000);

// ── 指针 → 触控事件 ──
//
// 坐标换算必须按**渲染后的显示尺寸**，不能用 naturalWidth：
// 画面会被 CSS 缩放过。换算后还要夹到屏幕范围内，
// 否则边缘点击会越界（设备侧的注入会失败或落到屏幕外）。
// 触控坐标空间 —— **不是图尺寸**。
//
// 服务端把 x/y 直接当 uinput 的 ABS 值写下去（inject_uinput.cpp 里
// 没有缩放），范围是启动时定的 touchWidth×touchHeight（默认 = 屏幕分辨率）。
// Android 再把这个范围映射到当前显示尺寸 —— 也就是说它是一个
// **归一化坐标空间**，换分辨率/转屏之后依然成立。
//
// 所以预览被降采样（maxWidth=360）之后，图是 360 宽，
// 但坐标仍要按 720 算。按图尺寸算的话，点画面正中央会落到
// 左上角四分之一处（实测 ABS 收到 180,320 而不是 360,640）。
let touchW = 0, touchH = 0;

function loadTouchRange() {
  fetch(withToken('/api/v1/info'))
    .then(r => r.json())
    .then(d => {
      if (d && d.touchWidth > 0 && d.touchHeight > 0) {
        touchW = d.touchWidth; touchH = d.touchHeight;
        dbg('触控坐标空间: ' + touchW + 'x' + touchH);
      }
    })
    .catch(() => {});   // 拿不到就退回图尺寸
}

// canvas 里**真正画着画面**的那块矩形（客户端坐标）。
//
//   适应（默认）  元素是 width/height:auto + max-width/height ——
//                 盒子本身就是按比例缩过的画面，两者相等
//   铺满          object-fit:contain —— 元素盒子是整个显示区，
//                 画面缩在中间，四周是黑边
//
// 两种模式都能用同一个式子算：把 iw x ih 按 contain 塞进盒子。
// 盒子比例本来就对的时候，结果就等于盒子本身 —— 所以不必分模式。
//
// ⚠️ 为什么不能直接拿 getBoundingClientRect()：
//    铺满模式下它给的是**整个显示区**（100% x 100%），画面只占中间一块。
//    拿它算比例，黑边会被当成画面的一部分，点得越靠边错得越多。
//    只点正中央查不出来 —— 两种算法在中心点重合。
function contentRect() {
  const r = cvs.getBoundingClientRect();
  const iw = cvs.width || 0, ih = cvs.height || 0;
  if (iw <= 0 || ih <= 0 || r.width <= 0 || r.height <= 0) return r;
  const s = Math.min(r.width / iw, r.height / ih);   // contain
  const cw = iw * s, ch = ih * s;
  if (Math.abs(cw - r.width) < 0.5 && Math.abs(ch - r.height) < 0.5) return r;
  return { left: r.left + (r.width - cw) / 2,
           top:  r.top  + (r.height - ch) / 2,
           width: cw, height: ch };
}

// 这个 client 坐标在画面里吗？不在（点在黑边上）就别注入 ——
// 否则会被钳成"点屏幕最边缘"，等于误触。
function inContent(ev) {
  const r = contentRect();
  return ev.clientX >= r.left - 1 && ev.clientX <= r.left + r.width + 1 &&
         ev.clientY >= r.top  - 1 && ev.clientY <= r.top  + r.height + 1;
}

function toScreen(ev) {
  const rect = contentRect();
  // ⚠️ 用**触控范围**，不是图尺寸 —— 见上面 touchW 那段。
  //    拿不到触控范围时才退回图尺寸（至少不崩）。
  const w = touchW || sw || cvs.width || 1;
  const h = touchH || sh || cvs.height || 1;
  const x = Math.round((ev.clientX - rect.left) / rect.width * w);
  const y = Math.round((ev.clientY - rect.top) / rect.height * h);
  return {
    x: Math.max(0, Math.min(w - 1, x)),
    y: Math.max(0, Math.min(h - 1, y))
  };
}

// pointerId → 触控槽位。支持多点触控（两根手指同时拖）。
const slotOf = new Map();
let nextSlot = 0;

function slotFor(pointerId) {
  if (!slotOf.has(pointerId)) {
    slotOf.set(pointerId, nextSlot);
    nextSlot = (nextSlot + 1) % 10;      // 设备侧 10 个槽位
  }
  return slotOf.get(pointerId);
}

// 移动事件合并。
//
// pointermove 在高刷屏上能到 200Hz，而设备侧的注入和屏幕刷新都跟不上。
// 每个事件都发只是白占带宽、还让服务端的锁更频繁地切换。
// 用 rAF 合并到"每帧一个"，既跟得上显示又不浪费。
const moveQueue = new Map();   // slot → {x,y}
let rafPending = false;

function queueMove(slot, x, y) {
  moveQueue.set(slot, {x, y});
  if (rafPending) return;
  rafPending = true;
  requestAnimationFrame(() => {
    rafPending = false;
    for (const [s, p] of moveQueue) send({t: 'move', x: p.x, y: p.y, id: s});
    moveQueue.clear();
  });
}

cvs.addEventListener('pointerdown', (ev) => {
  ev.preventDefault();
  // ⚠️ 铺满模式下 canvas 覆盖整个显示区，黑边那块也在里面。
  //    点在黑边上就当没点 —— 不然会被钳成"点屏幕最边缘"，是误触。
  if (!inContent(ev)) return;
  cvs.setPointerCapture(ev.pointerId);
  const p = toScreen(ev);
  const slot = slotFor(ev.pointerId);
  if (!send({t: 'down', x: p.x, y: p.y, id: slot})) {
    // 流没连上时退回一次性 POST，至少还能点
    tapPost(p.x, p.y);
  }
});

cvs.addEventListener('pointermove', (ev) => {
  const p = toScreen(ev);
  $('coord').textContent = p.x + ',' + p.y;
  if (!slotOf.has(ev.pointerId)) return;   // 没按下就不发
  queueMove(slotOf.get(ev.pointerId), p.x, p.y);
});

function endPointer(ev) {
  if (!slotOf.has(ev.pointerId)) return;
  const slot = slotOf.get(ev.pointerId);
  const p = toScreen(ev);
  slotOf.delete(ev.pointerId);
  moveQueue.delete(slot);
  send({t: 'up', x: p.x, y: p.y, id: slot});
}
cvs.addEventListener('pointerup', endPointer);
cvs.addEventListener('pointercancel', endPointer);
// 指针离开画面（比如拖到窗口外松手）也要收尾，
// 否则那根"手指"会在设备上一直按着
cvs.addEventListener('pointerleave', (ev) => {
  if (ev.buttons === 0) endPointer(ev);
});

// 长按不需要专门的命令：流式模型里"按下 → 手指不动 → 延迟抬起"
// 本来就是长按，系统会自己识别（Android 的 longPressTimeout 约 500ms）。
// 一次性手势（下面这个）只在触控流断开时兜底。

// ── 一次性手势（走 POST，不需要保持连接）──
function tapPost(x, y) {
  api('/tap', { method:'POST', headers:{'Content-Type':'application/json'},
                body: JSON.stringify({x:x, y:y, ms:50}) })
    .then(() => setStatus('点击 ' + x + ',' + y))
    .catch(e => setStatus('点击失败：' + e, true));
}


// ── 按键 ──
function key(name, longPress) {
  if (!name) return;
  api('/key', { method:'POST', headers:{'Content-Type':'application/json'},
                body: JSON.stringify({key:name, long:!!longPress}) })
    .then(d => setStatus('按键 ' + d.key + '（键码 ' + d.keyCode + '）')
        )
    .catch(e => setStatus('按键失败：' + e, true));
}
function keyLong() {
  const v = $('keytext').value.trim();
  if (v) key(v, true); else setStatus('先在输入框里填键名', true);
}

// ── 剪贴板 ──
function clipGet() {
  api('/clipboard?op=get')
    .then(d => {
      $('clip').textContent = d.has ? d.text : '（剪贴板为空）';
      $('clip').className = d.has ? '' : 'dim';
      setStatus('已读取剪贴板');
    })
    .catch(e => setStatus('读剪贴板失败：' + e, true));
}
function clipSet() {
  const t = $('cliptext').value;
  if (!t) { setStatus('请输入要写入的文本', true); return; }
  api('/clipboard', { method:'POST', headers:{'Content-Type':'application/json'},
                      body: JSON.stringify({op:'set', text:t}) })
    .then(() => { setStatus('已写入剪贴板'); clipGet(); })
    .catch(e => setStatus('写剪贴板失败：' + e, true));
}

// ── 应用列表 ──
function loadApps() {
  const sys = $('appSys').checked ? '1' : '0';
  $('apps').textContent = '加载中…';
  api('/apps?system=' + sys + '&meta=0')
    .then(d => {
      const box = $('apps');
      box.textContent = '';
      box.className = '';
      const filter = document.createElement('input');
      filter.type = 'text';
      filter.placeholder = '过滤包名…';
      // 137 个应用在下拉里翻很痛苦，加个过滤框
      const render = () => {
        const q = filter.value.toLowerCase();
        // 只渲染前 300 条，避免一次插几千个 DOM 节点
        const list = (d.apps || []).filter(a =>
            !q || a.package.toLowerCase().includes(q)).slice(0, 300);
        listBox.textContent = '';
        for (const a of list) {
          const row = document.createElement('div');
          row.className = 'app';
          const name = document.createElement('span');
          name.className = 'pkg';
          name.textContent = a.package;
          name.title = a.package;
          const bLaunch = document.createElement('button');
          bLaunch.textContent = '启动';
          bLaunch.onclick = () => appAction(a.package, 'launch');
          const bKill = document.createElement('button');
          bKill.textContent = '停止';
          bKill.onclick = () => appAction(a.package, 'kill');
          row.append(name, bLaunch, bKill);
          listBox.append(row);
        }
        if (!list.length) {
          const e = document.createElement('div');
          e.className = 'dim';
          e.textContent = '（没有匹配的应用）';
          listBox.append(e);
        }
      };
      const listBox = document.createElement('div');
      filter.oninput = render;
      box.append(filter, listBox);
      render();
      setStatus('共 ' + (d.count || 0) + ' 个应用');
    })
    .catch(e => setStatus('取应用列表失败：' + e, true));
}

function appAction(pkg, what) {
  const path = what === 'launch' ? '/apps/' + encodeURIComponent(pkg) + '/launch'
                                 : '/apps/' + encodeURIComponent(pkg) + '/kill';
  api(path, { method: 'POST', headers: {'Content-Type': 'application/json'},
              body: '{}' })
    .then(() => setStatus((what === 'launch' ? '已启动 ' : '已停止 ') + pkg))
    .catch(e => setStatus(what + ' ' + pkg + ' 失败：' + e, true));
}

// ── 电源 ──
function power(action) {
  const label = action === 'shutdown' ? '关机' : '重启';
  if (!confirm('确定要' + label + '设备吗？')) return;
  api('/power', { method:'POST', headers:{'Content-Type':'application/json'},
                  body: JSON.stringify({action: action}) })
    .then(d => setStatus(d.note || (label + '已下发')))
    .catch(e => setStatus(label + '失败：' + e, true));
}

// ── 画面流停止检测 ──
//
// "停止检测"= 画面没变时整帧跳过编码。静止画面下能省掉全部编码开销，
// 但刚打开页面时如果画面一直不动，会让人以为流挂了 —— 所以给个开关。
let skipUnchanged = true;

function setSkipUnchanged(on) {
  skipUnchanged = on;
  if (streamReady) {
    // 走 WS 命令，不用重连
    sendStream({t: 'skipUnchanged', v: on ? 1 : 0});
  }
  setStatus(on ? '已开启画面停止检测（静止时不重发）' : '已关闭 —— 每帧都会发送');
}

// ── 分辨率（降采样宽度）──
//
// 走 WS 的 maxWidth 命令，**不用重连**：连接时带 ?maxWidth= 那种做法
// 要断开重来，画面会闪一下，而且中途改的需求本来就很常见。
function setMaxWidth(w) {
  maxW = w;
  syncMaxWidthButtons();
  if (streamReady) {
    sendStream({t: 'maxWidth', v: w});
  }
  setStatus(w === 0 ? '分辨率：原始（不降采样）' : ('分辨率：宽 ' + w + 'px'));
}

function syncMaxWidthButtons() {
  for (const [id, v] of [['mw0', 0], ['mw720', 720],
                         ['mw480', 480], ['mw360', 360]]) {
    const el = $(id);
    if (el) el.className = (v === maxW) ? 'on' : '';
  }
  const lab = $('mwval');
  if (lab) lab.textContent = sw ? (sw + '×' + sh) : '';
}

// ── 屏幕方向 ──
//
// ⚠️ 服务端可能**转不动**（ROM 没有旋转支持），那时它会退到换显示尺寸，
//    并在应答里如实说走了哪条路。所以这里把结果显示出来 ——
//    假装成功的话，用户看到的是"按了没反应"，只会怀疑服务坏了。
function rotate(to) {
  setStatus('正在切换方向…');
  fetch(withToken('/api/v1/rotate'), {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({to: to})
  }).then(r => r.json()).then(d => {
    const el = $('rotmsg');
    const ok = d.applied !== false;
    let msg = ok ? '方向已切换' : '方向没切成';
    if (d.method === 'wm-size') {
      msg = '这台设备不支持真旋转，已改用显示尺寸（应用会按' +
            (d.width > d.height ? '横屏' : '竖屏') + '重新布局）';
    } else if (d.method === 'none' && !ok) {
      msg = '这台设备转不动：' + (d.note || '');
    }
    if (el) {
      el.textContent = msg + '  当前 ' + d.width + '×' + d.height
                     + ' · mRotation=' + d.actualRotation;
      el.style.color = ok ? '' : '#e0a020';
    }
    setStatus(msg);
    // ⚠️ 必须重取触控坐标空间。
    //
    // 服务端转屏时会**重建注入器**让坐标范围跟着显示走（见 dispatch.cpp 的
    // syncInjector），所以页面加载时取的那份就过期了。不重取的话，
    // 之后所有点击都会按旧空间换算 —— 转屏前能点中，转屏后全偏。
    loadTouchRange();
    setTimeout(refresh, 600);      // 尺寸变了，状态行也要跟着更新
  }).catch(e => setStatus('切方向失败：' + e, true));
}

function sendStream(obj) {
  if (streamReady && streamWs) {
    streamWs.send(JSON.stringify(obj));
    return true;
  }
  return false;
}

// ── 服务对外开关 ──
let serving = true;

function toggleService(on) {
  api('/service', { method:'POST', headers:{'Content-Type':'application/json'},
                    body: JSON.stringify({on: on}) })
    .then(d => {
      serving = d.serving;
      updateServiceUi(d.note || '');
    })
    .catch(e => setStatus('切换服务开关失败：' + e, true));
}

function refreshService() {
  api('/service').then(d => {
    serving = d.serving;
    updateServiceUi('');
  }).catch(() => {});
}

function updateServiceUi(note) {
  $('svcsw').checked = serving;
  $('svctext').textContent = serving ? '服务对外可用' : '服务已关闭';
  $('svcwrap').style.color = serving ? '#4a9' : '#c55';
  if (note) setStatus(note);
}

// ── 卡片收起 / 展开 ──
//
// 状态记在 localStorage：控制台的面板很多，用户收起某几块是长期偏好，
// 刷新一次就复位会很烦。
function toggleCard(id) {
  const el = $(id);
  if (!el) return;
  el.classList.toggle('collapsed');
  const st = readCollapsed();
  if (el.classList.contains('collapsed')) st[id] = 1; else delete st[id];
  try { localStorage.setItem('autod_collapsed', JSON.stringify(st)); } catch (e) {}
}

function readCollapsed() {
  try { return JSON.parse(localStorage.getItem('autod_collapsed') || '{}'); }
  catch (e) { return {}; }
}

function applyCollapsed() {
  const st = readCollapsed();
  for (const id of Object.keys(st)) {
    const el = $(id);
    if (el) el.classList.add('collapsed');
  }
}

// ── 面板收放 ──
//
// 横屏设备最缺的是**宽度**：1280x720 的画面配一个 300px 的面板，
// 在 1600px 的窗口里画面只能占 1300px 宽 —— 而竖屏设备缺的是高度，
// 面板放旁边反而不碍事。
//
// 所以：横屏流**默认收起**面板，把宽度全让给画面；竖屏流默认展开。
// 用户手动切过一次之后就按他的选择走（记在 localStorage）。
function readPanelPref() {
  try { return localStorage.getItem('autod.panel'); } catch (e) { return null; }
}
function writePanelPref(v) {
  try { localStorage.setItem('autod.panel', v); } catch (e) {}
}

function setPanel(open, remember) {
  document.body.classList.toggle('nopanel', !open);
  const b = $('panelsw');
  if (b) {
    b.textContent = open ? '面板 ✓' : '面板';
    b.title = open ? '收起右侧控制面板（画面占满）'
                   : '展开右侧控制面板';
  }
  if (remember) writePanelPref(open ? '1' : '0');
}

function togglePanel() {
  setPanel(document.body.classList.contains('nopanel'), true);
}

// ── 铺满 / 适应 ──
//
// 适应（默认）：保持设备宽高比，**只缩不放** —— 画面比显示区小的时候就
//              按原始像素显示（1:1），大了才缩到放得下，四周留黑边。
// 铺满：        同样保持比例，但**总是撑到显示区里能完整放下的最大值**
//              （画面小于显示区时会放大）。不裁切、不溢出、不变形。
//
// ⚠️ 两种模式都**不影响触控坐标** —— 画面尺寸写在 canvas 上，
//    坐标又从同一个 canvas 的 getBoundingClientRect() 读回来，
//    渲染和换算永远同源。改这里不需要动触控那条路。
function readFillPref() {
  try { return localStorage.getItem('autod.fill'); } catch (e) { return null; }
}
function setFill(on, remember) {
  document.body.classList.toggle('fillmode', on);
  const b = $('fillsw');
  if (b) {
    b.textContent = on ? '铺满 ✓' : '适应';
    b.title = on ? '当前：保持比例撑满显示区（放大到放得下的最大值，不变形也不裁切）'
                 : '当前：按原始像素显示，过大才缩小（可能有黑边）';
  }
  if (remember) {
    try { localStorage.setItem('autod.fill', on ? '1' : '0'); } catch (e) {}
  }
}
function toggleFill() {
  setFill(!document.body.classList.contains('fillmode'), true);
}

// 画面尺寸变了就重新决定默认值 —— 转屏之后该收该放会反过来。
// 只在用户没手动选过的时候动。
function applyPanelForAspect() {
  if (readPanelPref() !== null) return;
  if (!sw || !sh) return;
  setPanel(sh >= sw, false);      // 横屏（宽>高）→ 收起
}

// ── 上传安装 APK ──
//
// 直接把文件字节当请求体 POST，不用 multipart —— 服务端只收一个文件，
// 为它实现一遍 multipart 解析不划算，而传 File 本来就能直接当 body。
//
// ⚠️ 用 XMLHttpRequest 而不是 fetch：**fetch 给不了上传进度**。
//    APK 动辄几百 MB，上传要几十秒到几分钟，没有进度用户只能看着
//    "上传中…" 干等，分不清是在传还是卡死了。xhr.upload.onprogress
//    是浏览器里唯一能拿到上传进度的接口。
function uploadApk(input) {
  const f = input.files && input.files[0];
  if (!f) return;
  const msg = $('apkmsg');
  const mb = (f.size / 1048576).toFixed(1);

  // 本地先拦一道：服务端上限是 4GB，但设备内存/磁盘都可能更小，
  // 与其传完才失败，不如立刻说清楚
  msg.className = 'dim';
  msg.textContent = '上传中… ' + f.name + '（' + mb + ' MB） 0%';

  const replace = $('apkReplace').checked ? '1' : '0';
  const t0 = performance.now();
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/v1/install?replace=' + replace);
  xhr.setRequestHeader('Content-Type',
                       'application/vnd.android.package-archive');
  if (token) xhr.setRequestHeader('Authorization', 'Bearer ' + token);

  xhr.upload.onprogress = (e) => {
    if (!e.lengthComputable) return;
    const pct = Math.floor(e.loaded / e.total * 100);
    msg.textContent = '上传中… ' + f.name + '（' + mb + ' MB） '
                    + pct + '%  ' + ((e.loaded / 1048576).toFixed(0))
                    + ' / ' + mb + ' MB';
  };
  // 传完了但服务端还在装 —— 装一个几百 MB 的 APK 要好几秒，
  // 这段时间没有任何事件，不讲清楚又像是卡住了
  xhr.upload.onload = () => {
    msg.textContent = '已上传 ' + mb + ' MB，正在安装…';
  };

  // 服务端的错误原文可能是几百字的 Java 堆栈，塞进这个小 div 会把
  // 整个面板撑变形。截断显示，完整内容放 title（悬停可见）。
  const done = (ok, text) => {
    msg.className = ok ? 'ok' : 'err';
    msg.textContent = text.length > 220 ? text.slice(0, 220) + '…' : text;
    msg.title = text;
    setStatus(ok ? text : text.slice(0, 80), !ok);
    if (ok) setTimeout(loadRunning, 800);
  };

  xhr.onload = () => {
    let j = null;
    try { j = JSON.parse(xhr.responseText); } catch (e) {}
    // 服务端的原话最有价值（签名冲突、版本降级、空间不足），
    // 包装成"安装失败"就没用了
    const detail = (j && j.error) || ('HTTP ' + xhr.status);
    if (xhr.status === 413) {
      // 「传不上去」和「装不上」是两回事，别都报成安装失败 ——
      // 用户看到"安装失败：请求体超过上限"会去查应用，方向就错了
      done(false, '✗ 文件太大，服务端拒收：' + detail);
    } else if (j && j.ok) {
      done(true, '✓ 已安装 ' + (j.package || f.name)
                 + '（' + ((performance.now() - t0) / 1000).toFixed(1) + 's）');
    } else if (xhr.status === 0) {
      done(false, '✗ 连接被断开（文件太大或服务重启了？）');
    } else {
      done(false, '✗ 安装失败：' + detail);
    }
  };
  xhr.onerror = () => done(false, '✗ 上传失败：连接错误');
  xhr.ontimeout = () => done(false, '✗ 上传超时');
  xhr.onabort = () => done(false, '上传已取消');

  xhr.send(f);
  input.value = '';   // 允许重复选同一个文件
}

// ── 文件浏览 ──
//
// 路径一律用**绝对路径**。加了存储根之后"相对下载目录"不再唯一，
// 而绝对路径配合服务端报出的 storage 边界不会有歧义。
//
// ⚠️ 触控坐标那套"图尺寸 vs 触控范围"的坑这里不存在 —— 文件路径没有
//    缩放，给什么就是什么。
let fsCwd = '';          // 当前目录（绝对路径；空 = 下载目录）
let fsRoot = '';         // 存储根，从服务端问出来

function fsUp() {
  if (!fsCwd || !fsRoot || fsCwd === fsRoot) return fsRoot || '';
  const i = fsCwd.lastIndexOf('/');
  if (i <= 0) return fsRoot || '';
  const up = fsCwd.slice(0, i);
  // 不要退到存储根之上
  return up.length < fsRoot.length ? fsRoot : up;
}

function fsGoto(p) {
  // ⚠️ "/" 是**系统根**，不在允许范围内 —— 会被服务端正确拒掉。
  //    "根目录"指的是存储根（fsRoot，通常 /storage/emulated/0）。
  //    第一版按钮传的就是 '/'，点了一下直接报"路径不在允许范围内"。
  if (!p || p === '/') p = fsRoot || '';
  fsCwd = p;
  fsLoad();
}

function fsMsg(text, err) {
  const el = $('fsMsg');
  if (!el) return;
  el.className = err ? 'err' : 'dim';
  el.textContent = text;
}

function fsLoad() {
  const q = '/api/v1/files?op=list&path=' + encodeURIComponent(fsCwd);
  fetch(withToken(q))
    .then(r => r.json())
    .then(d => {
      if (!d || !d.ok) { fsMsg('✗ ' + ((d && d.error) || '读目录失败'), true); return; }
      if (d.storage && !fsRoot) fsRoot = d.storage;
      $('fsPath').textContent = '📁 ' + (d.dir === '.' ? (fsRoot + '（下载目录）') : d.dir);
      const box = $('fsList');
      if (!d.entries || !d.entries.length) {
        box.innerHTML = '<span class="dim">（空）</span>';
        return;
      }
      // 目录在前、同类按名字排 —— 和所有文件管理器一致，
      // 不排的话顺序随 readdir，看起来像乱的
      const es = d.entries.slice().sort((a, b) =>
        (b.dir - a.dir) || a.name.localeCompare(b.name));
      box.innerHTML = es.map(e => {
        const sz = e.dir ? '' : ('  ' + fmtSize(e.size));
        const click = e.dir
          ? 'onclick="fsGoto(' + JSON.stringify(e.path).replace(/"/g, '&quot;') + ')"'
          : '';
        return '<div class="row" style="margin:1px 0;align-items:center">'
             + '<span style="flex:1;cursor:' + (e.dir ? 'pointer' : 'default')
             + ';overflow:hidden;text-overflow:ellipsis;white-space:nowrap" ' + click + '>'
             + (e.dir ? '📁 ' : '📄 ') + esc(e.name) + '<span class="dim">' + sz + '</span></span>'
             + '<button style="padding:1px 6px;font-size:11px" onclick="fsDel('
             + JSON.stringify(e.path).replace(/"/g, '&quot;') + ',' + e.dir + ')">删</button>'
             + '</div>';
      }).join('');
      fsMsg('共 ' + d.count + ' 项');
    })
    .catch(e => fsMsg('✗ ' + e, true));
}

function esc(t) {
  return String(t).replace(/[&<>"]/g,
    c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
}

function fmtSize(n) {
  if (n < 1024) return n + ' B';
  if (n < 1048576) return (n / 1024).toFixed(0) + ' KB';
  if (n < 1073741824) return (n / 1048576).toFixed(1) + ' MB';
  return (n / 1073741824).toFixed(2) + ' GB';
}

function fsMkdir() {
  const name = prompt('新目录名：');
  if (!name) return;
  const p = (fsCwd || fsRoot) + '/' + name;
  fsPost({op: 'mkdir', path: p}, '已创建 ' + name);
}

function fsDel(path, isDir) {
  if (!confirm('删除 ' + path + (isDir ? '（含其中所有内容）' : '') + '？')) return;
  fsPost({op: 'delete', path: path, recursive: !!isDir}, '已删除');
}

function fsPost(body, okText) {
  fetch(withToken('/api/v1/files'), {
    method: 'POST',
    headers: authHeaders({'Content-Type': 'application/json'}),
    body: JSON.stringify(body)
  })
    .then(r => r.json())
    .then(d => {
      if (d && d.ok) { fsMsg('✓ ' + okText); fsLoad(); }
      else fsMsg('✗ ' + ((d && d.error) || '操作失败'), true);
    })
    .catch(e => fsMsg('✗ ' + e, true));
}

// ── 运行中的应用 ──
let runTimer = null;

function loadRunning() {
  const box = $('running');
  api('/running').then(d => {
    box.textContent = '';
    box.className = '';
    const all = d.apps || [];
    // 默认只看第三方应用。系统组件占了大半，默认全列出来的话
    // 真正要找的那个应用反而被淹了。
    const showSys = $('runSys').checked;
    const apps = showSys ? all : all.filter(a => !a.system);
    if (!apps.length) {
      box.innerHTML = '<span class="dim">（没有匹配的进程'
          + (showSys ? '' : '；勾上「含系统应用」看全部 ' + all.length + ' 个') + '）</span>';
      return;
    }
    // 前台和可见的排前面 —— 那才是用户关心的
    const rank = st => st.startsWith('fg') ? 0 : st.startsWith('vis') ? 1
                    : st.startsWith('prcp') ? 2 : 3;
    apps.sort((a, b) => rank(a.state) - rank(b.state));
    for (const a of apps) {
      const row = document.createElement('div');
      row.className = 'app';
      const name = document.createElement('span');
      name.className = 'pkg';
      name.textContent = a.pid + '  [' + a.state + ']  ' + a.process;
      name.title = a.process + '  uid=' + a.uid;
      const bKill = document.createElement('button');
      bKill.textContent = '关闭';
      // 系统进程不给按钮：误点会重启系统 UI，而不是关掉一个应用
      if (/^(android|com\.android\.systemui)$/.test(a.package)) {
        bKill.disabled = true;
        bKill.title = '系统进程，不提供关闭';
      } else {
        bKill.onclick = () => killApp(a.package);
      }
      row.append(name, bKill);
      box.append(row);
    }
    setStatus('运行中 ' + apps.length + ' 个'
              + (showSys ? '（含系统）' : '（仅第三方，共 ' + all.length + ' 个进程）'));
  }).catch(e => setStatus('取运行状态失败：' + e, true));
}

function killApp(pkg) {
  if (!confirm('强制停止 ' + pkg + '？')) return;
  api('/apps/' + encodeURIComponent(pkg) + '/kill',
      { method:'POST', headers:{'Content-Type':'application/json'}, body:'{}' })
    .then(() => { setStatus('已停止 ' + pkg); setTimeout(loadRunning, 600); })
    .catch(e => setStatus('停止失败：' + e, true));
}

function setRunAuto(on) {
  if (runTimer) { clearInterval(runTimer); runTimer = null; }
  if (on) runTimer = setInterval(loadRunning, 3000);
}

// ── 日志流 ──
//
// 用 WebSocket 增量推，而不是轮询 /api/v1/log：
// 日志是"有就推、没有就没有"，轮询在没日志时纯属白问，
// 有日志时又必然滞后一个轮询周期。
let logWs = null;
let logReady = false;
let logSeen = 0;
let logFilterLevel = -1;   // -1 = 不过滤

function connectLog() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  try { logWs = new WebSocket(withToken(proto + '//' + location.host + '/api/v1/logstream')); }
  catch (e) { return; }
  logWs.onopen = () => { logReady = true; };
  logWs.onclose = () => {
    logReady = false; logWs = null;
    setTimeout(connectLog, 2000);
  };
  logWs.onerror = () => { logReady = false; };
  logWs.onmessage = (ev) => {
    let m; try { m = JSON.parse(ev.data); } catch (e) { return; }
    if (m.t === 'lines') appendLogs(m.lines);
  };
}

function appendLogs(lines) {
  if (!lines || !lines.length) return;
  const box = $('logbox');
  if (!$('logAuto').checked) return;

  // 只在贴近底部时自动滚动。用户往上翻看历史时把他拽回底部
  // 是最招人烦的行为之一。
  const atBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 24;

  const frag = document.createDocumentFragment();
  for (const l of lines) {
    logSeen = Math.max(logSeen, l.seq || 0);
    const lv = 'IWED'[l.level] || '?';
    const d = document.createElement('div');
    d.className = 'lg-' + lv;
    // 时间前缀。服务端已经格式化好了（MM-DD HH:MM:SS），
    // 这里原样带上 —— 各端各自转时区的话，同一份日志在不同机器上
    // 会显示出不同的时间。
    const t = l.time ? (l.time + ' ') : '';
    d.textContent = t + lv + ' ' + (l.text || '');
    frag.append(d);
  }
  box.append(frag);
  // 上限：日志流是持续的，DOM 不限行数的话开一天就能把标签页拖死
  while (box.childElementCount > 500) box.removeChild(box.firstChild);
  if (atBottom) box.scrollTop = box.scrollHeight;
}

function logClear() {
  $('logbox').textContent = '';
  logSeen = 0;
  if (logReady) logWs.send(JSON.stringify({t: 'clear'}));
}

function logHistory() {
  api('/logfile').then(d => {
    $('logbox').textContent = '';
    const lines = (d.text || '').split('\n').filter(x => x);
    for (const t of lines) {
      const el = document.createElement('div');
      el.className = 'lg-T';
      el.textContent = t;
      $('logbox').append(el);
    }
    setStatus('历史日志 ' + d.bytes + ' 字节（' + d.path + '）');
  }).catch(e => setStatus('读历史日志失败：' + e, true));
}

// ── 启动 ──
//
// ⚠️ applyUrlParams 必须在 startStream 之前 —— 它决定用哪个格式。
//    放在后面的话第一帧已经按默认的 jpeg 拉了，
//    ?format=h264 要等下一次重连才生效。
applyUrlParams();
loadQualityRange();      // 不阻塞启动，拿到之后自己更新拖动条
loadTouchRange();        // 触控坐标空间（跟图尺寸不是一回事）
// 文件管理的边界（存储根）—— 问服务端，别在前端写死 /sdcard
fetch(withToken('/api/v1/files?op=roots')).then(r=>r.json())
  .then(d => { if (d && d.ok) { fsRoot = d.storage; } }).catch(()=>{});
syncMaxWidthButtons();
setInterval(pollCadence, 2000);
applyCollapsed();
setPanel(readPanelPref() === '0' ? false : true, false);
setFill(readFillPref() === '1', false);
refresh();
refreshService();
startStream();
connectTouch();
connectLog();
loadRunning();
setRunAuto(true);
setInterval(refresh, 10000);   // 定期刷状态，页面放着不动也不会显示过期信息
</script>
</body>
</html>
)HTML";
    return kHtml;
}

}  // namespace autod
