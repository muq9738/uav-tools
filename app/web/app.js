// 界面逻辑：任务表格、添加弹窗、详情弹窗、简化日志与事件流。
// 无构建依赖，桌面窗口与浏览器共用。

const token = new URLSearchParams(location.search).get('token') || sessionStorage.getItem('token') || ''
if (token) sessionStorage.setItem('token', token)

const state = {
  config: null,
  jobs: [],
  search: '',
  pending: new Set(),
  // 详情弹窗状态
  selected: null,
  source: null,
  fallbackTimer: null,
  lines: [],
  sequence: 0,
  level: 'all',
  expanded: false,
}

const $ = selector => document.querySelector(selector)

function withToken(url) {
  if (!token) return url
  const parsed = new URL(url, location.origin)
  parsed.searchParams.set('token', token)
  return parsed.pathname + parsed.search
}

async function api(path, options = {}) {
  const response = await fetch(withToken(path), {
    ...options,
    headers: { 'Content-Type': 'application/json', ...(options.headers || {}) },
  })
  const text = await response.text()
  const payload = text ? JSON.parse(text) : {}
  if (!response.ok) throw new Error(payload.error || `请求失败（${response.status}）`)
  return payload
}

const stateLabel = value => ({
  queued: '排队中', running: '转换中', pausing: '正在暂停', paused: '已暂停',
  succeeded: '已完成', failed: '失败', canceled: '已取消',
}[value] || value)

function formatDuration(milliseconds) {
  const total = Math.max(0, Math.floor(milliseconds / 1000))
  const minutes = Math.floor(total / 60)
  const seconds = total % 60
  return minutes ? `${minutes} 分 ${String(seconds).padStart(2, '0')} 秒` : `${seconds} 秒`
}

function elapsed(job) {
  if (!job.startedAt) return '—'
  const endAt = job.finishedAt || (job.state === 'paused' ? job.pausedAt : null)
  const end = endAt ? new Date(endAt) : new Date()
  return formatDuration(end - new Date(job.startedAt))
}

function percent(job) {
  const { done = 0, total = 0 } = job.progress || {}
  if (!total) return job.state === 'succeeded' ? 100 : 0
  return Math.min(100, Math.round((done / total) * 100))
}

function progressText(job) {
  const { done = 0, total = 0 } = job.progress || {}
  if (job.state === 'pausing') return '等待当前文件处理完成后暂停'
  if (job.state === 'paused') {
    if (!state.config?.sidecar?.available || !state.config.sidecar.pauseResume) return '已暂停 · 当前转换器不支持继续'
    return job.resumeRequired ? `已暂停 · 已处理 ${done} 个文件，可继续转换` : '已暂停 · 等待继续后排队'
  }
  if (!total) return job.state === 'queued' ? '等待空闲转换器' : done > 0 ? `已处理 ${done} 个文件` : '扫描输入中'
  return `${done} / ${total} 个文件 · ${percent(job)}%`
}

const GENERIC_TASK_SEGMENTS = new Set([
  'out', 'output', 'outputs',
  '3dtiles', 'tiles', 'tileset',
  'b3dm', 'b3dms', 'glb', 'gltf',
  'result', 'results', 'dist', 'build', 'export',
  'terra_osgbs', 'terra_osgb', 'terra_b3dms', 'terra_b3dm', 'terra_pcd',
  'osgb', 'osgbs', 'data', 'model', 'models', 'pc',
  'block', 'blocks', 'production', 'productions',
])

function isGenericTaskSegment(seg) {
  const s = (seg || '').trim()
  if (!s || s === '.' || s === '..' || /^[a-zA-Z]:$/.test(s) || /^\d+$/.test(s)) return true
  const lower = s.toLowerCase()
  if (GENERIC_TASK_SEGMENTS.has(lower)) return true
  if (lower.endsWith('-3dtiles') || lower.endsWith('_3dtiles')) {
    return isGenericTaskSegment(lower.slice(0, -8))
  }
  return false
}

