const runtime = window.FAULTLINE_CONFIG || {}
const defaults = { baseUrl: runtime.baseUrl || "http://127.0.0.1:9090", token: "" }
let stored = null
try {
  stored = JSON.parse(localStorage.getItem("faultline.connection") || "null")
} catch {
  localStorage.removeItem("faultline.connection")
}
const connection = { ...defaults, ...stored }
if (runtime.forceBaseUrl) {
  connection.baseUrl = defaults.baseUrl
  connection.token = ""
}
let state = null
let direction = "upstream"
let polling = false
let history = []
let lastSample = null
const drafts = { upstream: null, downstream: null }

const byId = id => document.getElementById(id)
const formatBytes = value => {
  if (value < 1024) return `${Math.round(value)} B`
  if (value < 1048576) return `${(value / 1024).toFixed(1)} KB`
  if (value < 1073741824) return `${(value / 1048576).toFixed(1)} MB`
  return `${(value / 1073741824).toFixed(2)} GB`
}
const formatDuration = value => {
  const seconds = Math.floor(value / 1000)
  const hours = String(Math.floor(seconds / 3600)).padStart(2, "0")
  const minutes = String(Math.floor((seconds % 3600) / 60)).padStart(2, "0")
  return `${hours}:${minutes}:${String(seconds % 60).padStart(2, "0")}`
}
const formatStageDuration = value => value === 0 ? "UNTIL STOP" : value < 1000 ? `${value} MS` : `${value / 1000} SEC`
const headers = extra => ({ ...(connection.token ? { Authorization: `Bearer ${connection.token}` } : {}), ...extra })

function recordSample(metrics, runId) {
  const now = performance.now()
  if (lastSample?.runId !== runId || metrics.upstream_bytes < (lastSample?.upstreamBytes || 0)) {
    history = []
    lastSample = null
  }
  if (lastSample) {
    const elapsedMs = Math.max(1, now - lastSample.time)
    if (elapsedMs <= 120000) {
      const elapsed = elapsedMs / 1000
      history.push({
        time: now,
        upstream: Math.max(0, metrics.upstream_bytes - lastSample.upstreamBytes) / elapsed,
        downstream: Math.max(0, metrics.downstream_bytes - lastSample.downstreamBytes) / elapsed
      })
    }
    history = history.filter(sample => sample.time >= now - 120000)
  }
  lastSample = { time: now, upstreamBytes: metrics.upstream_bytes, downstreamBytes: metrics.downstream_bytes, runId }
}

function chartPath(key, maximum, now) {
  if (!history.length) return ""
  return history.map((sample, index) => {
    const x = Math.max(0, 600 - (now - sample.time) / 120000 * 600)
    const y = 116 - sample[key] / maximum * 108
    return `${index === 0 ? "M" : "L"}${x.toFixed(1)} ${y.toFixed(1)}`
  }).join(" ")
}

function renderHistory() {
  const now = performance.now()
  const latest = history.at(-1) || { upstream: 0, downstream: 0 }
  const maximum = Math.max(1, ...history.flatMap(sample => [sample.upstream, sample.downstream]))
  byId("upstreamRate").textContent = `${formatBytes(latest.upstream)}/s`
  byId("downstreamRate").textContent = `${formatBytes(latest.downstream)}/s`
  byId("upstreamLine").setAttribute("d", chartPath("upstream", maximum, now))
  byId("downstreamLine").setAttribute("d", chartPath("downstream", maximum, now))
}

async function request(path, options = {}) {
  const response = await fetch(`${connection.baseUrl.replace(/\/$/, "")}${path}`, { ...options, headers: headers(options.headers) })
  if (!response.ok) {
    const body = await response.json().catch(() => ({}))
    throw new Error(body.error || `HTTP ${response.status}`)
  }
  return response.status === 204 ? null : response.json()
}

function setConnectionStatus(mode, label) {
  const node = byId("connectionStatus")
  node.className = `connection-status ${mode}`
  node.querySelector("strong").textContent = label
}

function renderPolicy() {
  if (!state) return
  const policy = drafts[direction] || state.scenario[direction]
  byId("latency").value = policy.latency_ms
  byId("jitter").value = policy.jitter_ms
  byId("bandwidth").value = policy.bandwidth_kbps
  renderOutputs()
  const dirty = drafts[direction] !== null
  byId("unsavedBadge").textContent = dirty ? "UNAPPLIED" : "SYNCED"
  byId("unsavedBadge").classList.toggle("dirty", dirty)
}

function renderOutputs() {
  byId("latencyOutput").textContent = `${byId("latency").value} ms`
  byId("jitterOutput").textContent = `${byId("jitter").value} ms`
  const bandwidth = Number(byId("bandwidth").value)
  byId("bandwidthOutput").textContent = bandwidth === 0 ? "Unlimited" : `${bandwidth.toLocaleString()} kbps`
}

