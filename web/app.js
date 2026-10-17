/* ==========================================================================
   Slhbond-Monitor - 前端逻辑
   --------------------------------------------------------------------------
   卡片分工：
     系统信息 / CPU 总览 / GPU / 网络（波形）/ NPU / 存储设备（每盘一方块）
   轮询 /api/v1/status 一次拿全部数据，另外两个动作会单独发请求：
     - 立即刷新：带 ?refresh=1 强制重新采样
     - 硬盘健康检测：POST /api/v1/disk/check?device=xxx
   ========================================================================== */

(function () {
  'use strict';

  var POLL_FALLBACK_MS = 5000;
  var DONUT_CIRCUMFERENCE = 2 * Math.PI * 50;
  var WAVE_W = 600, WAVE_H = 150, WAVE_PAD = 8;

  var LEVEL_COLORS = {
    idle:     '#22c55e',
    normal:   '#4c8dff',
    high:     '#f59e0b',
    critical: '#ef4444'
  };

  var state = {
    pollMs: POLL_FALLBACK_MS,
    baseUptime: null,
    baseAt: 0,
    online: false,
    busy: false,
    payload: null
  };

  /* ------------------------------------------------------------ DOM */

  function $(id) { return document.getElementById(id); }

  var el = {
    statusLed: $('statusLed'), statusText: $('statusText'),
    versionText: $('versionText'), updateBtn: $('updateBtn'),
    checkBtn: $('checkBtn'), checkLine: $('checkLine'),
    themeBtn: $('themeBtn'),
    updateVer: $('updateVersion'), refreshHint: $('refreshHint'),
    btnRefresh: $('btnRefresh'), btnCpuRefresh: $('btnCpuRefresh'),
    btnCopy: $('btnCopy'), logoLed: $('logoLed'),
    tooltip: $('tooltip'), toast: $('toast'),

    cpuChip: $('cpuChip'), cpuModel: $('cpuModel'), cpuArc: $('cpuArc'),
    cpuUsageText: $('cpuUsageText'), cpuCores: $('cpuCores'),
    cpuThreads: $('cpuThreads'), cpuMaxCore: $('cpuMaxCore'),
    cpuTemp: $('cpuTemp'), cpuFreqHint: $('cpuFreqHint'), coreBars: $('coreBars'),

    gpuChip: $('gpuChip'), gpuModel: $('gpuModel'), gpuFreqCur: $('gpuFreqCur'),
    gpuFreqRange: $('gpuFreqRange'), gpuSteps: $('gpuSteps'),
    gpuDriver: $('gpuDriver'), gpuTemp: $('gpuTemp'), gpuPower: $('gpuPower'),
    gpuActive: $('gpuActive'), gpuNote: $('gpuNote'),

    netChip: $('netChip'), netIface: $('netIface'), netGrid: $('netGrid'),
    netRxLine: $('netRxLine'), netTxLine: $('netTxLine'),
    netRxArea: $('netRxArea'), netTxArea: $('netTxArea'),
    netWaveEmpty: $('netWaveEmpty'),
    netRxRate: $('netRxRate'), netTxRate: $('netTxRate'), netPeak: $('netPeak'),
    netSpeed: $('netSpeed'), netRxTotal: $('netRxTotal'),
    netTxTotal: $('netTxTotal'), netErrors: $('netErrors'),

    npuChip: $('npuChip'), npuHardware: $('npuHardware'), npuBadge: $('npuBadge'),
    npuLoad: $('npuLoad'), npuFreq: $('npuFreq'), npuDriver: $('npuDriver'),
    npuCores: $('npuCores'), npuBarsWrap: $('npuBarsWrap'), npuBars: $('npuBars'),
    npuNote: $('npuNote'),

    diskGrid: $('diskGrid'), diskHint: $('diskHint'),

    /* 第一行右侧方块 */
    tileAlert: $('tileAlert'), alertCount: $('alertCount'), alertSub: $('alertSub'),
    tileTrace: $('tileTrace'), traceCount: $('traceCount'), traceSub: $('traceSub'),

    /* 警告面板 */
    alertPanel: $('alertPanel'), alertPanelClose: $('alertPanelClose'),
    alertList: $('alertList'), alertTotal: $('alertTotal'),
    thrGrid: $('thrGrid'), thrSave: $('thrSave'), thrStatus: $('thrStatus'),

    /* 溯源时间轴 */
    traceBar: $('traceBar'), traceExit: $('traceExit'), traceSlider: $('traceSlider'),
    tracePlay: $('tracePlay'), traceEvents: $('traceEvents'),
    traceTime: $('traceTime'), traceAgo: $('traceAgo'), traceMeta: $('traceMeta'),
    traceTimeBtn: $('traceTimeBtn'), tracePicker: $('tracePicker'),
    tracePick: $('tracePick'), tracePickNote: $('tracePickNote'),
    traceDisk: $('traceDisk'),
    liveNote: $('liveNote'),

    /* 事件面板 */
    eventPanel: $('eventPanel'), eventPanelClose: $('eventPanelClose'),
    eventList: $('eventList'), eventNote: $('eventNote'), eventCount: $('eventCount'),
    eventShowService: $('eventShowService'),
    cmdList: $('cmdList'), cmdNote: $('cmdNote'), cmdCount: $('cmdCount'),
    cmdOnlyDeletes: $('cmdOnlyDeletes'), traceEventBadge: $('traceEventBadge')
  };

  var fields = {
    platform: $('f-platform'), osversion: $('f-osversion'),
    hostname: $('f-hostname'), uptime: $('f-uptime'),
    ipv4: $('f-ipv4'), ipv6: $('f-ipv6')
  };

  /* --------------------------------------------------------- 工具函数 */

  var NOT_AVAILABLE = '未获取';

  function formatUptime(totalSeconds) {
    var s = Math.max(0, Math.floor(totalSeconds));
    var days = Math.floor(s / 86400);
    var hours = Math.floor((s % 86400) / 3600);
    var mins = Math.floor((s % 3600) / 60);
    var secs = s % 60;

    if (days > 0) return days + ' 天 ' + hours + ' 小时 ' + mins + ' 分';
    if (hours > 0) return hours + ' 小时 ' + mins + ' 分 ' + secs + ' 秒';
    return mins + ' 分 ' + secs + ' 秒';
  }

  function orNA(value) {
    if (value === null || value === undefined) return NOT_AVAILABLE;
    var text = String(value).trim();
    return text === '' ? NOT_AVAILABLE : text;
  }

  function loadLevel(percent) {
    if (percent === null || percent === undefined || isNaN(percent)) return 'idle';
    if (percent >= 85) return 'critical';
    if (percent >= 60) return 'high';
    if (percent >= 25) return 'normal';
    return 'idle';
  }

  function formatFreq(khz) {
    if (!khz) return null;
    if (khz >= 1000000) return (khz / 1000000).toFixed(2) + ' GHz';
    return Math.round(khz / 1000) + ' MHz';
  }

  /** 字节 -> "111.8 GB"（十进制单位，与硬盘厂商标称一致） */
  function formatBytes(bytes) {
    if (!bytes || bytes < 0) return '—';
    var units = ['B', 'KB', 'MB', 'GB', 'TB', 'PB'];
    var i = 0;
    var v = bytes;
    while (v >= 1000 && i < units.length - 1) { v /= 1000; i++; }
    return (i === 0 ? v : v.toFixed(1)) + ' ' + units[i];
  }

  /** 字节/秒 -> "1.2 MB/s" */
  function formatRate(bps) {
    if (bps === null || bps === undefined) return '—';
    if (bps < 1) return '0 B/s';
    return formatBytes(bps) + '/s';
  }

  function setField(node, text, full) {
    if (!node) return;
    node.textContent = text;
    node.dataset.state = 'ready';

    if (full && full !== text) node.dataset.full = full;
    else if (text === NOT_AVAILABLE) node.dataset.full = '该主机未获取到对应信息';
    else delete node.dataset.full;

    requestAnimationFrame(function () { markTruncated(node); });
  }

  function markTruncated(node) {
    if (!node) return;
    var clipped = node.scrollWidth > node.clientWidth + 1;
    node.classList.toggle('is-truncated', clipped && !!node.dataset.full);
  }

  function setAllFieldsPending() {
    Object.keys(fields).forEach(function (k) { fields[k].dataset.state = 'pending'; });
  }

  /* ----------------------------------------------------------- 悬浮层 */

  var tooltipTimer = null;

  function tooltipText(node) {
    return node.dataset.tip || node.dataset.full || '';
  }

  /**
   * 放置悬浮提示。
   *
   * 优先贴锚点右侧，其次左侧，实在没位置才上下。关键约束是**任何情况下都
   * 不能压住锚点本身**：硬盘卡片的详情有十多行，早先"先往上放、放不下就贴
   * 顶"的写法会让它正好盖住卡片右上角的检测按钮。
   */
  function placeTooltip(target) {
    var rect = target.getBoundingClientRect();
    var box = el.tooltip.getBoundingClientRect();
    var gap = 12;
    var vw = window.innerWidth;
    var vh = window.innerHeight;
    var left;
    var top;

    if (rect.right + gap + box.width <= vw - 8) {
      left = rect.right + gap;                 /* 右侧 */
      top = rect.top;
    } else if (rect.left - gap - box.width >= 8) {
      left = rect.left - gap - box.width;      /* 左侧 */
      top = rect.top;
    } else {
      /* 横向都放不下（窄屏）：优先放下方 —— 检测按钮在卡片顶部，
         放到下面永远不会挡住它；下方也放不下才退回上方。 */
      left = Math.min(Math.max(8, rect.left), Math.max(8, vw - box.width - 8));
      top = (rect.bottom + gap + box.height <= vh - 8)
        ? rect.bottom + gap
        : rect.top - gap - box.height;
    }

    left = Math.min(Math.max(8, left), Math.max(8, vw - box.width - 8));
    top = Math.min(Math.max(8, top), Math.max(8, vh - box.height - 8));

    el.tooltip.style.left = left + 'px';
    el.tooltip.style.top = top + 'px';
  }

  function showTooltip(target) {
    var text = tooltipText(target);
    if (!text) return;
    el.tooltip.textContent = text;
    el.tooltip.classList.add('visible');
    el.tooltip.setAttribute('aria-hidden', 'false');
    placeTooltip(target);
  }

  function hideTooltip() {
    el.tooltip.classList.remove('visible');
    el.tooltip.setAttribute('aria-hidden', 'true');
  }

  function attachHover(node) {
    node.addEventListener('mouseenter', function () {
      if (!tooltipText(node)) return;
      clearTimeout(tooltipTimer);
      tooltipTimer = setTimeout(function () { showTooltip(node); }, 110);
    });
    node.addEventListener('mouseleave', function () {
      clearTimeout(tooltipTimer);
      hideTooltip();
    });
    node.addEventListener('focus', function () {
      if (tooltipText(node)) showTooltip(node);
    });
    node.addEventListener('blur', hideTooltip);
    node.addEventListener('click', function () {
      if (tooltipText(node)) showTooltip(node);
    });
  }

  /** 字段类：只有被截断时才值得悬浮 */
  function bindTooltip(node) {
    if (!node) return;
    node.addEventListener('mouseenter', function () {
      if (!node.classList.contains('is-truncated')) return;
      clearTimeout(tooltipTimer);
      tooltipTimer = setTimeout(function () { showTooltip(node); }, 90);
    });
    node.addEventListener('mouseleave', function () {
      clearTimeout(tooltipTimer);
      hideTooltip();
    });
    node.addEventListener('click', function () {
      if (node.classList.contains('is-truncated')) showTooltip(node);
    });
  }

  /* ------------------------------------------------------------- 提示 */

  var toastTimer = null;

  function toast(message) {
    el.toast.textContent = message;
    el.toast.classList.add('visible');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { el.toast.classList.remove('visible'); }, 2600);
  }

  function setOnline(online) {
    state.online = online;
    el.statusLed.dataset.state = online ? 'online' : 'offline';
    el.statusText.textContent = online ? '在线' : '离线';
    el.logoLed.setAttribute('fill', online ? '#22c55e' : '#ef4444');
  }

  /* ----------------------------------------------- 柱状图（CPU / NPU） */

  var coreBarNodes = [];
  var npuBarNodes = [];

  function buildBars(container, count, labelPrefix) {
    var nodes = [];
    container.innerHTML = '';
    for (var i = 0; i < count; i++) {
      var bar = document.createElement('div');
      bar.className = 'core-bar';
      var track = document.createElement('div');
      track.className = 'core-bar-track';
      var fill = document.createElement('div');
      fill.className = 'core-bar-fill';
      fill.style.height = '2%';
      fill.dataset.level = 'idle';
      track.appendChild(fill);
      var label = document.createElement('span');
      label.className = 'core-bar-label';
      label.textContent = labelPrefix + i;
      bar.appendChild(track);
      bar.appendChild(label);
      container.appendChild(bar);
      nodes.push(fill);
    }
    return nodes;
  }

  function updateBars(nodes, values, labelPrefix) {
    for (var i = 0; i < nodes.length; i++) {
      var raw = (i < values.length) ? values[i] : null;
      var node = nodes[i];
      if (typeof raw !== 'number' || isNaN(raw)) {
        node.style.height = '2%';
        node.dataset.level = 'idle';
        node.removeAttribute('title');
        continue;
      }
      var value = Math.max(0, Math.min(100, raw));
      node.style.height = Math.max(2, value) + '%';
      node.dataset.level = loadLevel(value);
      node.title = labelPrefix + i + ' 负载 ' + value.toFixed(1) + '%';
    }
  }

  /* --------------------------------------------------------- 渲染 CPU */

  function renderCpu(cpu) {
    if (!cpu || !el.cpuChip) return;

    var parts = [];
    if (cpu.soc) parts.push(cpu.soc);
    if (cpu.core_name && cpu.cores) parts.push(cpu.cores + '× ' + cpu.core_name);
    setField(el.cpuModel, parts.length ? parts.join(' · ') : (cpu.model || '未知'),
             (parts.length && cpu.model) ? cpu.model : null);

    var percent = (typeof cpu.usage_percent === 'number') ? cpu.usage_percent : null;
    if (percent === null) {
      el.cpuArc.style.strokeDasharray = '0 ' + DONUT_CIRCUMFERENCE;
      el.cpuUsageText.textContent = '—';
      el.cpuChip.dataset.level = 'idle';
    } else {
      var value = Math.max(0, Math.min(100, percent));
      var level = loadLevel(value);
      el.cpuArc.style.strokeDasharray =
        (DONUT_CIRCUMFERENCE * value / 100).toFixed(2) + ' ' + DONUT_CIRCUMFERENCE.toFixed(2);
      el.cpuArc.style.stroke = LEVEL_COLORS[level];
      el.cpuUsageText.textContent = value.toFixed(1) + '%';
      el.cpuChip.dataset.level = level;
    }

    el.cpuCores.textContent = cpu.cores ? String(cpu.cores) : '—';
    el.cpuThreads.textContent = cpu.threads ? String(cpu.threads) : '—';
    el.cpuMaxCore.textContent = (typeof cpu.max_core_percent === 'number')
      ? cpu.max_core_percent.toFixed(1) + '%' : '—';
    el.cpuMaxCore.title = (typeof cpu.busiest_core === 'number' && cpu.busiest_core >= 0)
      ? ('最高负载线程：CPU' + cpu.busiest_core) : '';
    el.cpuTemp.textContent = (typeof cpu.temp_c === 'number')
      ? cpu.temp_c.toFixed(1) + ' °C' : '未获取';
    el.cpuTemp.title = cpu.temp_label ? ('温度来源：' + cpu.temp_label) : '';

    var hints = [];
    var cur = formatFreq(cpu.freq_cur_khz);
    var max = formatFreq(cpu.freq_max_khz);
    if (cur) hints.push('当前 ' + cur);
    if (max) hints.push('上限 ' + max);
    if (cpu.governor) hints.push(cpu.governor);
    el.cpuFreqHint.textContent = hints.join('  ·  ');

    var usage = cpu.core_usage;
    if (!usage || !usage.length) {
      if (coreBarNodes.length !== (cpu.threads || 0)) {
        coreBarNodes = buildBars(el.coreBars, cpu.threads || 0, '');
      }
      return;
    }
    if (coreBarNodes.length !== usage.length) coreBarNodes = buildBars(el.coreBars, usage.length, '');
    updateBars(coreBarNodes, usage, '线程 ');
  }

  /* --------------------------------------------------------- 渲染 GPU */

  function renderGpu(gpu) {
    if (!gpu || !el.gpuChip) return;

    if (!gpu.present) {
      el.gpuChip.dataset.state = 'off';
      el.gpuModel.textContent = '未检测到 GPU';
      el.gpuFreqCur.textContent = '—';
      el.gpuFreqRange.textContent = '';
      el.gpuSteps.innerHTML = '';
      el.gpuDriver.textContent = '—';
      el.gpuTemp.textContent = '—';
      el.gpuPower.textContent = '—';
      el.gpuActive.textContent = '—';
      el.gpuNote.textContent = '该平台未暴露 GPU 设备。';
      return;
    }

    el.gpuChip.dataset.state = 'ready';
    /* 副标题只放型号；驱动另有专门的指标位，挤在一起会被省略号截掉 */
    el.gpuModel.textContent = gpu.model || 'GPU';
    el.gpuModel.title = gpu.compatible ? ('compatible: ' + gpu.compatible.trim()) : '';

    el.gpuFreqCur.textContent = formatFreq(Math.round((gpu.freq_cur_hz || 0) / 1000)) || '—';
    var rng = [];
    var lo = formatFreq(Math.round((gpu.freq_min_hz || 0) / 1000));
    var hi = formatFreq(Math.round((gpu.freq_max_hz || 0) / 1000));
    var avg = formatFreq(Math.round((gpu.freq_avg_hz || 0) / 1000));
    if (lo && hi) rng.push(lo + ' – ' + hi);
    if (avg) rng.push('均 ' + avg);
    if (gpu.governor) rng.push(gpu.governor);
    el.gpuFreqRange.textContent = rng.join('  ·  ');

    /* 频率档位柱：越高档柱子越高，当前档位高亮 */
    var steps = gpu.freq_steps_hz || [];
    if (el.gpuSteps.childElementCount !== steps.length) {
      el.gpuSteps.innerHTML = '';
      steps.forEach(function (hz, i) {
        var wrap = document.createElement('div');
        wrap.className = 'gpu-step';
        var bar = document.createElement('div');
        bar.className = 'gpu-step-bar';
        var label = document.createElement('span');
        label.className = 'gpu-step-label';
        label.textContent = (hz / 1e6).toFixed(0) + 'M';
        wrap.appendChild(bar);
        wrap.appendChild(label);
        wrap.title = (hz / 1e6).toFixed(0) + ' MHz';
        el.gpuSteps.appendChild(wrap);
      });
    }
    var n = steps.length || 1;
    Array.prototype.forEach.call(el.gpuSteps.children, function (node, i) {
      var bar = node.firstChild;
      bar.style.height = (34 + (i / Math.max(1, n - 1)) * 56) + '%';
      node.dataset.current = (i === gpu.freq_step_index) ? '1' : '0';
    });

    el.gpuDriver.textContent = gpu.driver || '—';
    el.gpuDriver.title = gpu.render_node ? ('渲染节点 ' + gpu.render_node) : '';
    el.gpuTemp.textContent = (typeof gpu.temp_c === 'number')
      ? gpu.temp_c.toFixed(1) + ' °C' : '未获取';
    el.gpuTemp.title = gpu.temp_label ? ('温度来源：' + gpu.temp_label) : '';
    el.gpuPower.textContent = gpu.power_state === 'active' ? '运行中'
      : (gpu.power_state === 'suspended' ? '已挂起' : orNA(gpu.power_state));
    el.gpuActive.textContent = (typeof gpu.active_ratio_percent === 'number')
      ? gpu.active_ratio_percent.toFixed(0) + '%' : '—';
    el.gpuActive.title = 'GPU 上电时长占采样区间的比例';

    /* panfrost 不提供负载计数器，如实说明，不编数字 */
    if (typeof gpu.load_percent === 'number') {
      el.gpuNote.textContent = '实时负载 ' + gpu.load_percent.toFixed(1) + '%（来源：'
        + (gpu.load_source || '内核') + '）';
    } else {
      el.gpuNote.textContent = '当前内核（' + (gpu.driver || 'GPU 驱动')
        + '）未提供负载计数器，因此以频率档位与上电占比反映 GPU 活跃度。';
    }
  }

  /* ------------------------------------------------------- 渲染网络 */

  /**
   * 波形路径。
   *
   * x 轴固定为完整历史窗口（SLH 里是 48 个采样点），新数据从右侧进入、
   * 旧数据向左滚出。早期样本不满一屏时就只画左侧那一段——如果按当前样本数
   * 拉伸到整个宽度，开头的几个点会被拉成一条毫无意义的斜线。
   */
  function wavePath(values, maxValue, fill, capacity) {
    var n = values.length;
    if (n === 0) return '';

    var usable = WAVE_H - WAVE_PAD * 2;
    var step = WAVE_W / Math.max(1, capacity - 1);
    var offset = WAVE_W - (n - 1) * step;      /* 右对齐 */
    var scale = maxValue > 0 ? maxValue : 1;
    var pts = [];
    var i;

    for (i = 0; i < n; i++) {
      var v = values[i] || 0;
      var y = WAVE_PAD + usable - Math.min(1, v / scale) * usable;
      pts.push([offset + i * step, y]);
    }

    var d = 'M' + pts[0][0].toFixed(1) + ',' + pts[0][1].toFixed(1);
    for (i = 1; i < n; i++) d += ' L' + pts[i][0].toFixed(1) + ',' + pts[i][1].toFixed(1);
    if (fill) {
      d += ' L' + pts[n - 1][0].toFixed(1) + ',' + WAVE_H;
      d += ' L' + pts[0][0].toFixed(1) + ',' + WAVE_H + ' Z';
    }
    return d;
  }

  function renderGrid() {
    if (el.netGrid.childElementCount) return;
    var lines = '';
    for (var i = 1; i <= 3; i++) {
      var y = (WAVE_H / 4) * i;
      lines += '<line x1="0" y1="' + y + '" x2="' + WAVE_W + '" y2="' + y + '"/>';
    }
    el.netGrid.innerHTML = lines;
  }

  function renderNetwork(net) {
    if (!net || !el.netChip) return;

    var list = net.interfaces || [];
    var idx = (typeof net.primary_index === 'number' && net.primary_index >= 0)
      ? net.primary_index : -1;
    var iface = idx >= 0 ? list[idx] : null;

    if (!iface) {
      el.netChip.dataset.state = 'off';
      el.netIface.textContent = '未检测到网络接口';
      el.netWaveEmpty.classList.remove('hidden');
      el.netWaveEmpty.textContent = '没有可用接口';
      return;
    }

    el.netChip.dataset.state = iface.up ? 'ready' : 'hw';
    el.netIface.textContent = iface.name
      + (iface.kind === 'physical' ? ' · 物理网卡' : ' · ' + iface.kind)
      + (iface.mac ? ' · ' + iface.mac : '');
    el.netIface.title = iface.mac ? ('MAC ' + iface.mac) : '';

    var rx = iface.rx_history || [];
    var tx = iface.tx_history || [];

    el.netRxRate.textContent = formatRate(iface.rx_bps);
    el.netTxRate.textContent = formatRate(iface.tx_bps);
    var peak = Math.max(iface.rx_peak_bps || 0, iface.tx_peak_bps || 0);
    el.netPeak.textContent = formatRate(peak);

    el.netSpeed.textContent = iface.speed_mbps
      ? (iface.speed_mbps >= 1000 ? (iface.speed_mbps / 1000) + ' Gbps'
                                  : iface.speed_mbps + ' Mbps')
      : '—';
    el.netRxTotal.textContent = formatBytes(iface.rx_bytes);
    el.netTxTotal.textContent = formatBytes(iface.tx_bytes);
    el.netErrors.textContent = ((iface.rx_errors || 0) + (iface.tx_errors || 0))
      + ' / ' + ((iface.rx_dropped || 0) + (iface.tx_dropped || 0));

    /* 波形：至少两个点才画得出来，否则显示占位文案 */
    if (rx.length < 2) {
      el.netWaveEmpty.classList.remove('hidden');
      el.netWaveEmpty.textContent = '正在采集波形…（已 ' + rx.length + '/2 个采样点）';
      return;
    }
    el.netWaveEmpty.classList.add('hidden');
    renderGrid();

    /* 纵轴按窗口内的最大值自适应，并给一个下限避免空闲时把噪声放大 */
    var maxValue = 1024;
    rx.concat(tx).forEach(function (v) { if (v > maxValue) maxValue = v; });
    maxValue *= 1.1;

    var capacity = net.history_length || Math.max(rx.length, 48);

    el.netRxLine.setAttribute('d', wavePath(rx, maxValue, false, capacity));
    el.netTxLine.setAttribute('d', wavePath(tx, maxValue, false, capacity));
    el.netRxArea.setAttribute('d', wavePath(rx, maxValue, true, capacity));
    el.netTxArea.setAttribute('d', wavePath(tx, maxValue, true, capacity));
  }

  /* --------------------------------------------------------- 渲染 NPU */

  var NPU_BADGE = {
    ready:         { text: '已就绪',     attr: 'ready'   },
    hardware_only: { text: '驱动未加载', attr: 'hw'      },
    unknown:       { text: '未检测到',   attr: 'unknown' }
  };

  function renderNpu(npu) {
    if (!npu || !el.npuChip) return;

    var badge = NPU_BADGE[npu.state] || NPU_BADGE.unknown;
    el.npuHardware.textContent = npu.hardware || '未检测到加速器';
    el.npuBadge.textContent = badge.text;
    el.npuBadge.dataset.state = badge.attr;
    el.npuChip.dataset.state = badge.attr;

    el.npuLoad.textContent = npu.load_available ? npu.load_percent.toFixed(1) + '%' : '—';
    el.npuFreq.textContent = formatFreq(Math.round((npu.freq_hz || 0) / 1000)) || '—';
    el.npuDriver.textContent = npu.driver_loaded ? (npu.driver_version || '已加载') : '未加载';
    el.npuDriver.title = npu.source ? ('数据来源：' + npu.source) : '';
    el.npuCores.textContent = npu.core_count ? String(npu.core_count) : '—';
    el.npuNote.textContent = npu.note || '';

    var cores = npu.core_load;
    if (npu.load_available && cores && cores.length) {
      el.npuBarsWrap.classList.remove('hidden');
      if (npuBarNodes.length !== cores.length) npuBarNodes = buildBars(el.npuBars, cores.length, 'C');
      updateBars(npuBarNodes, cores, '核心 ');
    } else {
      el.npuBarsWrap.classList.add('hidden');
    }
  }

  /* --------------------------------------------------------- 渲染硬盘 */

  var HEALTH_TEXT = {
    healthy: '健康', warning: '警告', fault: '故障', unknown: '未检测'
  };

  /* 参考图的外形：机身 + 深色前面板 + 右侧状态灯 */
  function diskLogoSvg() {
    return '<svg viewBox="0 0 64 64" aria-hidden="true">'
      + '<rect x="13" y="5" width="38" height="54" rx="9" fill="none" '
      +   'stroke="currentColor" stroke-width="3"/>'
      + '<rect x="16" y="38" width="32" height="15" rx="5.5" fill="#0d1015" '
      +   'stroke="currentColor" stroke-width="2.4"/>'
      + '<rect class="disk-led" x="41" y="41" width="3.8" height="9" rx="1.9"/>'
      + '</svg>';
  }

  /**
   * 迷你曲线路径（方块内部的读写历史）。
   *
   * 和网络波形一样，x 轴固定为完整窗口宽度并右对齐，这样新采样从右端进入、
   * 曲线向左生长；早期样本不满一屏时就只画右边一小段。窗口容量随数据一起
   * 从服务端下发（io_history_length），前后端不会各写一个数字。
   */
  function sparkPath(values, maxValue, capacity, w, h) {
    var n = values.length;
    if (n < 2) return '';

    var step = w / Math.max(1, capacity - 1);
    var offset = w - (n - 1) * step;
    var scale = maxValue > 0 ? maxValue : 1;
    var d = '';
    var i;

    for (i = 0; i < n; i++) {
      var v = values[i] || 0;
      var x = offset + i * step;
      var y = h - Math.min(1, v / scale) * h;
      d += (i ? ' L' : 'M') + x.toFixed(2) + ',' + y.toFixed(2);
    }
    return d;
  }

  var diskCards = {};

  function createDiskCard(disk) {
    var root = document.createElement('article');
    root.className = 'disk-card';
    root.id = 'disk-' + disk.device;
    root.dataset.health = disk.health || 'unknown';
    root.tabIndex = 0;
    root.innerHTML =
      '<div class="disk-top">'
      +   '<span class="disk-logo">' + diskLogoSvg() + '</span>'
      +   '<button type="button" class="disk-check" title="执行健康检测">'
      +     '<svg viewBox="0 0 24 24" aria-hidden="true">'
      +       '<path d="M20 11a8 8 0 1 0-2.3 5.7"/>'
      +       '<polyline points="20 4 20 11 13 11"/>'
      +     '</svg>'
      +   '</button>'
      + '</div>'
      + '<div class="disk-name-row">'
      +   '<h3 class="disk-name"></h3>'
      +   '<span class="disk-sys hidden">系统盘</span>'
      + '</div>'
      + '<p class="disk-meta"></p>'
      + '<div class="disk-cap">'
      +   '<div class="disk-cap-bar"><span></span></div>'
      +   '<p class="disk-cap-text"><span class="cap-free"></span>'
      +     '<span class="cap-used"></span></p>'
      + '</div>'
      /* 方块内部的读写历史 + 当前速率 */
      + '<div class="disk-io">'
      +   '<svg class="disk-spark" viewBox="0 0 100 24" preserveAspectRatio="none" '
      +        'aria-hidden="true">'
      +     '<path class="spark-write" d=""/>'
      +     '<path class="spark-read" d=""/>'
      +   '</svg>'
      +   '<div class="disk-io-legend">'
      +     '<span class="io-item io-item-read"><i>读</i><b class="rate-read">—</b></span>'
      +     '<span class="io-item io-item-write"><i>写</i><b class="rate-write">—</b></span>'
      +   '</div>'
      + '</div>'
      + '<span class="disk-health"></span>';

    var card = {
      root: root,
      name: root.querySelector('.disk-name'),
      sys: root.querySelector('.disk-sys'),
      meta: root.querySelector('.disk-meta'),
      bar: root.querySelector('.disk-cap-bar span'),
      free: root.querySelector('.cap-free'),
      used: root.querySelector('.cap-used'),
      health: root.querySelector('.disk-health'),
      button: root.querySelector('.disk-check'),
      sparkRead: root.querySelector('.spark-read'),
      sparkWrite: root.querySelector('.spark-write'),
      rateRead: root.querySelector('.rate-read'),
      rateWrite: root.querySelector('.rate-write'),
      device: disk.device
    };

    card.button.addEventListener('click', function (event) {
      event.stopPropagation();
      requestDiskCheck(card.device, card.button);
    });

    attachHover(root);
    el.diskGrid.appendChild(root);
    return card;
  }

  function updateDiskCard(card, disk) {
    var health = disk.health || 'unknown';

    card.root.dataset.health = health;
    card.name.textContent = disk.name || disk.device;
    card.sys.classList.toggle('hidden', !disk.is_system);
    card.sys.title = disk.is_system
      ? ('系统所在磁盘（挂载于 ' + (disk.system_mount || '/') + '）') : '';
    card.meta.textContent = (disk.kind || '磁盘') + ' · ' + formatBytes(disk.size_bytes);

    var usedPct = (typeof disk.fs_used_percent === 'number') ? disk.fs_used_percent : 0;
    card.bar.style.width = Math.max(0, Math.min(100, usedPct)).toFixed(1) + '%';
    card.free.textContent = disk.mounted
      ? ('可用 ' + formatBytes(disk.fs_free_bytes))
      : formatBytes(disk.size_bytes) + ' 未挂载';
    card.used.textContent = disk.mounted ? usedPct.toFixed(1) + '%' : '—';

    card.health.textContent = HEALTH_TEXT[health] || '未知';

    /* 当前读写：方块内部显示的是窗口均值 + 瞬时高亮，避免空闲时全是 0。
       曲线用整个窗口的最大值做纵轴，突发就能看出来。 */
    var rh = disk.read_history || [];
    var wh = disk.write_history || [];
    var capacity = disk.io_history_length || Math.max(rh.length, 32);
    var peak = 1;
    rh.concat(wh).forEach(function (v) { if (v > peak) peak = v; });
    peak *= 1.15;

    card.sparkRead.setAttribute('d', sparkPath(rh, peak, capacity, 100, 24));
    card.sparkWrite.setAttribute('d', sparkPath(wh, peak, capacity, 100, 24));

    card.rateRead.textContent = formatRate(disk.read_avg_bps);
    card.rateWrite.textContent = formatRate(disk.write_avg_bps);
    card.rateRead.title = '窗口均值；瞬时 ' + formatRate(disk.read_bps)
      + '，峰值 ' + formatRate(disk.read_peak_bps);
    card.rateWrite.title = '窗口均值；瞬时 ' + formatRate(disk.write_bps)
      + '，峰值 ' + formatRate(disk.write_peak_bps);

    /* 悬浮显示完整详情 */
    var lines = [
      (disk.name || disk.device) + '  (' + disk.device + ')'
        + (disk.is_system ? '  [系统盘]' : ''),
      '类型：' + (disk.kind || '—') + ' · ' + (disk.transport || '—'),
      '总容量：' + formatBytes(disk.size_bytes)
    ];
    if (disk.mounted) {
      lines.push('挂载点：' + disk.mount_point + '  (' + (disk.fs_type || '?') + ')');
      lines.push('可用容量：' + formatBytes(disk.fs_free_bytes)
                 + '  /  已用 ' + (typeof disk.fs_used_percent === 'number'
                                   ? disk.fs_used_percent.toFixed(1) + '%' : '—'));
      if (disk.fs_errors_count) lines.push('文件系统错误计数：' + disk.fs_errors_count);
    } else {
      lines.push('未挂载');
    }
    if (disk.vendor) lines.push('厂商：' + disk.vendor);
    if (disk.serial) lines.push('序列号：' + disk.serial);
    if (disk.firmware) lines.push('固件：' + disk.firmware);
    if (typeof disk.temperature_c === 'number') lines.push('温度：' + disk.temperature_c.toFixed(1) + ' °C');
    if (disk.power_on_hours) lines.push('通电时长：' + disk.power_on_hours + ' 小时');
    lines.push('当前读写：' + formatRate(disk.read_bps) + '  /  ' + formatRate(disk.write_bps));
    lines.push('窗口均值：' + formatRate(disk.read_avg_bps) + '  /  ' + formatRate(disk.write_avg_bps));
    lines.push('峰值读写：' + formatRate(disk.read_peak_bps) + '  /  ' + formatRate(disk.write_peak_bps));
    lines.push('累计读 / 写：' + formatBytes(disk.read_bytes) + '  /  ' + formatBytes(disk.write_bytes));
    lines.push('');
    lines.push('健康等级：' + (HEALTH_TEXT[health] || '未知')
               + (disk.health_checked ? (disk.health_auto ? '（首次自动检测）' : '（手动检测）') : '（尚未检测）'));
    if (disk.health_source) lines.push('检测来源：' + disk.health_source);
    if (disk.health_summary) lines.push('结论：' + disk.health_summary);
    if (disk.health_detail) lines.push(disk.health_detail);
    if (disk.health_checked_at) lines.push('检测时间：' + disk.health_checked_at);
    lines.push('');
    lines.push('点击右上角按钮可重新执行健康检测。');

    card.root.dataset.tip = lines.join('\n');
  }

  function renderDisks(storage) {
    if (!el.diskGrid) return;
    var disks = (storage && storage.disks) || [];
    var seen = {};

    disks.forEach(function (disk) {
      seen[disk.device] = true;
      var card = diskCards[disk.device] || (diskCards[disk.device] = createDiskCard(disk));
      updateDiskCard(card, disk);
      var busy = storage.check_in_progress && storage.check_device === disk.device;
      card.button.disabled = !!busy;
      card.button.classList.toggle('spinning', !!busy);
    });

    Object.keys(diskCards).forEach(function (device) {
      if (!seen[device]) {
        diskCards[device].root.remove();
        delete diskCards[device];
      }
    });

    if (!disks.length && !el.diskGrid.querySelector('.disk-empty')) {
      var empty = document.createElement('p');
      empty.className = 'disk-empty';
      empty.textContent = '未检测到硬盘';
      el.diskGrid.appendChild(empty);
    } else if (disks.length) {
      var placeholder = el.diskGrid.querySelector('.disk-empty');
      if (placeholder) placeholder.remove();
    }

    el.diskHint.textContent = disks.length
      ? (disks.length + ' 块设备 · 首次识别自动检测一次，之后点按钮手动触发')
      : '插入硬盘后自动出现';
  }

  function requestDiskCheck(device, button) {
    button.classList.add('spinning');
    button.disabled = true;

    fetch('/api/v1/disk/check?device=' + encodeURIComponent(device), { method: 'POST' })
      .then(function (response) {
        return response.json().then(function (body) { return { ok: response.ok, body: body }; });
      })
      .then(function (result) {
        if (!result.ok) {
          throw new Error((result.body.error && result.body.error.message) || '请求被拒绝');
        }
        toast('已开始检测 ' + device + '，稍后自动刷新结果');
        /* 检测通常一两秒内完成，随后几次轮询会把结果带回来 */
        setTimeout(function () { fetchStatus(true); }, 1500);
        setTimeout(function () { fetchStatus(true); }, 4000);
      })
      .catch(function (error) {
        toast('检测失败：' + error.message);
        button.classList.remove('spinning');
        button.disabled = false;
      });
  }

  /* ----------------------------------------------------- 渲染系统信息 */

  function render(payload) {
    /* 实时快照始终留一份：溯源要用它补齐历史里没存的静态字段。 */
    state.live = payload;

    renderTiles(payload);

    /* 溯源模式下画面归时间轴管，实时数据只更新方块计数。 */
    if (trace.active) return;

    renderSystemFields(payload);
    renderCards(payload);

    var version = (payload && payload.version) || {};
    if (version.current) el.versionText.textContent = 'v' + version.current;
    renderCheckLine(version);
    renderUpdateButton(version, payload.update);

    if (typeof payload.poll_interval_ms === 'number' && payload.poll_interval_ms > 0) {
      var next = payload.poll_interval_ms;
      if (next !== state.pollMs) { state.pollMs = next; restartPolling(); }
      el.refreshHint.textContent = '每 ' + Math.round(next / 1000) + ' 秒自动刷新';
    }
  }

  /** 系统信息卡片左栏的六个字段。 */
  function renderSystemFields(payload) {
    var system = (payload && payload.system) || {};

    var platform = system.platform || '';
    var arch = system.arch || '';
    setField(fields.platform,
             (platform && arch ? platform + ' · ' + arch : (platform || arch)) || NOT_AVAILABLE,
             system.kernel ? 'kernel ' + system.kernel : null);
    setField(fields.osversion, orNA(system.os_version),
             system.kernel ? '内核 ' + system.kernel : null);
    setField(fields.hostname, orNA(system.hostname), system.hostname || null);

    if (typeof system.uptime_seconds === 'number') {
      state.baseUptime = system.uptime_seconds;
      state.baseAt = (window.performance && performance.now) ? performance.now() : Date.now();
      setField(fields.uptime, formatUptime(state.baseUptime));
    } else {
      setField(fields.uptime, orNA(system.uptime_human));
    }

    setField(fields.ipv4, orNA(system.ipv4),
             system.ipv4_interface ? system.ipv4 + '  (' + system.ipv4_interface + ')' : system.ipv4);
    setField(fields.ipv6, orNA(system.ipv6),
             system.ipv6_interface ? system.ipv6 + '  (' + system.ipv6_interface + ')' : system.ipv6);
  }

  /**
   * 四张硬件卡片。溯源模式不渲染存储区：那部分数据（SMART 结论、逐盘读写）
   * 体积太大，没有进每秒记录，与其显示实时值冒充历史，不如整块淡出。
   */
  function renderCards(payload) {
    renderCpu(payload.cpu);
    renderGpu(payload.gpu);
    renderNetwork(payload.network);
    renderNpu(payload.npu);
    if (!trace.active) renderDisks(payload.storage);
  }

  function renderUpdateButton(version, update) {
    var available = version && version.update_available === true;
    var staged = update && update.staged === true;
    var busy = update && (update.phase === 'downloading' ||
                          update.phase === 'verifying' ||
                          update.phase === 'extracting');

    /* 已就绪 / 正在下载时，即使清单还没刷新也要把按钮留在可见状态 */
    el.updateBtn.classList.toggle('hidden', !available && !staged && !busy);
    if (!available && !staged && !busy) return;

    el.updateVer.textContent = (staged ? 'v' + (update.version || version.latest || '') : '')
      || (version.latest ? 'v' + version.latest : '');

    if (staged) {
      el.updateBtn.disabled = true;
      setUpdateBtnText('等待应用');
      el.updateBtn.title = '更新已下载并校验完成，系统会在 5 分钟内自动应用并重启服务';
    } else if (busy) {
      el.updateBtn.disabled = true;
      setUpdateBtnText(update.percent >= 0 ? ('下载中 ' + update.percent + '%') : '下载中…');
      el.updateBtn.title = update.message || '正在下载更新包';
    } else if (update && update.phase === 'failed') {
      el.updateBtn.disabled = false;
      setUpdateBtnText('更新失败');
      el.updateBtn.title = update.error || '更新失败';
    } else {
      el.updateBtn.disabled = false;
      setUpdateBtnText('更新可用');
      el.updateBtn.title = version.notes
        ? '新版本 ' + version.latest + '：' + version.notes
        : ('新版本 ' + version.latest + ' 可用，点击下载并更新');
    }
  }

  /* ------------------------------------------------------------ 轮询 */

  function fetchStatus(force) {
    if (state.busy) return;
    state.busy = true;

    fetch('/api/v1/status' + (force ? '?refresh=1' : ''),
          { method: 'GET', headers: { 'Accept': 'application/json' }, cache: 'no-store' })
      .then(function (response) {
        if (!response.ok) throw new Error('HTTP ' + response.status);
        return response.json();
      })
      .then(function (payload) {
        state.payload = payload;
        setOnline(true);
        render(payload);
      })
      .catch(function (error) {
        setOnline(false);
        console.warn('Slhbond-Monitor: 拉取数据失败 -', error.message);
      })
      .then(function () { state.busy = false; });
  }

  var pollTimer = null;

  function restartPolling() {
    clearInterval(pollTimer);
    pollTimer = setInterval(function () { fetchStatus(false); }, state.pollMs);
  }

  function startUptimeTicker() {
    setInterval(function () {
      if (state.baseUptime === null) return;
      var now = (window.performance && performance.now) ? performance.now() : Date.now();
      fields.uptime.textContent = formatUptime(state.baseUptime + (now - state.baseAt) / 1000);
      fields.uptime.dataset.state = 'ready';
    }, 1000);
  }

  /* -------------------------------------------------------- 交互动作 */

  function spin(button) {
    if (!button) return;
    button.classList.add('spinning');
    setTimeout(function () { button.classList.remove('spinning'); }, 700);
  }

  function refreshNow(button) { spin(button); fetchStatus(true); }

  function copySummary() {
    var p = state.payload;
    if (!p) { toast('暂无数据可复制'); return; }

    var s = p.system || {}, c = p.cpu || {}, g = p.gpu || {}, npu = p.npu || {};
    var net = p.network || {}, v = p.version || {};
    var list = net.interfaces || [];
    var iface = (net.primary_index >= 0) ? list[net.primary_index] : null;

    var cpuModel = [];
    if (c.soc) cpuModel.push(c.soc);
    if (c.core_name && c.cores) cpuModel.push(c.cores + '× ' + c.core_name);
    if (!cpuModel.length && c.model) cpuModel.push(c.model);

    var lines = [
      'Slhbond-Monitor 系统信息',
      '平台类型: ' + (s.platform || '') + (s.arch ? ' · ' + s.arch : ''),
      '系统版本: ' + orNA(s.os_version),
      '主机名: ' + orNA(s.hostname),
      '系统运行时长: ' + orNA(s.uptime_human),
      'IPv4 地址: ' + orNA(s.ipv4),
      'IPv6 地址: ' + orNA(s.ipv6),
      'CPU 型号: ' + (cpuModel.join(' · ') || '未知'),
      '核心/线程: ' + (c.cores || '—') + ' / ' + (c.threads || '—'),
      '平均使用率: ' + (typeof c.usage_percent === 'number' ? c.usage_percent.toFixed(1) + '%' : '—'),
      '单线程最高占用: ' + (typeof c.max_core_percent === 'number' ? c.max_core_percent.toFixed(1) + '%' : '—'),
      'CPU 最高温度: ' + (typeof c.temp_c === 'number' ? c.temp_c.toFixed(1) + ' °C' : '未获取'),
      'GPU: ' + (g.present ? (g.model + ' / ' + g.driver + ' / '
               + (formatFreq(Math.round((g.freq_cur_hz || 0) / 1000)) || '—')) : '未检测到'),
      '网络: ' + (iface ? (iface.name + ' ↓' + formatRate(iface.rx_bps)
               + ' ↑' + formatRate(iface.tx_bps)) : '未检测到'),
      'NPU: ' + (npu.state === 'ready' ? '已就绪'
               : (npu.state === 'hardware_only' ? '驱动未加载' : '未检测到'))
    ];

    ((p.storage && p.storage.disks) || []).forEach(function (d) {
      lines.push('硬盘 ' + d.device + ' (' + (d.name || '') + '): '
                 + (HEALTH_TEXT[d.health] || '未知') + '，总 ' + formatBytes(d.size_bytes)
                 + '，可用 ' + (d.mounted ? formatBytes(d.fs_free_bytes) : '未挂载'));
    });

    lines.push('版本: v' + (v.current || ''));
    var text = lines.join('\n');

    function done(ok) { toast(ok ? '已复制到剪贴板' : '复制失败，请手动选择文本'); }

    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(
        function () { done(true); },
        function () { done(legacyCopy(text)); });
      return;
    }
    done(legacyCopy(text));
  }

  function legacyCopy(text) {
    var area = document.createElement('textarea');
    area.value = text;
    area.setAttribute('readonly', '');
    area.style.position = 'fixed';
    area.style.top = '-1000px';
    document.body.appendChild(area);
    area.select();
    var ok = false;
    try { ok = document.execCommand('copy'); } catch (e) { ok = false; }
    document.body.removeChild(area);
    return ok;
  }

  /* ================================================================== */
  /* 检查版本                                                             */
  /* ================================================================== */

  var checking = false;

  /** 两行：上次/下次 一行，每日定时一行 —— 侧栏太窄，挤一行会断在词中间 */
  function renderCheckLine(version) {
    if (!version || !version.checked_ts) {
      el.checkLine.textContent = version && version.update_check_enabled
        ? '尚未检查'
        : '未启用更新检查';
      return;
    }

    var now = Math.floor(Date.now() / 1000);
    var next = version.next_check_at || 0;
    var clock = formatClock(version.checked_ts).slice(11, 16);
    var line1;
    var line2 = '';

    if (version.last_check_ok === false && version.last_error) {
      /* 检查过但失败：原因直接写在这里，不必点开控制台 */
      line1 = '上次 ' + clock + ' 失败';
      line2 = version.last_error.slice(0, 26);
    } else {
      line1 = '上次 ' + clock;
      if (next > now)
        line1 += ' · 下次 ' + formatAge(next - now) + '后';
    }

    if (typeof version.daily_hour === 'number' && version.daily_hour >= 0)
      line2 = '每天 ' + pad2(version.daily_hour) + ':00 自动检查';
    else if (version.interval_sec > 0)
      line2 = '每 ' + formatAge(version.interval_sec) + '自动检查';
    else
      line2 = '仅手动检查';

    el.checkLine.innerHTML = '<span>' + esc(line1) + '</span>'
      + (line2 ? '<span>' + esc(line2) + '</span>' : '');
  }

  function runCheck() {
    if (checking) return;
    checking = true;

    var span = el.checkBtn.querySelector('span');
    var label = span.textContent;

    span.textContent = '检查中…';
    el.checkBtn.disabled = true;
    el.checkBtn.dataset.state = 'busy';

    fetch('/api/v1/update/check', { method: 'POST' })
      .then(function (r) {
        return r.json().then(function (d) { return { ok: r.ok, body: d }; });
      })
      .then(function (res) {
        var d = res.body || {};

        if (!res.ok || d.checked === false) {
          toast('检查失败：'
            + (d.error || d.last_error || d.message || '未知原因'));
        } else if (d.can_apply) {
          toast('发现新版本 v' + (d.version || '') + '，点下面的「更新可用」即可自动更新');
        } else {
          toast('已是最新版本 v' + (d.version || ''));
        }
        /* 立刻刷新一次，让「更新可用」按钮和版本号同步 */
        fetchStatus(true);
      })
      .catch(function () {
        toast('检查失败：请求超时或网络不可达');
      })
      .then(function () {
        checking = false;
        el.checkBtn.disabled = false;
        el.checkBtn.dataset.state = '';
        span.textContent = label;
      });
  }

  /* ================================================================== */
  /* 自更新                                                              */
  /* ================================================================== */

  var updatePoll = null;

  function setUpdateBtnText(text) {
    var span = el.updateBtn.querySelector('span');

    if (span) span.textContent = text;
  }

  function pollUpdate() {
    fetch('/api/v1/update')
      .then(function (r) { return r.json(); })
      .then(function (d) {
        if (d.phase === 'staged') {
          if (updatePoll) { clearInterval(updatePoll); updatePoll = null; }
          setUpdateBtnText('等待应用');
          el.updateBtn.disabled = true;
          toast('v' + (d.version || '新版本') + ' 已下载并校验完成，'
                + '系统会在 5 分钟内自动应用并重启服务');
          return;
        }
        if (d.phase === 'failed') {
          if (updatePoll) { clearInterval(updatePoll); updatePoll = null; }
          setUpdateBtnText('更新失败');
          el.updateBtn.disabled = false;
          toast('更新失败：' + (d.error || '未知原因'));
          return;
        }
        setUpdateBtnText(d.percent >= 0 ? ('下载中 ' + d.percent + '%') : '下载中…');
      })
      .catch(function () { /* 忽略：下一轮再试 */ });
  }

  function startUpdate() {
    el.updateBtn.disabled = true;
    setUpdateBtnText('准备下载…');

    fetch('/api/v1/update/apply', { method: 'POST' })
      .then(function (r) {
        return r.json().then(function (d) { return { ok: r.ok, body: d }; });
      })
      .then(function (res) {
        if (!res.ok) {
          el.updateBtn.disabled = false;
          setUpdateBtnText('更新可用');
          toast('无法更新：' + ((res.body && (res.body.message || res.body.error))
                              || '未知原因'));
          return;
        }
        toast('已开始下载，完成后会自动应用');
        if (!updatePoll) updatePoll = setInterval(pollUpdate, 1500);
        pollUpdate();
      })
      .catch(function () {
        el.updateBtn.disabled = false;
        setUpdateBtnText('更新可用');
        toast('无法更新：请求失败');
      });
  }

  function openUpdate() {
    var v = (state.payload && state.payload.version) || {};
    var u = (state.payload && state.payload.update) || {};

    if (u.staged) {
      toast('v' + (u.version || v.latest || '') + ' 已就绪，等待系统应用');
      return;
    }
    if (v.release_url && !v.update_available) {
      window.open(v.release_url, '_blank', 'noopener');
      return;
    }
    if (!v.update_available) {
      toast(v.notes ? ('v' + v.latest + '：' + v.notes) : '已是最新版本');
      return;
    }
    if (u.can_apply === false) {
      toast(v.notes || ('v' + v.latest + ' 可用，但清单里没有可下载的地址'));
      return;
    }
    if (!u.download_ready) {
      toast('系统里没有 curl 或 wget，无法从 https 下载更新包');
      return;
    }
    startUpdate();
  }

  /* ------------------------------------------------------------ 启动 */

  /* ================================================================== */
  /* 第一行右侧：警告 / 溯源 两个方块                                     */
  /* ================================================================== */

  function esc(s) {
    return String(s === null || s === undefined ? '' : s)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
  }

  function pad2(n) { return (n < 10 ? '0' : '') + n; }

  function formatClock(ts) {
    var d = new Date(ts * 1000);
    return d.getFullYear() + '-' + pad2(d.getMonth() + 1) + '-' + pad2(d.getDate())
      + ' ' + pad2(d.getHours()) + ':' + pad2(d.getMinutes()) + ':' + pad2(d.getSeconds());
  }

  function formatAge(seconds) {
    if (seconds < 60) return seconds + ' 秒';
    if (seconds < 3600) return Math.floor(seconds / 60) + ' 分钟';
    if (seconds < 86400) return (seconds / 3600).toFixed(1) + ' 小时';
    return (seconds / 86400).toFixed(1) + ' 天';
  }

  function formatSize(bytes) {
    var units = ['B', 'KB', 'MB', 'GB'];
    var i = 0;
    var v = bytes || 0;
    while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
    return (i === 0 ? v : v.toFixed(1)) + ' ' + units[i];
  }

  function trimNum(v) {
    if (typeof v !== 'number') return '—';
    return (Math.round(v * 10) / 10).toString();
  }

  /** 把 /api/v1/status 里的三个摘要刷到方块上。 */
  function renderTiles(payload) {
    var a = (payload && payload.alerts) || null;
    var h = (payload && payload.history) || null;

    if (a && el.tileAlert) {
      var total = a.total || 0;
      var active = a.active || 0;
      var metrics = a.metric_count || 0;

      el.alertCount.textContent = String(total);
      el.tileAlert.dataset.state = active > 0 ? 'active' : (total > 0 ? 'warn' : 'ok');
      el.alertSub.textContent = active > 0
        ? (active + ' 项正在超限')
        : (metrics + ' 项指标 · 超标 ' + (a.hold_sec || 10) + ' 秒即记录');
      el.tileAlert.title = '监控 ' + metrics + ' 项指标，点开可查看记录并修改阈值';
    }

    if (h && el.tileTrace) {
      var stored = h.stored || 0;
      var cap = h.capacity || 0;

      el.traceCount.textContent = String(stored);
      el.tileTrace.dataset.state = stored > 0 ? 'ok' : 'idle';
      if (stored > 1 && h.oldest && h.newest) {
        el.traceSub.textContent = formatAge(Math.max(0, h.newest - h.oldest))
          + ' · ' + formatSize(h.disk_bytes);
      } else {
        el.traceSub.textContent = '每秒 1 条 · 保留 '
          + Math.round(cap / 3600) + ' 小时';
      }
    }

    /* 删除类操作是最该被一眼看到的，所以在「事件」按钮上挂个计数 */
    var c = (payload && payload.commands) || null;
    if (c && el.traceEventBadge) {
      var dels = c.deletes || 0;

      el.traceEventBadge.textContent = dels ? String(dels) : '';
      el.traceEventBadge.classList.toggle('hidden', !dels);
      el.traceEvents.title = dels
        ? ('查看开机 / 关机 / 登录与命令记录（其中删除类操作 ' + dels + ' 次）')
        : '查看开机 / 关机 / 登录与命令记录';
    }
  }

  /* ---------------------------------------------------------- 面板开关 */

  function openPanel(node) {
    if (!node) return;
    node.classList.remove('hidden');
    hideTooltip();
  }

  function closePanel(node) {
    if (node) node.classList.add('hidden');
  }

  function anyPanelOpen() {
    return (el.alertPanel && !el.alertPanel.classList.contains('hidden'))
        || (el.eventPanel && !el.eventPanel.classList.contains('hidden'));
  }

  /* ------------------------------------------------------------ 警告面板 */

  function loadAlerts() {
    fetch('/api/v1/alerts?limit=200')
      .then(function (r) { return r.json(); })
      .then(renderAlerts)
      .catch(function () {
        el.alertList.innerHTML = '<li class="rec-empty">读取失败，请检查服务是否在运行。</li>';
      });
  }

  function renderAlerts(d) {
    var metrics = d.metrics || [];

    buildThresholdGrid(metrics, (d.thresholds || {}).hold_sec);
    renderMetricStates(d.states);

    el.alertTotal.textContent = '共 ' + (d.total || 0) + ' 条'
      + (d.active ? '，' + d.active + ' 项进行中' : '');

    var list = el.alertList;
    var items = d.alerts || [];

    list.innerHTML = '';
    if (!items.length) {
      list.innerHTML = '<li class="rec-empty">还没有超限记录。'
        + '某个指标连续超过阈值达到设定时长后，这里会自动出现一条；'
        + '各指标当前距离记录还差多久，见上面的实时状态。</li>';
      return;
    }

    items.forEach(function (a) {
      var li = document.createElement('li');
      var sub = '峰值 ' + a.peak.toFixed(1) + '　阈值 ' + a.threshold.toFixed(1)
              + '　持续 ' + a.duration_sec + ' 秒';

      li.dataset.kind = a.ongoing ? 'ongoing' : a.metric;
      li.innerHTML =
        '<div class="rec-top"><span class="rec-title">' + esc(a.metric_name)
        + (a.ongoing ? ' · 正在进行' : '')
        + (a.interrupted ? ' · 服务停止而中断' : '') + '</span>'
        + '<span class="rec-time">' + formatClock(a.ts) + '</span></div>'
        + '<div class="rec-sub">' + esc(sub)
        + (a.ended_ts ? '　结束于 ' + formatClock(a.ended_ts) : '') + '</div>';
      list.appendChild(li);
    });
  }

  /* 阈值输入框按服务端下发的指标表生成，加指标不用改前端 */
  var thrInputs = {};
  var thrLive = {};

  function buildThresholdGrid(metrics, holdSec) {
    var grid = el.thrGrid;

    grid.innerHTML = '';
    thrInputs = {};

    metrics.forEach(function (m) {
      var label = document.createElement('label');
      var input = document.createElement('input');

      /* 测不到的指标样式上区分开（虚线边框 + 悬停提示），
       * 但不在界面上堆一行说明文字。 */
      label.className = 'thr-item' + (m.available ? '' : ' thr-off');
      if (!m.available)
        label.title = '该设备测不到这个指标，暂不参与判断；设定值会保留';
      label.innerHTML = '<span class="thr-name">' + esc(m.name)
        + ' <span class="unit">' + esc(m.unit) + '</span></span>';

      input.type = 'number';
      input.min = String(m.min);
      input.max = String(m.max);
      input.step = '1';
      input.value = String(Math.round(m.threshold));
      input.dataset.key = m.key;
      /* 测不到也允许设定：换内核或插上硬盘后这个值立刻就用得上 */
      label.appendChild(input);

      /* 实时值 + 距离记录还差多久，解释"为什么还没记录" */
      var live = document.createElement('span');
      live.className = 'thr-live';
      label.appendChild(live);
      thrLive[m.key] = live;

      grid.appendChild(label);
      thrInputs[m.key] = input;
    });

    var hlabel = document.createElement('label');
    var hinput = document.createElement('input');

    hlabel.className = 'thr-item';
    hlabel.innerHTML = '<span class="thr-name">持续时长 <span class="unit">秒</span></span>';
    hinput.type = 'number';
    hinput.min = '1';
    hinput.max = '3600';
    hinput.step = '1';
    hinput.value = String(holdSec || 10);
    hlabel.appendChild(hinput);
    grid.appendChild(hlabel);
    thrInputs.hold = hinput;

    el.thrGrid.addEventListener('keydown', function (ev) {
      if (ev.key === 'Enter') saveThresholds();
    }, { once: true });
  }

  /** 把每个指标的当前值与"已超限多久"刷到对应的输入框下面。 */
  function renderMetricStates(states) {
    (states || []).forEach(function (s) {
      var node = thrLive[s.key];

      if (!node)
        return;
      if (!s.available) {
        node.textContent = '未检测到';
        node.dataset.level = 'off';
        return;
      }

      var text = '当前 ' + s.current.toFixed(1) + s.unit;
      if (s.active) {
        text += '　已记录中 ' + s.pending_sec + ' 秒';
        node.dataset.level = 'active';
      } else if (s.pending_sec > 0) {
        text += '　已超限 ' + s.pending_sec + ' / ' + s.hold_sec + ' 秒';
        node.dataset.level = 'pending';
      } else {
        node.dataset.level = 'idle';
      }
      node.textContent = text;
    });
  }

  function saveThresholds() {
    var parts = [];

    Object.keys(thrInputs).forEach(function (k) {
      var v = thrInputs[k].value;

      if (v !== '' && v !== null)
        parts.push(k + '=' + encodeURIComponent(v));
    });
    if (!parts.length) {
      el.thrStatus.textContent = '没有可保存的值';
      return;
    }

    el.thrStatus.textContent = '保存中…';
    el.thrSave.disabled = true;

    fetch('/api/v1/alerts/config?' + parts.join('&'), { method: 'POST' })
      .then(function (r) {
        return r.json().then(function (d) { return { ok: r.ok, body: d }; });
      })
      .then(function (res) {
        el.thrSave.disabled = false;
        if (res.ok) {
          el.thrStatus.textContent = '已保存';
          toast('告警阈值已保存');
          fetchStatus(true);
        } else {
          el.thrStatus.textContent = (res.body && (res.body.message || res.body.error))
            || '保存失败';
        }
      })
      .catch(function () {
        el.thrSave.disabled = false;
        el.thrStatus.textContent = '保存失败';
      });
  }

  /* ------------------------------------------------------------ 事件面板 */

  function loadEvents() {
    var svc = (el.eventShowService && el.eventShowService.checked) ? '&service=1' : '';

    fetch('/api/v1/events?limit=200' + svc)
      .then(function (r) { return r.json(); })
      .then(renderEvents)
      .catch(function () {
        el.eventList.innerHTML = '<li class="rec-empty">读取失败。</li>';
      });
  }

  /* 只有前两个是「整机」的开关机，其余都是别的性质 */
  var EVENT_KIND = {
    '开机': 'boot', '关机': 'shutdown',
    '登录': 'login', '登出': 'logout',
    '服务启动': 'service', '服务停止': 'service'
  };

  function renderEvents(d) {
    var notes = [];

    if (d.just_booted)
      notes.push('本次启动检测到整机重启过（内核 boot_id 变了）。');
    else
      notes.push('本次启动与上次是**同一次开机**，只是监控服务被重启 —— '
                 + '不算开关机，所以不会出现在下面的记录里。');

    notes.push('开机时间由 /proc/uptime 反推，是真实的内核启动时刻；'
               + '关机时间取自系统关闭前服务收到的退出信号，'
               + '如果那次是断电或强制重启，会标明「关机时刻无法确定」。');

    if (!d.login_tracked)
      notes.push('登录记录不可用：' + (d.login_note || '未知原因'));
    else
      notes.push('登录记录读取自 /var/log/wtmp，每 10 秒补齐一次。');

    el.eventNote.innerHTML = notes.map(function (t) {
      return t.replace(/\*\*(.+?)\*\*/g, '<b>$1</b>');
    }).join(' ');

    el.eventCount.textContent = '共 ' + (d.total || 0) + ' 条';

    var list = el.eventList;
    var items = d.events || [];

    list.innerHTML = '';
    if (!items.length) {
      list.innerHTML = '<li class="rec-empty">暂无记录。'
        + (d.service_shown ? '' : '勾选上面的「显示服务启停」可以看到服务重启记录。')
        + '</li>';
      return;
    }

    items.forEach(function (e) {
      var li = document.createElement('li');
      var sub = [];

      if (e.user) sub.push('用户 ' + e.user);
      if (e.ip) sub.push('来源 ' + e.ip);
      if (e.detail) sub.push(e.detail);

      li.dataset.kind = e.unclean ? 'unclean' : (EVENT_KIND[e.type] || 'logout');
      li.innerHTML =
        '<div class="rec-top"><span class="rec-title">' + esc(e.type)
        + (e.unclean ? '（异常）' : '') + '</span>'
        + '<span class="rec-time">' + formatClock(e.ts) + '</span></div>'
        + (sub.length ? '<div class="rec-sub">' + esc(sub.join('　')) + '</div>' : '');
      list.appendChild(li);
    });
  }

  /* ------------------------------------------------------------ 命令记录 */

  function loadCommands() {
    var only = (el.cmdOnlyDeletes && el.cmdOnlyDeletes.checked) ? '&deletes=1' : '';

    fetch('/api/v1/commands?limit=200' + only)
      .then(function (r) { return r.json(); })
      .then(renderCommands)
      .catch(function () {
        el.cmdList.innerHTML = '<li class="rec-empty">读取失败。</li>';
      });
  }

  function renderCommands(d) {
    el.cmdCount.textContent = '共 ' + (d.total || 0) + ' 条'
      + (d.deletes ? '，其中删除类 ' + d.deletes + ' 次' : '');

    el.cmdNote.textContent = d.history_tracked
      ? '进程列表每秒扫描一次，存活不足 1 秒的进程可能漏掉；'
        + 'shell 历史每 10 秒补齐一次，两者合起来能覆盖绝大多数交互式命令。'
      : ('实时进程扫描可用，但 shell 历史不可用：' + (d.history_note || '未知原因'));

    var list = el.cmdList;
    var items = d.commands || [];

    list.innerHTML = '';
    if (!items.length && !d.deletes_only) {
      items = d.commands || [];
    }
    if (!items.length) {
      list.innerHTML = '<li class="rec-empty">'
        + (d.deletes_only ? '没有删除类操作的记录。' : '暂无命令记录。') + '</li>';
      return;
    }

    items.forEach(function (c) {
      var li = document.createElement('li');
      var sub = [];

      if (c.user) sub.push('用户 ' + c.user);
      if (c.pid) sub.push('PID ' + c.pid);
      sub.push(c.source === 'history' ? '来自 shell 历史' : '实时捕获');
      if (c.target) sub.push('目标 ' + c.target);

      li.dataset.kind = c.is_delete ? 'delete' : 'cmd';
      li.innerHTML =
        '<div class="rec-top"><span class="rec-title">'
        + (c.is_delete ? '<em class="del-tag">删除</em> ' : '')
        + esc(c.command || '') + '</span>'
        + '<span class="rec-time">' + formatClock(c.ts) + '</span></div>'
        + '<div class="rec-sub">' + esc(sub.join('　')) + '</div>';
      list.appendChild(li);
    });
  }

  /* ================================================================== */
  /* 溯源：时间轴回看                                                     */
  /* ================================================================== */

  /* 服务端把每条记录压成定长数组，这里的顺序必须与 api.c 一致。 */
  var HIST_FIELDS = ['ts', 'cpu', 'cpuMax', 'mem', 'tempC10', 'freqMhz',
                     'load1x100', 'netRx', 'netTx', 'diskR', 'diskW',
                     'gpu', 'npu', 'gpuTempC10', 'gpuFreqMhz', 'diskTempC10'];

  /* 溯源时波形用的窗口长度，与网络卡片的实时窗口保持一致 */
  var TRACE_WAVE = 48;

  var trace = {
    active: false,
    records: [],
    meta: null,
    index: 0,
    base: null,      /* 最后一次实时快照，用于补齐历史里没存的静态字段 */
    bootTime: 0,
    playing: false,
    timer: null,
    loading: false
  };

  function histRecord(i) {
    var row = trace.records[i];
    if (!row) return null;

    var o = {};
    var n;
    for (n = 0; n < HIST_FIELDS.length; n++) o[HIST_FIELDS[n]] = row[n];
    o.core = row.slice(HIST_FIELDS.length);
    return o;
  }

  function copyShallow(o) {
    var out = {};
    var k;
    for (k in o) if (Object.prototype.hasOwnProperty.call(o, k)) out[k] = o[k];
    return out;
  }

  /**
   * 历史记录只存了随时间变化的量，卡片上还有型号、驱动、MAC 这类静态字段，
   * 所以用最后一次实时快照做底、用历史值覆盖动态部分。
   */
  function traceSystem(r) {
    var base = (trace.base && trace.base.system) || {};
    var o = copyShallow(base);

    if (trace.bootTime && r.ts > trace.bootTime)
      o.uptime_seconds = r.ts - trace.bootTime;
    return o;
  }

  function traceCpu(r) {
    var base = (trace.base && trace.base.cpu) || {};
    var o = copyShallow(base);

    o.usage_percent = r.cpu >= 0 ? r.cpu : null;
    o.max_core_percent = r.cpuMax >= 0 ? r.cpuMax : null;
    o.temp_c = r.tempC10 >= 0 ? r.tempC10 / 10 : null;
    o.freq_cur_khz = r.freqMhz > 0 ? r.freqMhz * 1000 : 0;
    o.core_usage = r.core.map(function (v) { return v >= 0 ? v : null; });
    return o;
  }

  /** 从历史窗口重建网络波形，而不是沿用实时曲线。 */
  function traceNetwork(r) {
    var base = (trace.base && trace.base.network) || {};
    var o = copyShallow(base);
    var idx = (typeof o.primary_index === 'number' && o.primary_index >= 0)
      ? o.primary_index : 0;

    var rx = [];
    var tx = [];
    var start = Math.max(0, trace.index - TRACE_WAVE + 1);

    for (var i = start; i <= trace.index; i++) {
      var rec = histRecord(i);
      if (!rec) continue;
      rx.push(rec.netRx > 0 ? rec.netRx : 0);
      tx.push(rec.netTx > 0 ? rec.netTx : 0);
    }

    o.interfaces = (base.interfaces || []).map(function (itf, i) {
      var c = copyShallow(itf);

      if (i === idx) {
        c.rx_bps = r.netRx >= 0 ? r.netRx : null;
        c.tx_bps = r.netTx >= 0 ? r.netTx : null;
        c.rx_history = rx;
        c.tx_history = tx;
        /* 峰值与累计量是本次开机的，历史里没有，不要拿来冒充那一刻的值 */
        c.rx_peak_bps = null;
        c.tx_peak_bps = null;
      }
      return c;
    });
    o.history_length = TRACE_WAVE;
    return o;
  }

  function traceGpu(r) {
    var base = (trace.base && trace.base.gpu) || {};
    var o = copyShallow(base);

    /* 记录里存的是"上电占比"，不是利用率 —— 和卡片的口径一致 */
    o.active_ratio_percent = r.gpu >= 0 ? r.gpu : null;
    o.temp_c = r.gpuTempC10 >= 0 ? r.gpuTempC10 / 10 : null;
    o.freq_cur_hz = r.gpuFreqMhz > 0 ? r.gpuFreqMhz * 1000000 : 0;
    return o;
  }

  function traceNpu(r) {
    var base = (trace.base && trace.base.npu) || {};
    var o = copyShallow(base);

    if (r.npu >= 0) {
      o.load_percent = r.npu;
      o.load_available = true;
    } else {
      o.load_available = false;
    }
    return o;
  }

  function showTraceFrame(i) {
    var r = histRecord(i);
    if (!r) return;

    trace.index = i;
    el.traceSlider.value = String(i);
    el.traceSlider.style.setProperty('--fill',
      (trace.records.length > 1 ? (i / (trace.records.length - 1)) * 100 : 100).toFixed(2) + '%');

    el.traceTime.textContent = formatClock(r.ts);
    el.traceAgo.textContent = '（' + formatAge(Math.max(0, Math.floor(Date.now() / 1000) - r.ts)) + '前）';

    /* 时间轴在动时同步输入框，方便以某一刻为基准再微调 */
    if (!el.tracePicker.classList.contains('hidden'))
      el.tracePick.value = tsToLocalInput(r.ts);

    /* 磁盘温度历史上也有，放在时间轴上一起显示 */
    el.traceDisk.textContent = r.diskTempC10 >= 0
      ? ('磁盘 ' + (r.diskTempC10 / 10).toFixed(1) + '°C')
      : '磁盘温度未上报';

    renderSystemFields({ system: traceSystem(r),
                         version: (trace.base && trace.base.version) || {} });
    renderCpu(traceCpu(r));
    renderGpu(traceGpu(r));
    renderNetwork(traceNetwork(r));
    renderNpu(traceNpu(r));
  }

  /* ------------------------------------------------- 手动选择日期与时间 */

  /** epoch 秒 -> "YYYY-MM-DDTHH:MM:SS"（本地时区，datetime-local 要的格式） */
  function tsToLocalInput(ts) {
    var d = new Date(ts * 1000);

    return d.getFullYear() + '-' + pad2(d.getMonth() + 1) + '-' + pad2(d.getDate())
      + 'T' + pad2(d.getHours()) + ':' + pad2(d.getMinutes()) + ':' + pad2(d.getSeconds());
  }

  /**
   * "YYYY-MM-DDTHH:MM:SS" -> epoch 秒。
   *
   * 不用 new Date(string)：不同浏览器对不带时区的字符串解释不一致，
   * 有的当本地时间、有的当 UTC。手动拆字段再交给 Date 构造函数才可控。
   */
  function localInputToTs(value) {
    var m = /^(\d{4})-(\d{2})-(\d{2})[T ](\d{2}):(\d{2})(?::(\d{2}))?$/.exec(value || '');

    if (!m)
      return null;
    return Math.floor(new Date(Number(m[1]), Number(m[2]) - 1, Number(m[3]),
                               Number(m[4]), Number(m[5]),
                               Number(m[6] || 0)).getTime() / 1000);
  }

  /** 记录按时间有序，二分找最接近的一条。 */
  function indexForTs(ts) {
    var lo = 0;
    var hi = trace.records.length - 1;

    while (lo < hi) {
      var mid = (lo + hi) >> 1;

      if (histRecord(mid).ts < ts) lo = mid + 1; else hi = mid;
    }
    if (lo > 0) {
      var before = Math.abs(histRecord(lo - 1).ts - ts);
      var at = Math.abs(histRecord(lo).ts - ts);

      if (before <= at) return lo - 1;
    }
    return lo;
  }

  function jumpToTs(ts) {
    if (!trace.records.length || ts === null || isNaN(ts))
      return;

    var first = histRecord(0).ts;
    var last = histRecord(trace.records.length - 1).ts;
    var note = '';

    if (ts < first) {
      ts = first;
      note = '所选时间早于已保存的最早记录，已跳到最早一条。';
    } else if (ts > last) {
      ts = last;
      note = '所选时间晚于最新记录，已跳到最新一条。';
    }

    stopTracePlay();
    showTraceFrame(indexForTs(ts));
    el.tracePickNote.textContent = note
      || ('已跳到 ' + el.traceTime.textContent
          + '（' + formatAge(Math.max(0, last - first)) + '窗口内）');
  }

  function togglePicker(force) {
    if (!trace.active)
      return;

    var show = (force === undefined)
      ? el.tracePicker.classList.contains('hidden') : force;

    if (!show) {
      el.tracePicker.classList.add('hidden');
      return;
    }

    var first = histRecord(0).ts;
    var last = histRecord(trace.records.length - 1).ts;

    el.tracePick.value = tsToLocalInput(histRecord(trace.index).ts);
    el.tracePick.min = tsToLocalInput(first);
    el.tracePick.max = tsToLocalInput(last);
    el.tracePickNote.textContent = '可查范围：' + formatClock(first)
      + ' ~ ' + formatClock(last);
    el.tracePicker.classList.remove('hidden');

    /* 直接展开系统日历；没有这个 API 的浏览器退化成点输入框自己弹 */
    try {
      if (el.tracePick.showPicker)
        el.tracePick.showPicker();
    } catch (err) { /* 需要用户手势，忽略 */ }
    el.tracePick.focus();
  }

  function enterTrace() {
    if (trace.active || trace.loading) return;
    trace.loading = true;
    toast('正在读取历史记录…');

    fetch('/api/v1/history?limit=4000')
      .then(function (r) { return r.json(); })
      .then(function (d) {
        trace.loading = false;
        if (!d.records || d.records.length < 2) {
          toast('历史数据还不够，请等十几秒再试');
          return;
        }

        trace.records = d.records;
        trace.meta = d;
        trace.base = state.live || null;
        trace.bootTime = (state.live && state.live.events && state.live.events.boot_time) || 0;
        trace.active = true;
        trace.index = d.records.length - 1;

        el.traceSlider.min = 0;
        el.traceSlider.max = String(d.records.length - 1);
        el.traceSlider.value = String(trace.index);

        var span = d.newest && d.oldest ? (d.newest - d.oldest) : 0;
        el.traceMeta.textContent = d.records.length + ' 个采样点 · 每 '
          + d.interval_sec + ' 秒一条 · 跨度 ' + formatAge(span)
          + (d.decimated ? ' · 已抽稀' : '');

        document.body.classList.add('tracing');
        el.traceBar.classList.remove('hidden');
        el.liveNote.classList.remove('hidden');
        showTraceFrame(trace.index);
      })
      .catch(function () {
        trace.loading = false;
        toast('读取历史失败');
      });
  }

  function stopTracePlay() {
    trace.playing = false;
    if (trace.timer) { clearInterval(trace.timer); trace.timer = null; }
    if (el.tracePlay) el.tracePlay.textContent = '▶';
  }

  function toggleTracePlay() {
    if (trace.playing) { stopTracePlay(); return; }

    trace.playing = true;
    el.tracePlay.textContent = '❚❚';
    trace.timer = setInterval(function () {
      var next = trace.index + 1;
      if (next > trace.records.length - 1) { stopTracePlay(); return; }
      showTraceFrame(next);
    }, 250);
  }

  function exitTrace() {
    stopTracePlay();
    togglePicker(false);
    trace.active = false;
    trace.records = [];
    document.body.classList.remove('tracing');
    el.traceBar.classList.add('hidden');
    el.liveNote.classList.add('hidden');
    /* trace.active 已复位，render() 会正常走实时分支 */
    if (state.live) render(state.live);
  }

  function initTiles() {
    el.tileAlert.addEventListener('click', function () {
      openPanel(el.alertPanel);
      el.alertList.innerHTML = '<li class="rec-empty">读取中…</li>';
      loadAlerts();
    });
    el.alertPanelClose.addEventListener('click', function () { closePanel(el.alertPanel); });
    el.alertPanel.addEventListener('click', function (ev) {
      if (ev.target === el.alertPanel) closePanel(el.alertPanel);
    });
    el.thrSave.addEventListener('click', saveThresholds);

    el.tileTrace.addEventListener('click', enterTrace);
    el.traceExit.addEventListener('click', exitTrace);
    el.traceSlider.addEventListener('input', function () {
      stopTracePlay();
      showTraceFrame(parseInt(el.traceSlider.value, 10) || 0);
    });
    el.tracePlay.addEventListener('click', toggleTracePlay);

    /* --- 手动选择日期与时间 --- */
    el.traceTimeBtn.addEventListener('click', function (ev) {
      ev.stopPropagation();
      togglePicker();
    });
    el.tracePick.addEventListener('change', function () {
      jumpToTs(localInputToTs(el.tracePick.value));
    });
    Array.prototype.forEach.call(
      el.tracePicker.querySelectorAll('[data-jump]'),
      function (btn) {
        btn.addEventListener('click', function () {
          var v = btn.getAttribute('data-jump');

          if (v === 'oldest') {
            jumpToTs(histRecord(0).ts);
          } else if (v === 'newest') {
            jumpToTs(histRecord(trace.records.length - 1).ts);
          } else {
            jumpToTs(histRecord(trace.index).ts + parseInt(v, 10));
          }
        });
      });
    /* 点别处收起选择器 */
    document.addEventListener('click', function (ev) {
      if (!trace.active || el.tracePicker.classList.contains('hidden'))
        return;
      if (el.tracePicker.contains(ev.target) || el.traceTimeBtn.contains(ev.target))
        return;
      togglePicker(false);
    });

    el.traceEvents.addEventListener('click', function () {
      openPanel(el.eventPanel);
      el.eventList.innerHTML = '<li class="rec-empty">读取中…</li>';
      el.cmdList.innerHTML = '<li class="rec-empty">读取中…</li>';
      loadEvents();
      loadCommands();
    });    el.cmdOnlyDeletes.addEventListener('change', function () {
      el.cmdList.innerHTML = '<li class="rec-empty">读取中…</li>';
      loadCommands();
    });
    el.eventShowService.addEventListener('change', function () {
      el.eventList.innerHTML = '<li class="rec-empty">读取中…</li>';
      loadEvents();
    });
    el.eventPanelClose.addEventListener('click', function () { closePanel(el.eventPanel); });
    el.eventPanel.addEventListener('click', function (ev) {
      if (ev.target === el.eventPanel) closePanel(el.eventPanel);
    });

    document.addEventListener('keydown', function (ev) {
      if (ev.key === 'Escape') {
        if (anyPanelOpen()) {
          closePanel(el.alertPanel);
          closePanel(el.eventPanel);
        } else if (trace.active && !el.tracePicker.classList.contains('hidden')) {
          togglePicker(false);
        } else if (trace.active) {
          exitTrace();
        }
        return;
      }

      /* 溯源模式下用左右方向键逐秒微调 */
      if (trace.active && (ev.key === 'ArrowLeft' || ev.key === 'ArrowRight')) {
        if (ev.target && /^(INPUT|TEXTAREA|SELECT)$/.test(ev.target.tagName))
          return;
        stopTracePlay();
        showTraceFrame(Math.max(0, Math.min(trace.records.length - 1,
          trace.index + (ev.key === 'ArrowRight' ? 1 : -1))));
        ev.preventDefault();
      }
    });
  }

  /* ================================================================== */
  /* 主题：深色 / 浅色                                                    */
  /* ================================================================== */

  var THEME_KEY = 'slhbond-theme';

  function currentTheme() {
    return document.documentElement.getAttribute('data-theme') === 'light'
      ? 'light' : 'dark';
  }

  function applyTheme(theme, animate) {
    var root = document.documentElement;

    if (animate) {
      root.classList.add('theme-switching');
      /* 过渡结束后立刻摘掉，别让它影响之后的悬停动效 */
      setTimeout(function () { root.classList.remove('theme-switching'); }, 260);
    }

    if (theme === 'light')
      root.setAttribute('data-theme', 'light');
    else
      root.removeAttribute('data-theme');

    /* 同步浏览器自身的滚动条/表单控件配色 */
    var meta = document.querySelector('meta[name="color-scheme"]');
    if (meta) meta.setAttribute('content', theme === 'light' ? 'light' : 'dark');

    if (el.themeBtn) {
      var tip = theme === 'light' ? '切换到深色主题' : '切换到浅色主题';

      el.themeBtn.title = tip;
      el.themeBtn.setAttribute('aria-label', tip);
      el.themeBtn.setAttribute('aria-pressed', theme === 'light' ? 'true' : 'false');
    }
  }

  function initTheme() {
    var saved = null;

    try { saved = localStorage.getItem(THEME_KEY); } catch (e) { /* 忽略 */ }

    /* 首屏脚本已经设过属性了，这里只是把按钮的提示语对齐 */
    applyTheme(saved === 'light' ? 'light' : currentTheme(), false);

    if (!el.themeBtn) return;
    el.themeBtn.addEventListener('click', function () {
      var next = currentTheme() === 'light' ? 'dark' : 'light';

      applyTheme(next, true);
      try { localStorage.setItem(THEME_KEY, next); } catch (e) { /* 忽略 */ }
    });
  }

  function init() {
    ['platform', 'osversion', 'hostname', 'uptime', 'ipv4', 'ipv6'].forEach(function (k) {
      bindTooltip(fields[k]);
    });
    bindTooltip(el.cpuModel);

    el.btnRefresh.addEventListener('click', function () { refreshNow(el.btnRefresh); });
    el.btnCpuRefresh.addEventListener('click', function () { refreshNow(el.btnCpuRefresh); });
    el.btnCopy.addEventListener('click', copySummary);
    el.updateBtn.addEventListener('click', openUpdate);
    el.checkBtn.addEventListener('click', runCheck);

    initTheme();

    window.addEventListener('resize', function () {
      Object.keys(fields).forEach(function (k) { markTruncated(fields[k]); });
      markTruncated(el.cpuModel);
      hideTooltip();
    });
    window.addEventListener('scroll', hideTooltip, true);

    setAllFieldsPending();
    setOnline(false);
    el.statusText.textContent = '连接中…';

    initTiles();

    fetchStatus(false);
    restartPolling();
    startUptimeTicker();
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