function splitPathSegments(path) {
  return String(path || '').split(/[\\/]+/).map(s => s.trim()).filter(Boolean)
}

function pickProjectSegment(path, stripOsgbFile = false) {
  const segs = splitPathSegments(path)
  if (stripOsgbFile && segs.length > 0 && segs[segs.length - 1].toLowerCase().endsWith('.osgb')) {
    segs.pop()
  }
  for (let i = segs.length - 1; i >= 0; i -= 1) {
    if (!isGenericTaskSegment(segs[i])) return segs[i]
  }
  return ''
}

function taskName(job) {
  const explicit = (job.spec?.name || '').trim()
  if (explicit && !isGenericTaskSegment(explicit)) return explicit
  const outSegs = splitPathSegments(job.spec?.output)
  if (outSegs.length > 0 && !isGenericTaskSegment(outSegs[outSegs.length - 1])) {
    return outSegs[outSegs.length - 1]
  }
  return pickProjectSegment(job.spec?.output, false)
    || pickProjectSegment(job.spec?.input, true)
    || outSegs[outSegs.length - 1]
    || job.id
}

function notify(text, isError = false) {
  const banner = $('#banner')
  banner.textContent = text
  banner.classList.toggle('error', isError)
  banner.hidden = false
  clearTimeout(banner.timer)
  banner.timer = setTimeout(() => { banner.hidden = true }, isError ? 12000 : 5000)
}

// ---------- 任务表格 ----------

function renderJobs() {
  const query = state.search.trim().toLowerCase()
  $('#task-count').textContent = String(state.jobs.length)
  const filtered = state.jobs.filter(job => !query
    || taskName(job).toLowerCase().includes(query)
    || job.spec.input.toLowerCase().includes(query)
    || job.spec.output.toLowerCase().includes(query))
  const body = $('#jobs')
  body.replaceChildren(...filtered.map(job => {
    const row = document.createElement('tr')
    row.dataset.id = job.id
    const active = ['queued', 'running', 'pausing', 'paused'].includes(job.state)

    const nameCell = document.createElement('td')
    const name = document.createElement('div')
    name.className = 'name'
    name.textContent = taskName(job)
    name.title = job.spec.input
    const sub = document.createElement('div')
    sub.className = 'sub'
    sub.textContent = new Date(job.createdAt).toLocaleString('zh-CN', { hour12: false })
    nameCell.append(name, sub)

    const stateCell = document.createElement('td')
    const badge = document.createElement('span')
    badge.className = `badge ${job.state}`
    badge.textContent = stateLabel(job.state)
    stateCell.append(badge)
    if (active) {
      const bar = document.createElement('div')
      bar.className = 'mini-progress'
      const fill = document.createElement('span')
      fill.style.width = `${percent(job)}%`
      bar.append(fill)
      const caption = document.createElement('div')
      caption.className = 'progress-caption'
      caption.textContent = progressText(job)
      stateCell.append(bar, caption)
    }

    const timeCell = document.createElement('td')
    timeCell.className = 'time col-time'
    timeCell.textContent = elapsed(job)

    const pathCell = document.createElement('td')
    pathCell.className = 'path'
    pathCell.textContent = job.spec.output
    pathCell.title = job.spec.output

    const opsCell = document.createElement('td')
    const ops = document.createElement('div')
    ops.className = 'ops'
    ops.append(...jobActions(job))
    if (job.state === 'succeeded') {
      ops.append(linkButton(job.spec.preview ? '保真预览' : '预览', () => openPreview(job.id), 'preview-link'))
      if (job.spec.preview || job.report?.preview?.status === 'complete') ops.append(linkButton('轻量预览', () => openPreview(job.id, true), 'preview-link'))
    }
    if (state.config?.revealAvailable && job.state === 'succeeded') {
      ops.append(linkButton('打开目录', async () => {
        try { await api(`/api/jobs/${job.id}/reveal`, { method: 'POST' }) }
        catch (error) { notify(error.message, true) }
      }))
    }
    ops.append(linkButton('详情', () => openDetail(job.id)))
    opsCell.append(ops)

    row.append(nameCell, stateCell, timeCell, pathCell, opsCell)
    row.addEventListener('click', event => {
      if (event.target.closest('button')) return
      openDetail(job.id)
    })
    return row
  }))
  $('#empty').hidden = filtered.length > 0
  const searching = query.length > 0
  $('#empty-title').textContent = searching ? '没有匹配的任务' : '还没有转换任务'
  $('#empty-description').textContent = searching ? '换个关键词试试' : '选择 OSGB 数据，开始转换'
  $('#empty-add').hidden = searching
}