function renderStages(stages, connections) {
  byId("stageCount").textContent = `${stages.length} ${stages.length === 1 ? "STAGE" : "STAGES"}`
  if (!stages.length) {
    byId("stageList").innerHTML = '<div class="empty-state"><span>⌁</span><p>Static policy · no stage sequence</p></div>'
    return
  }
  byId("stageList").innerHTML = stages.map((stage, index) => {
    const active = connections.filter(connection => connection.state === "active" && connection.stage_index === index)
    const progress = active.map(connection => Math.min(100, connection.stage_elapsed_ms / stage.duration_ms * 100))
    const progressStart = stage.duration_ms === 0 || !active.length ? 0 : Math.min(...progress)
    const progressEnd = stage.duration_ms === 0 || !active.length ? 0 : Math.max(...progress)
    const progressWidth = Math.min(100 - progressStart, Math.max(2, progressEnd - progressStart))
    const progressLabel = progressStart === progressEnd
      ? `${progressEnd.toFixed(0)}%`
      : `${progressStart.toFixed(0)}-${progressEnd.toFixed(0)}%`
    return `
    <div class="stage ${active.length ? "active-stage" : ""}">
      <span class="stage-index">${index + 1}</span>
      <div><h3>${escapeHtml(stage.name)}${active.length ? `<em>${active.length} ACTIVE · ${progressLabel}</em>` : ""}</h3><p>↑ ${stage.upstream.latency_ms}ms · ${stage.upstream.bandwidth_kbps || "∞"}kbps<br>↓ ${stage.downstream.latency_ms}ms · ${stage.downstream.bandwidth_kbps || "∞"}kbps · reset ${(stage.reset_probability * 100).toFixed(1)}%</p>${active.length && stage.duration_ms !== 0 ? `<span class="stage-progress"><i style="left:${progressStart.toFixed(1)}%;width:${progressWidth.toFixed(1)}%"></i></span>` : ""}</div>
      <span class="stage-duration">${formatStageDuration(stage.duration_ms)}</span>
    </div>`
  }).join("")
}

function escapeHtml(value) {
  const node = document.createElement("span")
  node.textContent = value
  return node.innerHTML
}

function render(next) {
  state = next
  const { scenario, metrics, lifecycle } = next
  recordSample(metrics, lifecycle.run_id)
  byId("scenarioName").innerHTML = `${escapeHtml(scenario.name)}<span>.</span>`
  byId("experimentId").textContent = lifecycle.experiment_id || "unassigned"
  byId("runId").textContent = lifecycle.run_id || "-"
  byId("uptime").textContent = formatDuration(lifecycle.uptime_ms)
  byId("activeMetric").textContent = metrics.active_connections.toLocaleString()
  byId("acceptedMetric").textContent = metrics.accepted_connections.toLocaleString()
  byId("completedMetric").textContent = metrics.completed_connections.toLocaleString()
  byId("disruptedMetric").textContent = (metrics.reset_connections + metrics.timed_out_connections).toLocaleString()
  byId("bytesMetric").textContent = formatBytes(metrics.upstream_bytes + metrics.downstream_bytes)
  byId("transitionsMetric").textContent = metrics.stage_transitions.toLocaleString()
  byId("listenEndpoint").textContent = scenario.proxy.listen
  byId("upstreamEndpoint").textContent = scenario.proxy.upstream
  renderStages(scenario.stages, next.connections || [])
  renderHistory()
  renderPolicy()
  setConnectionStatus("online", lifecycle.status.toUpperCase())
}

async function poll() {
  if (polling) return
  polling = true
  try {
    render(await request("/v1/state"))
  } catch (error) {
    setConnectionStatus("offline", "OFFLINE")
    byId("formMessage").textContent = `Control API unavailable: ${error.message}`
    byId("formMessage").className = "form-message error"
  } finally {
    polling = false
  }
}

document.querySelectorAll(".tab").forEach(tab => tab.addEventListener("click", () => {
  direction = tab.dataset.direction
  document.querySelectorAll(".tab").forEach(item => item.classList.toggle("active", item === tab))
  renderPolicy()
}))

document.querySelectorAll('input[type="range"]').forEach(input => input.addEventListener("input", () => {
  renderOutputs()
  drafts[direction] = {
    latency_ms: Number(byId("latency").value),
    jitter_ms: Number(byId("jitter").value),
    bandwidth_kbps: Number(byId("bandwidth").value)
  }
  renderPolicy()
}))

byId("policyForm").addEventListener("submit", async event => {
  event.preventDefault()
  const submittedDirection = direction
  const button = event.currentTarget.querySelector("button")
  const query = new URLSearchParams(new FormData(event.currentTarget))
  button.disabled = true
  try {
    await request(`/v1/policies/${submittedDirection}?${query}`, { method: "PUT", headers: { "X-Faultline-Confirm": "update" } })
    drafts[submittedDirection] = null
    byId("formMessage").textContent = `${submittedDirection} policy applied`
    byId("formMessage").className = "form-message"
    if (direction === submittedDirection) renderPolicy()
    await poll()
  } catch (error) {
    byId("formMessage").textContent = error.message
    byId("formMessage").className = "form-message error"
  } finally {
    button.disabled = false
  }
})

byId("settingsButton").addEventListener("click", () => {
  byId("baseUrl").value = connection.baseUrl
  byId("authToken").value = connection.token
  byId("settingsDialog").showModal()
})

byId("settingsForm").addEventListener("submit", event => {
  event.preventDefault()
  connection.baseUrl = byId("baseUrl").value.replace(/\/$/, "")
  connection.token = byId("authToken").value
  localStorage.setItem("faultline.connection", JSON.stringify(connection))
  byId("settingsDialog").close()
  poll()
})

byId("shutdownButton").addEventListener("click", async () => {
  if (!window.confirm("Stop the active Faultline run? Existing connections will close.")) return
  try {
    await request("/v1/shutdown", { method: "POST", headers: { "X-Faultline-Confirm": "shutdown" } })
    setConnectionStatus("", "STOPPING")
  } catch (error) {
    window.alert(`Shutdown failed: ${error.message}`)
  }
})

byId("baseUrl").value = connection.baseUrl
byId("authToken").value = connection.token
poll()
setInterval(poll, 2000)
