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

    diskGrid: $('diskGrid'), diskHint: $('diskHint')
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
    var system = (payload && payload.system) || {};
    var version = (payload && payload.version) || {};

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

    if (version.current) el.versionText.textContent = 'v' + version.current;
    renderUpdateButton(version);

    renderCpu(payload.cpu);
    renderGpu(payload.gpu);
    renderNetwork(payload.network);
    renderNpu(payload.npu);
    renderDisks(payload.storage);

    if (typeof payload.poll_interval_ms === 'number' && payload.poll_interval_ms > 0) {
      var next = payload.poll_interval_ms;
      if (next !== state.pollMs) { state.pollMs = next; restartPolling(); }
      el.refreshHint.textContent = '每 ' + Math.round(next / 1000) + ' 秒自动刷新';
    }
  }

  function renderUpdateButton(version) {
    var available = version && version.update_available === true;
    el.updateBtn.classList.toggle('hidden', !available);
    if (!available) return;
    el.updateVer.textContent = version.latest ? 'v' + version.latest : '';
    el.updateBtn.title = version.notes
      ? '新版本 ' + version.latest + '：' + version.notes
      : '新版本 ' + version.latest + ' 可用';
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

  function openUpdate() {
    var v = (state.payload && state.payload.version) || {};
    if (v.release_url) { window.open(v.release_url, '_blank', 'noopener'); return; }
    toast(v.notes ? ('v' + v.latest + '：' + v.notes) : '已是最新版本');
  }

  /* ------------------------------------------------------------ 启动 */

  function init() {
    ['platform', 'osversion', 'hostname', 'uptime', 'ipv4', 'ipv6'].forEach(function (k) {
      bindTooltip(fields[k]);
    });
    bindTooltip(el.cpuModel);

    el.btnRefresh.addEventListener('click', function () { refreshNow(el.btnRefresh); });
    el.btnCpuRefresh.addEventListener('click', function () { refreshNow(el.btnCpuRefresh); });
    el.btnCopy.addEventListener('click', copySummary);
    el.updateBtn.addEventListener('click', openUpdate);

    window.addEventListener('resize', function () {
      Object.keys(fields).forEach(function (k) { markTruncated(fields[k]); });
      markTruncated(el.cpuModel);
      hideTooltip();
    });
    window.addEventListener('scroll', hideTooltip, true);

    setAllFieldsPending();
    setOnline(false);
    el.statusText.textContent = '连接中…';

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