function linkButton(text, onClick, extra = '') {
  const button = document.createElement('button')
  button.type = 'button'
  button.className = `link ${extra}`.trim()
  button.textContent = text
  button.addEventListener('click', async event => {
    event.stopPropagation()
    if (button.disabled) return
    button.disabled = true
    try { await onClick() }
    finally { button.disabled = false }
  })
  return button
}

function jobActions(job) {
  const buttons = []
  if (state.config?.sidecar?.available && state.config.sidecar.pauseResume) {
    if (['queued', 'running'].includes(job.state)) {
      buttons.push(linkButton('暂停', () => changeJob(job, 'pause')))
    } else if (job.state === 'paused') {
      buttons.push(linkButton('继续', () => changeJob(job, 'resume')))
    }
  }
  if (['queued', 'running', 'pausing', 'paused'].includes(job.state)) {
    buttons.push(linkButton('取消', async () => {
      if (confirm('取消这个任务？取消后将无法继续转换。')) await changeJob(job, 'cancel')
    }, 'danger'))
  }
  for (const button of buttons) button.disabled = state.pending.has(job.id)
  return buttons
}

async function changeJob(job, action) {
  if (state.pending.has(job.id)) return
  state.pending.add(job.id)
  renderJobs()
  if (state.selected === job.id) renderDetail(job)
  try {
    const updated = await api(`/api/jobs/${job.id}/${action}`, { method: 'POST' })
    applyJob(updated)
    notify(action === 'pause' ? (updated.state === 'paused' ? '任务已暂停' : updated.state === 'pausing' ? '已请求暂停，正在等待当前文件处理完成' : `任务${stateLabel(updated.state)}`)
      : action === 'resume' ? '任务已继续，正在排队转换' : '已请求取消任务')
  } catch (error) { notify(error.message, true) }
  finally {
    state.pending.delete(job.id)
    await refreshJobs()
  }
}

function applyJob(job) {
  const index = state.jobs.findIndex(item => item.id === job.id)
  if (index >= 0) state.jobs[index] = job
  else state.jobs.unshift(job)
  renderJobs()
  if (state.selected === job.id) renderDetail(job)
}

async function refreshJobs() {
  try {
    const result = await api('/api/jobs')
    state.jobs = result.jobs || []
    renderJobs()
    if (state.selected) {
      const job = state.jobs.find(item => item.id === state.selected)
      if (job) renderDetail(job)
    }
  } catch (error) {
    notify('无法读取任务列表：' + error.message, true)
  }
}

// ---------- Cesium 预览 ----------

function openPreview(id, lightweight = false) {
  const dialog = $('#preview-dialog')
  const entry = lightweight ? '/preview.html' : '/viewer.html'
  $('#preview-frame').src = withToken(`${entry}?job=${encodeURIComponent(id)}`)
  dialog.showModal()
}

$('#preview-dialog').addEventListener('close', () => {
  // 销毁 WebGL、Worker 与瓦片缓存，避免关闭预览后仍占用显存。
  $('#preview-frame').src = 'about:blank'
})

window.addEventListener('message', event => {
  if (event.origin === location.origin && event.data?.type === 'close-preview') {
    $('#preview-dialog').close()
  }
})

