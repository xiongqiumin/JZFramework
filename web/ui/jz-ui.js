/* ===========================================================================
   jz-ui.js —— 极小的交互增强(经典 script, 无构建, 不引第三方)
   只做"锦上添花": toast / 日志自动贴底。布局与配色在没有本文件时也必须正确。

   用法:
     <script src="/ui/jz-ui.js"></script>
     JzUI.toast('已保存');  JzUI.toast('失败了', 'fail');
     <pre class="jz-log" data-autoscroll>…</pre>     // 新内容到达自动滚到底
   =========================================================================== */
(function (global) {
  'use strict';

  var JzUI = {};

  /* ---- toast ---- */
  function toastHost() {
    var el = document.getElementById('jz-toasts');
    if (!el) {
      el = document.createElement('div');
      el.id = 'jz-toasts';
      el.setAttribute('role', 'status');
      el.setAttribute('aria-live', 'polite');
      document.body.appendChild(el);
    }
    return el;
  }

  JzUI.toast = function (text, kind, ms) {
    var host = toastHost();
    // 同时最多 3 条, 免得手机上糊满屏
    while (host.children.length >= 3) host.removeChild(host.firstChild);

    var item = document.createElement('div');
    item.className = 'jz-toast' + (kind ? ' jz-toast--' + kind : '');
    item.textContent = text;
    host.appendChild(item);

    setTimeout(function () {
      if (item.parentNode) item.parentNode.removeChild(item);
    }, ms || 3000);
  };

  /* ---- 弹窗: 原生 <dialog> 的一层薄包装 ----
     不自研 modal(焦点陷阱 / Esc / 顶层渲染都由浏览器负责), 这里只管外观与 Promise 化。
       JzUI.dialog({ title, body, size:'lg', actions:[{text,value,kind}] }) -> Promise<value|null>
       JzUI.confirm('确定删除?', { danger:true }) -> Promise<boolean>          */
  function el(tag, cls) {
    var n = document.createElement(tag);
    if (cls) n.className = cls;
    return n;
  }

  JzUI.dialog = function (opts) {
    opts = opts || {};

    var dlg = el('dialog', 'jz-dialog' + (opts.size === 'lg' ? ' jz-dialog--lg' : ''));

    var hd = el('div', 'jz-dialog__hd');
    var title = el('h2', 'jz-dialog__title');
    title.textContent = opts.title || '';
    hd.appendChild(title);
    if (opts.closable !== false) {
      var x = el('button', 'jz-dialog__x');
      x.type = 'button';
      x.textContent = '×';
      x.setAttribute('aria-label', '关闭');
      x.onclick = function () { dlg.close(''); };
      hd.appendChild(x);
    }

    var bd = el('div', 'jz-dialog__bd');
    if (typeof opts.body === 'string') bd.innerHTML = opts.body;
    else if (opts.body) bd.appendChild(opts.body);

    var actions = opts.actions || [{ text: '确定', value: 'ok', kind: 'primary' }];
    var ft = el('div', 'jz-dialog__ft');
    actions.forEach(function (a) {
      var b = el('button', 'jz-btn' + (a.kind ? ' jz-btn--' + a.kind : ''));
      b.type = 'button';
      b.textContent = a.text || '确定';
      b.onclick = function () { dlg.close(a.value == null ? '' : String(a.value)); };
      ft.appendChild(b);
    });

    dlg.appendChild(hd);
    dlg.appendChild(bd);
    if (actions.length) dlg.appendChild(ft);
    document.body.appendChild(dlg);

    // 点遮罩关闭: dialog 自身铺满视口, 点到它就等于点到"外面"
    dlg.addEventListener('click', function (e) {
      if (e.target === dlg && opts.dismissible !== false) dlg.close('');
    });

    return new Promise(function (resolve) {
      dlg.addEventListener('close', function () {
        var v = dlg.returnValue;
        dlg.remove();
        resolve(v === '' ? null : v);
      }, { once: true });
      dlg.showModal();
    });
  };

  JzUI.confirm = function (text, opts) {
    opts = opts || {};
    return JzUI.dialog({
      title: opts.title || '确认',
      body: text,
      actions: [
        { text: opts.cancelText || '取消', value: 'cancel' },
        { text: opts.okText || '确定', value: 'ok', kind: opts.danger ? 'danger' : 'primary' }
      ]
    }).then(function (v) { return v === 'ok'; });
  };

  /* ---- 日志/输出块自动贴底 ----
     规则: 只有用户本来就在底部时才跟随; 手动往上翻看历史时不打断。 */
  function stick(el) {
    if (el.scrollHeight - el.scrollTop - el.clientHeight > 24) return;  // 不在底部
    el.scrollTop = el.scrollHeight;
  }

  function watch(el) {
    if (el.__jzStick) return;
    el.__jzStick = true;
    el.scrollTop = el.scrollHeight;
    var mo = new MutationObserver(function () { stick(el); });
    mo.observe(el, { childList: true, characterData: true, subtree: true });
  }

  function initAutoScroll(root) {
    var list = (root || document).querySelectorAll('[data-autoscroll]');
    for (var i = 0; i < list.length; i++) watch(list[i]);
  }

  JzUI.initAutoScroll = initAutoScroll;

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', function () { initAutoScroll(); });
  } else {
    initAutoScroll();
  }

  global.JzUI = JzUI;
})(window);
