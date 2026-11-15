/* ===========================================================================
   boot.js —— app.js 的"看门人"
   
   背景: server 的 http.token 会把 **app.js 本身**也挡住(见 authGuard: /api/* 与
   /app.js)。而 token 输入界面原来写在 app.js 里 —— app.js 被 401 掉, 输入框也就
   永远出不来。所以这段必须做成一个**不鉴权**的小脚本, 由它先探一下鉴权:
   
     有 token(或不需要) → 动态插入 <script src="/app.js">;
     没有 token          → 弹一个自带的输入框, 存 cookie 后 reload;
     被临时封禁          → 只提示 + 倒计时自动重试(这时输了也没用)。
   
   为什么不做成"登录页 + 302": 页面/样式(css)本身不是秘密, 只藏 app.js 里的接口和
   交互逻辑就够了; 少一次整页跳转, 手机端返回键也不会回到登录页。
   ========================================================================== */
(function () {
  var APP_SRC = '/app.js?v=80';

  function loadApp() {
    var s = document.createElement('script');
    s.src = APP_SRC;
    document.body.appendChild(s);
  }

  function cookieSet(name, value, days) {
    // Secure 只在 https 下加: 手机常用 http://192.168.x.x:8188 打开, 加了浏览器直接丢
    var secure = location.protocol === 'https:' ? '; Secure' : '';
    document.cookie = name + '=' + encodeURIComponent(value) +
      '; Path=/; Max-Age=' + (days > 0 ? days * 86400 : 0) + '; SameSite=Lax' + secure;
  }

  function overlay(title, bodyHtml) {
    var box = document.createElement('div');
    box.style.cssText = 'position:fixed;inset:0;z-index:99999;display:flex;align-items:center;' +
      'justify-content:center;background:rgba(0,0,0,.55);font:15px/1.6 system-ui,sans-serif';
    var card = document.createElement('div');
    card.style.cssText = 'background:#fff;color:#222;border-radius:12px;padding:20px 22px;' +
      'width:min(90vw,360px);box-sizing:border-box;box-shadow:0 10px 40px rgba(0,0,0,.35)';
    var h = document.createElement('div');
    h.style.cssText = 'font-weight:600;margin-bottom:8px';
    h.textContent = title;
    card.appendChild(h);
    if (typeof bodyHtml === 'string') {
      var b = document.createElement('div');
      b.style.cssText = 'color:#666;margin-bottom:12px';
      b.innerHTML = bodyHtml;
      card.appendChild(b);
    }
    box.appendChild(card);
    document.body.appendChild(box);
    return { box: box, card: card };
  }

  function showGate(msg) {
    var ui = overlay('需要访问 token', (msg || '这台 server 需要 token') +
      '<br>token 由 server.json 的 <code>http.token</code> 配置。');
    var input = document.createElement('input');
    input.type = 'password';
    input.placeholder = '粘贴 token';
    input.style.cssText = 'width:100%;box-sizing:border-box;padding:10px 12px;border:1px solid #ccc;' +
      'border-radius:8px;margin-bottom:12px;font:inherit';
    var btn = document.createElement('button');
    btn.textContent = '保存并进入';
    btn.style.cssText = 'width:100%;padding:10px;border:0;border-radius:8px;background:#2f6feb;' +
      'color:#fff;font:inherit;cursor:pointer';
    function go() {
      var v = (input.value || '').trim();
      if (!v) { input.focus(); return; }
      cookieSet('jz_token', v, 30);
      location.reload();
    }
    btn.addEventListener('click', go);
    input.addEventListener('keydown', function (e) { if (e.key === 'Enter') go(); });
    ui.card.appendChild(input);
    ui.card.appendChild(btn);
    setTimeout(function () { input.focus(); }, 0);
  }

  function showBanned(sec, why) {
    sec = Math.max(1, parseInt(sec, 10) || 60);
    var ui = overlay('尝试次数过多', '');
    var tip = ui.card.lastChild;
    var left = sec;
    function tick() {
      tip.textContent = (why || 'token 连续错误，已临时封禁') + '；' + left + ' 秒后自动重试。';
      if (left-- <= 0) { location.reload(); return; }
      setTimeout(tick, 1000);
    }
    tick();
  }

  function probe() {
    fetch('/api/status', { credentials: 'same-origin' })
      .then(function (r) {
        if (r.ok) { loadApp(); return; }
        if (r.status === 401) {
          r.json().then(function (j) { showGate(j && j.error); })
                .catch(function () { showGate(); });
          return;
        }
        if (r.status === 403) {
          r.json().then(function (j) { showBanned(j && j.retryAfterSec, j && j.error); })
                .catch(function () { showBanned(60); });
          return;
        }
        loadApp();   // 其它状态码: 交给 app.js 自己的错误提示
      })
      .catch(function () { loadApp(); });   // 服务端不可达: 同上
  }

  if (document.readyState === 'loading')
    document.addEventListener('DOMContentLoaded', probe);
  else
    probe();
})();