// ---------- 添加任务 ----------

function openNewJob() {
  $('#form-message').textContent = ''
  $('#new-job-dialog').showModal()
  $('#input').focus()
}

$('#add').addEventListener('click', openNewJob)
$('#sidebar-add').addEventListener('click', openNewJob)
$('#empty-add').addEventListener('click', openNewJob)

// 输入目录确定后自动给出输出目录，减少填写。
$('#input').addEventListener('change', () => {
  const input = $('#input').value.trim().replace(/[\\/]+$/, '')
  if (!input || $('#output').dataset.touched === '1') return
  if (!$('#output').value.trim()) $('#output').value = `${input}-3dtiles`
})
$('#output').addEventListener('input', () => { $('#output').dataset.touched = '1' })

$('#new-job').addEventListener('submit', async event => {
  event.preventDefault()
  const submit = $('#submit')
  const message = $('#form-message')
  submit.disabled = true
  message.textContent = ''
  const body = {
    input: $('#input').value.trim(),
    output: $('#output').value.trim(),
    threads: Number($('#threads').value) || 2,
    geometry: $('#geometry').value,
    memoryBudgetMiB: Number($('#memoryBudgetMiB').value),
    preview: $('#generatePreview').checked,
    rebuildTop: $('#rebuildTop').value === 'true',
    embedTextures: $('#embedTextures') ? $('#embedTextures').value === 'true' : false,
    topSealDistance: Number($('#topSealDistance').value),
    textureFormat: $('#textureFormat').value,
    dracoSpeed: Number($('#dracoSpeed').value),
    srs: $('#srs').value.trim(),
    origin: $('#origin').value.trim(),
  }
  try {
    const job = await api('/api/jobs', { method: 'POST', body: JSON.stringify(body) })
    rememberDefaults(body)
    $('#new-job-dialog').close()
    await refreshJobs()
    openDetail(job.id)
    notify('任务已提交，正在排队转换')
  } catch (error) {
    message.textContent = error.message
  } finally {
    submit.disabled = false
  }
})

for (const button of document.querySelectorAll('.pick')) {
  button.addEventListener('click', async () => {
    const target = $(`#${button.dataset.target}`)
    const previous = button.textContent
    button.disabled = true
    button.textContent = '…'
    try {
      const result = await api('/api/pick-directory', { method: 'POST', body: '{}' })
      if (result.path) {
        target.value = result.path
        target.dispatchEvent(new Event('change'))
      }
    } catch (error) {
      notify(error.message, true)
    } finally {
      button.disabled = false
      button.textContent = previous
    }
  })
}

// ---------- 任务详情与简化日志 ----------

function stopStreaming() {
  if (state.source) { state.source.close(); state.source = null }
  if (state.fallbackTimer) { clearInterval(state.fallbackTimer); state.fallbackTimer = null }
}

function openDetail(id) {
  stopStreaming()
  state.selected = id
  state.lines = []
  state.sequence = 0
  state.level = 'all'
  state.expanded = false
  const job = state.jobs.find(item => item.id === id)
  $('#detail-dialog').showModal()
  if (job) renderDetail(job)
  startStreaming(id)
}

$('#detail-dialog').addEventListener('close', () => {
  state.selected = null
  stopStreaming()
  refreshJobs()
})

function renderDetail(job) {
  const failed = job.state === 'failed'
  if (failed && !state.expanded) state.expanded = true
  if ($('#detail-title')) $('#detail-title').textContent = taskName(job)
  const container = $('#detail-body')
  container.replaceChildren()

  const status = document.createElement('div')
  status.className = 'detail-state'
  const badge = document.createElement('span')
  badge.className = `badge ${job.state}`
  badge.textContent = stateLabel(job.state)
  const time = document.createElement('span')
  time.className = 'time'
  time.textContent = elapsed(job)
  status.append(badge, time)
  status.append(...jobActions(job))
  if (job.state === 'succeeded') {
    status.append(linkButton(job.spec.preview ? '保真预览' : '预览模型', () => openPreview(job.id)))
    if (job.spec.preview || job.report?.preview?.status === 'complete') status.append(linkButton('轻量预览', () => openPreview(job.id, true)))
  }
  if (state.config?.revealAvailable && job.state === 'succeeded') {
    status.append(linkButton('打开输出目录', async () => {
      try { await api(`/api/jobs/${job.id}/reveal`, { method: 'POST' }) }
      catch (error) { notify(error.message, true) }
    }))
  }
  container.append(status)

  const bar = document.createElement('div')
  bar.className = `big-progress${failed ? ' failed' : ''}`
  const fill = document.createElement('span')
  fill.style.width = `${percent(job)}%`
  bar.append(fill)
  container.append(bar)
  const caption = document.createElement('div')
  caption.className = 'detail-progress-text'
  caption.textContent = progressText(job) + (job.progress?.source === 'output-count' ? '（按已产出文件估算）' : '')
  container.append(caption)

  const paths = document.createElement('div')
  paths.className = 'detail-paths'
  for (const [label, value] of [['输入', job.spec.input], ['输出', job.spec.output]]) {
    const item = document.createElement('div')
    item.className = 'item'
    const tag = document.createElement('span')
    tag.textContent = label
    const code = document.createElement('code')
    code.textContent = value
    code.title = value
    item.append(tag, code)
    paths.append(item)
  }
  container.append(paths)

  if (job.error) {
    const error = document.createElement('p')
    error.className = 'error-text'
    error.style.padding = '0'
    error.textContent = job.error
    container.append(error)
  }

  if (job.state === 'succeeded' && job.report) container.append(reportGrid(job.report))
  container.append(logSection(job))
  renderLog()
}

function reportGrid(report) {
  const entries = [
    ['输入文件', report.files],
    ['输出 GLB', report.glbs],
    ['原编码纹理', report.originalEncodedImages],
    ['纹理引用', report.imageReferences],
    ['三角面（全部 LOD）', report.trianglesAllLods],
    ['顶点（全部 LOD）', report.verticesAllLods],
    ['转换耗时', report.elapsedSeconds != null ? `${report.elapsedSeconds.toFixed(2)} 秒` : null],
    ['坐标舍入误差', report.coordinateRoundingErrorMeters != null ? `${(report.coordinateRoundingErrorMeters * 1000).toFixed(4)} 毫米` : null],
    ['几何编码', ({ raw: '原始数组', EXT_meshopt_compression: 'Meshopt（无量化）', KHR_draco_mesh_compression: 'Draco（有损）' })[report.geometryEncoding] || report.geometryEncoding],
    ['纹理编码', ({ original: '原图', uastc: 'KTX2-UASTC（有损）', etc1s: 'KTX2-ETC1S（有损）' })[report.textureFormat]],
    ['新增远景层', report.rebuildTop != null ? (report.topGlbs ? `${report.topGlbs} 个（有损，焊缝 ${report.topSealDistance ?? 2} 米）` : '无') : null],
    ['轻量预览', report.preview ? `${report.preview.triangles} 三角面 · ${report.preview.glbs} GLB · ${report.preview.streamingLod ? '流式 LOD + 量化 Meshopt + 原画质纹理' : 'Meshopt + UASTC'}` : null],
    ['位置量化误差', (report.geometryEncoding === 'KHR_draco_mesh_compression' || report.quantizeGeometry) && report.quantizationErrorMeters != null ? `${(report.quantizationErrorMeters * 1000).toFixed(3)} 毫米` : null],
    ['远景表面偏移（采样）', report.topAncestors?.length ? `${(Math.max(...report.topAncestors.map(top => top.bake?.maxSampledSurfaceDistanceMeters || 0)) * 1000).toFixed(3)} 毫米` : null],
    ['压缩纹理显存（估算）', report.textureResidency?.withMipChainBytes > 0 ? (() => {
      const r = report.textureResidency
      const tiers = Object.entries(r.tiers || {}).map(([k, n]) => `${k}×${n}`).join('，')
      return `${(r.withMipChainBytes / 1048576).toFixed(2)} MiB（较未压缩省 ${(r.savedVsRgba8 * 100).toFixed(1)}%${tiers ? `；${tiers}` : ''}）`
    })() : null],
    ['Draco 流复用', report.dracoStreams?.reused > 0 ? `复用 ${report.dracoStreams.reused} / ${report.dracoStreams.primitives} 条（几何不变，仅省体积）` : null],
  ].filter(([, value]) => value !== null && value !== undefined)
  const dl = document.createElement('dl')
  dl.className = 'report'
  for (const [label, value] of entries) {
    const cell = document.createElement('div')
    const dt = document.createElement('dt')
    dt.textContent = label
    const dd = document.createElement('dd')
    dd.textContent = String(value)
    cell.append(dt, dd)
    dl.append(cell)
  }
  return dl
}

function logSection(job) {
  const section = document.createElement('div')
  section.className = 'log-section'
  const toolbar = document.createElement('div')
  toolbar.className = 'log-toolbar'
  const title = document.createElement('span')
  title.className = 'title'
  title.textContent = '日志'
  const spacer = document.createElement('span')
  spacer.className = 'spacer'
  const filter = document.createElement('div')
  filter.className = 'segmented'
  for (const [value, text] of [['all', '全部'], ['warn', '警告'], ['error', '错误']]) {
    const button = document.createElement('button')
    button.type = 'button'
    button.textContent = text
    button.classList.toggle('active', state.level === value)
    button.addEventListener('click', () => { state.level = value; renderDetail(state.jobs.find(j => j.id === state.selected)) })
    filter.append(button)
  }
  toolbar.append(title, spacer, filter)
  if (job.error) {
    toolbar.append(linkButton('复制错误', () => copyText(job.error)))
  }
  toolbar.append(linkButton(state.expanded ? '收起日志' : '展开全部', () => {
    state.expanded = !state.expanded
    renderDetail(state.jobs.find(j => j.id === state.selected))
  }))
  section.append(toolbar)
  const log = document.createElement('pre')
  log.id = 'log'
  log.className = `log${state.expanded ? ' expanded' : ''}`
  log.tabIndex = 0
  section.append(log)
  return section
}

async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text)
    notify('已复制')
  } catch {
    notify('复制失败，请手动选择日志文本', true)
  }
}

const levelMinimum = { all: 0, warn: 2, error: 3 }
const levelOrder = { debug: 0, info: 1, warn: 2, error: 3 }

function visibleLines() {
  const filtered = state.lines.filter(line => (levelOrder[line.level] ?? 1) >= levelMinimum[state.level])
  return state.expanded ? filtered : filtered.slice(-8)
}

function renderLog() {
  const log = $('#log')
  if (!log) return
  log.replaceChildren()
  for (const line of visibleLines()) {
    const row = document.createElement('span')
    row.className = line.level
    const time = document.createElement('span')
    time.className = 'ts'
    time.textContent = new Date(line.at).toLocaleTimeString('zh-CN', { hour12: false })
    row.append(time, document.createTextNode(line.text + '\n'))
    log.append(row)
  }
  log.scrollTop = log.scrollHeight
}

function pushLine(line) {
  if (line.seq <= state.sequence) return
  state.lines.push(line)
  if (state.lines.length > 20000) state.lines.splice(0, 5000)
  state.sequence = Math.max(state.sequence, line.seq)
  renderLog()
}

function startStreaming(id) {
  if (typeof EventSource !== 'function') { startPolling(id); return }
  const source = new EventSource(withToken(`/api/jobs/${id}/events?after=0`))
  state.source = source
  source.addEventListener('log', event => {
    const payload = JSON.parse(event.data)
    if (payload.line) pushLine(payload.line)
  })
  source.addEventListener('state', event => {
    const payload = JSON.parse(event.data)
    applyJob(payload.job)
    if (payload.job.state && ['succeeded', 'failed', 'canceled'].includes(payload.job.state)) {
      stopStreaming()
      refreshJobs()
    }
  })
  source.addEventListener('progress', event => {
    applyJob(JSON.parse(event.data).job)
  })
  source.addEventListener('error', () => {
    if (source.readyState === EventSource.CLOSED) {
      stopStreaming()
      startPolling(id)
    }
  })
}

function startPolling(id) {
  state.fallbackTimer = setInterval(async () => {
    try {
      const result = await api(`/api/jobs/${id}/logs?after=${state.sequence}&limit=500`)
      for (const line of result.lines) pushLine(line)
      const job = await api(`/api/jobs/${id}`)
      applyJob(job)
      if (['succeeded', 'failed', 'canceled'].includes(job.state)) {
        stopStreaming()
        refreshJobs()
      }
    } catch { stopStreaming() }
  }, 1200)
}

// ---------- 环境页 ----------

for (const button of document.querySelectorAll('.rail-item')) {
  button.addEventListener('click', () => {
    for (const item of document.querySelectorAll('.rail-item')) item.classList.toggle('active', item === button)
    const view = button.dataset.view
    $('#view-tasks').hidden = view !== 'tasks'
    $('#view-settings').hidden = view !== 'settings'
    if (view === 'settings') loadDoctor()
  })
}

function formatUpdaterStatus(updater) {
  if (!updater) return state.config?.updaterAvailable ? '已启用' : '未启用'
  if (!updater.enabled) return '当前安装方式不支持自更新（请检查安装目录写权限）'
  const details = [updater.tokenConfigured ? '已内置只读 GitHub 令牌' : 'GitHub 公开访问']
  if (updater.activeProxy) {
    try {
      details.push(`国内加速 ${new URL(updater.activeProxy).host}`)
    } catch {
      details.push(`国内加速 ${updater.activeProxy}`)
    }
  } else if (updater.chinaMirror) {
    details.push('国内加速待命')
  }
  const suffix = details.join(' · ')
  if (updater.availableVersion) return `发现新版本 v${updater.availableVersion}（${suffix}）`
  return `已启用（${suffix}）`
}

function syncUpdaterUI(updaterInfo) {
  const available = Boolean(state.config?.updaterAvailable)
  if ($('#check-updates')) $('#check-updates').hidden = !available
  if ($('#sidebar-update')) $('#sidebar-update').hidden = !available
  const tag = $('#sidebar-update-tag')
  if (tag) {
    if (available && updaterInfo?.availableVersion) {
      tag.textContent = 'v' + updaterInfo.availableVersion
      tag.hidden = false
    } else {
      tag.hidden = true
    }
  }
}

async function triggerUpdateCheck() {
  try {
    await api('/api/check-updates', { method: 'POST' })
    notify('已打开更新检查窗口')
  } catch (error) {
    notify('检查更新失败：' + error.message, true)
  }
}

async function loadDoctor() {
  const container = $('#doctor-body')
  try {
    const report = await api('/api/doctor')
    const sidecar = report.sidecar || {}
    const rows = [
      ['应用版本', report.version],
      ['系统', report.platform],
      ['HTTP/HTTPS 切片服务', report.httpsAddr ? 'http(s)://' + report.httpsAddr : '未启用'],
      ['自动更新', formatUpdaterStatus(report.updater)],
      ['数据目录', report.dataDir],
      ['转换器', sidecar.path || '未找到'],
      ['进度来源', sidecar.jsonEvents ? '结构化事件' : '输出文件估算'],
      ['写入权限', report.dataDirWritable ? '可写' : '不可写'],
      ['暂停 / 继续', sidecar.available && sidecar.pauseResume ? '支持' : '不支持'],
    ]
    syncUpdaterUI(report.updater)
    container.replaceChildren(...rows.map(([key, value]) => {
      const item = document.createElement('div')
      item.className = 'row-item'
      const k = document.createElement('span')
      k.className = 'k'
      k.textContent = key
      const v = document.createElement('span')
      v.className = 'v'
      v.textContent = value
      v.title = value
      item.append(k, v)
      return item
    }))
    const chip = $('#engine-chip')
    chip.textContent = sidecar.available ? '转换器就绪' : '转换器不可用'
    chip.className = `chip ${sidecar.available ? 'ok' : 'bad'}`
    if (!sidecar.available && sidecar.detail) {
      const detail = document.createElement('p')
      detail.className = 'note'
      detail.textContent = sidecar.detail
      container.append(detail)
    }
  } catch (error) {
    container.textContent = error.message
  }
}

// ---------- 通用 ----------

for (const button of document.querySelectorAll('[data-close]')) {
  button.addEventListener('click', () => $(`#${button.dataset.close}`).close())
}
$('#search').addEventListener('input', event => { state.search = event.target.value; renderJobs() })
$('#refresh').addEventListener('click', refreshJobs)

function rememberDefaults(body) {
  localStorage.setItem('osgb-tiles-input', body.input)
  localStorage.setItem('osgb-tiles-output', body.output)
  localStorage.setItem('osgb-tiles-threads', String(body.threads))
  localStorage.setItem('osgb-tiles-geometry', body.geometry)
  localStorage.setItem('osgb-tiles-memoryBudgetMiB', String(body.memoryBudgetMiB))
  localStorage.setItem('osgb-tiles-srs', body.srs)
  localStorage.setItem('osgb-tiles-origin', body.origin)
}

async function bootstrap() {
  for (const key of ['input', 'output', 'threads', 'geometry', 'memoryBudgetMiB', 'srs', 'origin']) {
    const value = localStorage.getItem(`osgb-tiles-${key}`)
    if (value) $(`#${key}`).value = value
  }
  try {
    state.config = await api('/api/config')
    if (state.config.version && $('#app-version')) {
      $('#app-version').textContent = 'v' + state.config.version
    }
    syncUpdaterUI(state.config.updater)
    if (state.config.updaterAvailable) {
      if ($('#check-updates')) $('#check-updates').addEventListener('click', triggerUpdateCheck)
      if ($('#sidebar-update')) $('#sidebar-update').addEventListener('click', triggerUpdateCheck)
      setTimeout(async () => {
        try {
          const latest = await api('/api/config')
          state.config = latest
          syncUpdaterUI(latest.updater)
        } catch {}
      }, 15000)
    }
    const sidecar = state.config.sidecar || {}
    for (const option of $('#geometry').options) {
      if (option.value === 'draco') option.disabled = !sidecar.geometryFormats?.includes('draco')
    }
    for (const option of $('#textureFormat').options) {
      option.disabled = option.value !== 'original' && !sidecar.textureFormats?.includes(option.value)
    }
    $('#rebuildTop').options[1].disabled = !sidecar.rebuildTop
    $('#topSealDistance').disabled = !sidecar.rebuildTop
    if ($('#embedTextures')) $('#embedTextures').options[1].disabled = !sidecar.embedTextures
    $('#generatePreview').disabled = sidecar.available && !sidecar.preview
    if ($('#generatePreview').disabled) $('#generatePreview').checked = false
    if (state.config.pickerAvailable) {
      for (const button of document.querySelectorAll('.pick')) button.hidden = false
    }
  } catch (error) {
    notify('服务不可用：' + error.message, true)
    return
  }
  await refreshJobs()
  // 本机服务开销可忽略：始终轮询，保证外部提交的任务也能及时出现。
  setInterval(refreshJobs, 4000)
  setInterval(() => {
    // 耗时列每秒刷新，不额外请求。
    if (state.jobs.some(job => ['running', 'pausing'].includes(job.state))) renderJobs()
  }, 1000)
}

bootstrap()
