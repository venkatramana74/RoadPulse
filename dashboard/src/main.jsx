import React, { useEffect, useMemo, useRef, useState } from "react";
import { createRoot } from "react-dom/client";
import {
  Activity, AlertTriangle, Bell, CalendarDays, ChevronDown, ChevronLeft,
  ChevronRight, CircuitBoard, Download, FileJson, FileText, Gauge,
  LayoutDashboard, MapPin, Moon, Network, RadioTower, RefreshCw,
  Search, Settings, ShieldCheck, Siren, Sun
} from "lucide-react";
import "./styles.css";

const MAPPLS_API_KEY = "vymebdrjjzsvkivyzabaqlxzalxqsmmkcslb";
const DEFAULT_API = "https://09xvgayg27.execute-api.ap-south-1.amazonaws.com";
const PAGE_SIZE = 100;

const sampleEvents = [
  { event_id: 1248, timestamp_ms: 1748356241000, road: "Marathahalli Road", severity: "high", peak_g: 3.78, x_g: .51, y_g: .22, z_g: .86, magnitude_g: 1.05, device_id: "UNIT-02", lat: 17.3095, lng: 78.59177, status: "New" },
  { event_id: 1247, timestamp_ms: 1748356238000, road: "Indiranagar Road", severity: "medium", peak_g: 2.91, x_g: .41, y_g: .18, z_g: .45, magnitude_g: .68, device_id: "UNIT-03", lat: 17.421, lng: 78.443, status: "New" },
  { event_id: 1246, timestamp_ms: 1748356235000, road: "Whitefield Road", severity: "low", peak_g: 1.67, x_g: .25, y_g: .12, z_g: .31, magnitude_g: .43, device_id: "UNIT-01", lat: 17.449, lng: 78.568, status: "New" },
  { event_id: 1245, timestamp_ms: 1748356232000, road: "Koramangala 4th Block", severity: "high", peak_g: 4.21, x_g: .62, y_g: .34, z_g: .95, magnitude_g: 1.27, device_id: "UNIT-04", lat: 17.392, lng: 78.443, status: "Alert" },
  { event_id: 1244, timestamp_ms: 1748356229000, road: "Kadubeesanahalli", severity: "medium", peak_g: 2.45, x_g: .33, y_g: .16, z_g: .44, magnitude_g: .61, device_id: "UNIT-02", lat: 17.433, lng: 78.665, status: "New" }
];

function normalizeTimestamp(value) {
  const raw = Number(value || 0);
  if (!raw) return Date.now();
  return raw > 946684800 && raw < 100000000000 ? raw * 1000 : raw;
}

function normalizeEvent(item, index = 0) {
  const severity = String(item.severity || "low").toLowerCase();
  const timestamp_ms = normalizeTimestamp(item.timestamp_ms ?? item.timestamp ?? item.created_at_ms);
  const rawEventId = item.event_id ?? item.id ?? item.eventId;
  const fallbackEventId = `EV-${String(timestamp_ms).slice(-8)}-${String(index + 1).padStart(2, "0")}`;
  const x_g = Number(item.x_g ?? item.x ?? 0);
  const y_g = Number(item.y_g ?? item.y ?? 0);
  const z_g = Number(item.z_g ?? item.z ?? 0);
  const vectorG = Math.sqrt((x_g * x_g) + (y_g * y_g) + (z_g * z_g));
  const magnitude_g = Number(item.magnitude_g ?? item.magnitude ?? vectorG);
  const reportedPeak = Number(item.peak_g ?? item.peakG ?? item.g_force ?? 0);
  return {
    event_id: rawEventId === undefined || rawEventId === null || /^event_\d{2}$/i.test(String(rawEventId)) ? fallbackEventId : rawEventId,
    timestamp_ms,
    road: item.road || item.location || item.road_name || "Unknown Road",
    severity: ["low", "medium", "high"].includes(severity) ? severity : "low",
    peak_g: Math.max(reportedPeak, magnitude_g, vectorG),
    x_g,
    y_g,
    z_g,
    magnitude_g,
    device_id: item.device_id || item.device || item.deviceId || "ESP32",
    lat: Number(item.lat ?? item.latitude ?? 0),
    lng: Number(item.lng ?? item.longitude ?? 0),
    status: item.status || (["medium", "high"].includes(severity) ? "Alert" : "New")
  };
}

function unwrapEvents(data) {
  const body = typeof data?.body === "string" ? JSON.parse(data.body) : data?.body;
  const source = body || data;
  if (Array.isArray(source)) return source;
  return source?.events || source?.items || source?.detections || source?.data || [];
}

function normalizeEvents(data) {
  const list = unwrapEvents(data);
  if (!Array.isArray(list) || list.length === 0) return [];
  return list.map(normalizeEvent).sort((a, b) => b.timestamp_ms - a.timestamp_ms);
}

function summarize(events) {
  return events.reduce((acc, event) => {
    acc.total += 1;
    acc[event.severity] += 1;
    acc.peak = Math.max(acc.peak, event.peak_g || 0);
    return acc;
  }, { total: 0, low: 0, medium: 0, high: 0, peak: 0 });
}

function normalizeSummary(data, events) {
  const body = typeof data?.body === "string" ? JSON.parse(data.body) : data?.body;
  const source = body || data || {};
  const fallback = summarize(events);
  return {
    total: Number(source.total ?? source.count ?? fallback.total),
    low: Number(source.low ?? source.severity_counts?.low ?? fallback.low),
    medium: Number(source.medium ?? source.severity_counts?.medium ?? fallback.medium),
    high: Number(source.high ?? source.severity_counts?.high ?? fallback.high),
    peak: Number(source.peak ?? source.peak_g ?? fallback.peak)
  };
}

function formatTime(timestampMs) {
  if (!timestampMs) return "No timestamp";
  if (timestampMs < 946684800000) return `Uptime ${Math.round(timestampMs / 1000)}s`;
  return new Date(timestampMs).toLocaleString("en-IN", { dateStyle: "medium", timeStyle: "medium" });
}

function endpoint(base, path, limit) {
  const url = new URL(`${base.replace(/\/+$/, "")}${path}`);
  if (limit === "all") {
    url.searchParams.set("limit", "10000");
    url.searchParams.set("all", "true");
  } else if (limit) {
    url.searchParams.set("limit", limit);
  }
  return url.toString();
}

async function fetchJson(url, apiKey) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 9000);
  try {
    const response = await fetch(url, {
      cache: "no-store",
      headers: { accept: "application/json", ...(apiKey ? { "x-api-key": apiKey } : {}) },
      signal: controller.signal
    });
    const text = await response.text();
    if (!response.ok) throw new Error(`${response.status} ${response.statusText}: ${text.slice(0, 140)}`);
    return text ? JSON.parse(text) : {};
  } finally {
    clearTimeout(timeout);
  }
}

function useLiveEvents(apiUrl, apiKey, limit, refreshMs) {
  const [events, setEvents] = useState(sampleEvents);
  const [summary, setSummary] = useState(() => summarize(sampleEvents));
  const [apiState, setApiState] = useState("sample");
  const [error, setError] = useState("");
  const [lastUpdated, setLastUpdated] = useState("");
  const [retries, setRetries] = useState(0);

  useEffect(() => {
    let alive = true;
    let timer;

    async function load() {
      if (!apiUrl.trim()) return;
      setApiState((current) => current === "live" ? "refreshing" : "connecting");
      try {
        const [eventsResult, summaryResult] = await Promise.allSettled([
          fetchJson(endpoint(apiUrl, "/events", limit), apiKey),
          fetchJson(endpoint(apiUrl, "/summary"), apiKey)
        ]);
        if (eventsResult.status === "rejected") throw eventsResult.reason;
        const data = eventsResult.value;
        const nextEvents = normalizeEvents(data);
        if (!alive) return;
        setEvents(nextEvents.length ? nextEvents : []);
        setSummary(summaryResult.status === "fulfilled" ? normalizeSummary(summaryResult.value, nextEvents) : summarize(nextEvents));
        setApiState("live");
        setError("");
        setRetries(0);
        setLastUpdated(new Date().toLocaleTimeString("en-IN"));
      } catch (err) {
        if (!alive) return;
        setApiState("error");
        setError(err.message);
        setRetries((value) => value + 1);
      }
    }

    load();
    timer = setInterval(load, refreshMs);
    return () => {
      alive = false;
      clearInterval(timer);
    };
  }, [apiUrl, apiKey, limit, refreshMs]);

  return { events, summary, apiState, error, lastUpdated, retries };
}

function App() {
  const apiUrl = DEFAULT_API;
  const apiKey = "";
  const [limit, setLimit] = useState("all");
  const [severity, setSeverity] = useState("all");
  const [timeRange, setTimeRange] = useState("all");
  const [search, setSearch] = useState("");
  const [page, setPage] = useState(1);
  const [themeMode, setThemeMode] = useState(() => localStorage.getItem("roadpulseThemeMode") || "dark");
  const [refreshMs, setRefreshMs] = useState(() => Number(localStorage.getItem("roadpulseRefreshMs") || 3000));
  const [activeView, setActiveView] = useState("dashboard");
  const [mapFocusEvent, setMapFocusEvent] = useState(null);
  const [verifiedAlerts, setVerifiedAlerts] = useState(() => new Set(JSON.parse(localStorage.getItem("verifiedAlerts") || "[]")));
  const { events, summary, apiState, error, lastUpdated, retries } = useLiveEvents(apiUrl, apiKey, limit, refreshMs);

  useEffect(() => {
    const applyTheme = () => {
      const systemDark = window.matchMedia?.("(prefers-color-scheme: dark)")?.matches;
      document.documentElement.dataset.theme = themeMode === "system" ? (systemDark ? "dark" : "light") : themeMode;
    };
    applyTheme();
    localStorage.setItem("roadpulseThemeMode", themeMode);
    const media = window.matchMedia?.("(prefers-color-scheme: dark)");
    media?.addEventListener?.("change", applyTheme);
    return () => media?.removeEventListener?.("change", applyTheme);
  }, [themeMode]);

  useEffect(() => {
    localStorage.setItem("roadpulseRefreshMs", String(refreshMs));
  }, [refreshMs]);

  useEffect(() => {
    localStorage.setItem("verifiedAlerts", JSON.stringify([...verifiedAlerts]));
  }, [verifiedAlerts]);

  const filtered = useMemo(() => {
    const min = rangeStart(timeRange);
    return events.filter((event) => {
      const severityOk = severity === "all" || event.severity === severity;
      const timeOk = !min || event.timestamp_ms >= min;
      const searchOk = !search.trim() || String(event.event_id).toLowerCase().includes(search.trim().toLowerCase());
      return severityOk && timeOk && searchOk;
    });
  }, [events, severity, timeRange, search]);

  useEffect(() => setPage(1), [severity, timeRange, search, limit]);

  const filteredMetrics = useMemo(() => summarize(filtered), [filtered]);
  const unfiltered = severity === "all" && timeRange === "all" && !search.trim();
  const metrics = unfiltered ? { ...summary, peak: Math.max(summary.peak || 0, filteredMetrics.peak) } : filteredMetrics;
  const latest = filtered[0];

  return (
    <div className="app-shell">
      <Sidebar apiState={apiState} activeView={activeView} onNavigate={setActiveView} alertCount={metrics.medium + metrics.high} refreshMs={refreshMs} />
      <main className="dashboard">
        <Header apiState={apiState} lastUpdated={lastUpdated} themeMode={themeMode} />
        {activeView !== "settings" && <Toolbar
            limit={limit}
            severity={severity}
            timeRange={timeRange}
            search={search}
            setLimit={setLimit}
            setSeverity={setSeverity}
            setTimeRange={setTimeRange}
            setSearch={setSearch}
            onCsv={() => exportCsv(filtered)}
            onJson={() => downloadJson(filtered)}
          />}
        {activeView !== "settings" && <LiveStrip apiState={apiState} error={error} retries={retries} latest={latest} refreshMs={refreshMs} />}
        {activeView !== "settings" && <MetricGrid metrics={metrics} filtered={filtered} limit={limit} latest={latest} />}
        <ActiveView
          view={activeView}
          events={filtered}
          metrics={metrics}
          page={page}
          setPage={setPage}
          setActiveView={setActiveView}
          setMapFocusEvent={setMapFocusEvent}
          mapFocusEvent={mapFocusEvent}
          verifiedAlerts={verifiedAlerts}
          onVerifyAlert={(event) => setVerifiedAlerts((items) => new Set(items).add(alertKey(event)))}
          refreshMs={refreshMs}
          setRefreshMs={setRefreshMs}
          themeMode={themeMode}
          setThemeMode={setThemeMode}
        />
      </main>
    </div>
  );
}

function MetricGrid({ metrics, filtered, limit, latest }) {
  return <section className="metric-grid">
    <Metric tone="purple" icon={<CircuitBoard />} title="Total Detections" value={metrics.total.toLocaleString()} sub={`${filtered.length.toLocaleString()} rows loaded${limit === "all" ? " (unlimited mode)" : ""}`} />
    <Metric tone="green" icon={<Network />} title="Low Severity" value={metrics.low.toLocaleString()} sub={`${percent(metrics.low, metrics.total)}% of visible`} />
    <Metric tone="orange" icon={<Siren />} title="Medium Severity" value={metrics.medium.toLocaleString()} sub={`${percent(metrics.medium, metrics.total)}% of visible`} />
    <Metric tone="red" icon={<AlertTriangle />} title="High Severity" value={metrics.high.toLocaleString()} sub={`${percent(metrics.high, metrics.total)}% of visible`} />
    <Metric tone="cyan" icon={<Activity />} title="Peak G-Force" value={`${metrics.peak.toFixed(2)} G`} sub={latest ? `Latest: ${formatTime(latest.timestamp_ms)}` : "No events"} />
  </section>;
}

function ActiveView({ view, events, metrics, page, setPage, setActiveView, setMapFocusEvent, mapFocusEvent, verifiedAlerts, onVerifyAlert, refreshMs, setRefreshMs, themeMode, setThemeMode }) {
  const showOnMap = (event) => {
    setMapFocusEvent(event);
    setActiveView("road-network");
  };

  if (view === "road-network") return <section className="view-grid road-view map-only"><RoadMap events={events} focusEvent={mapFocusEvent} /><RightRail events={events} onShowMap={showOnMap} verifiedAlerts={verifiedAlerts} onVerifyAlert={onVerifyAlert} /></section>;
  if (view === "detections") return <section className="view-grid single"><Recent events={events} page={page} setPage={setPage} onShowMap={showOnMap} /></section>;
  if (view === "reports") return <ReportsView events={events} metrics={metrics} />;
  if (view === "alerts") return <AlertsView events={events} onShowMap={showOnMap} verifiedAlerts={verifiedAlerts} onVerifyAlert={onVerifyAlert} />;
  if (view === "analytics") return <AnalyticsView events={events} metrics={metrics} />;
  if (view === "devices") return <DevicesView events={events} />;
  if (view === "settings") return <SettingsView refreshMs={refreshMs} setRefreshMs={setRefreshMs} themeMode={themeMode} setThemeMode={setThemeMode} setActiveView={setActiveView} />;
  return <section className="content-grid">
    <RoadMap events={events} focusEvent={mapFocusEvent} />
    <SeverityPanel metrics={metrics} />
    <RightRail events={events} onShowMap={showOnMap} verifiedAlerts={verifiedAlerts} onVerifyAlert={onVerifyAlert} />
  </section>;
}

function LiveStrip({ apiState, error, retries, latest, refreshMs }) {
  return <div className={`live-strip ${apiState}`}>
    <span><i />{apiState === "live" || apiState === "refreshing" ? `Live polling every ${Math.round(refreshMs / 1000)}s` : apiState === "error" ? "API connection failed" : "Connecting to API"}</span>
    <span>Retries: {retries}</span>
    <span>Last event: {latest ? formatTime(latest.timestamp_ms) : "none"}</span>
    {error && <b>{error}</b>}
  </div>;
}

function Sidebar({ apiState, activeView, onNavigate, alertCount, refreshMs }) {
  const items = [
    [LayoutDashboard, "Dashboard", "dashboard"],
    [Network, "Road Network", "road-network"],
    [MapPin, "Detections", "detections"],
    [FileText, "Reports", "reports"],
    [Bell, "Alerts", "alerts", alertCount],
    [Activity, "Analytics", "analytics"],
    [CircuitBoard, "Devices", "devices"],
    [Settings, "Settings", "settings"]
  ];
  return <aside className="sidebar">
    <div className="brand"><div className="logo-road" /><div><h1>RoadPulse</h1><p>Smart Pothole Monitoring</p></div></div>
    <nav>{items.map(([Icon, label, id, badge]) => <button onClick={() => onNavigate(id)} className={activeView === id ? "active" : ""} key={label}><Icon size={19}/><span>{label}</span>{Number(badge) > 0 && <b>{Math.min(Number(badge), 99)}</b>}</button>)}</nav>
    <div className="road-visual"><div className="moon" /><div className="road-line" /></div>
    <div className="health">
      <div className="health-title"><Activity size={19}/><span>System Health<br/><strong>{apiState === "error" ? "API Needs Attention" : "Operational"}</strong></span></div>
      <p><ShieldCheck size={18}/> <span>Polling<br/><b>{Math.round(refreshMs / 1000)} seconds</b></span></p>
    </div>
  </aside>;
}

function Header({ apiState, lastUpdated, themeMode }) {
  return <header className="topbar">
    <div><h2>RoadPulse Live Dashboard</h2></div>
    <div className="top-actions"><span>{lastUpdated ? `Updated ${lastUpdated}` : "Waiting for data"}</span><span className="accuracy-note">accurately to 10-20 meters</span><div className="avatar">AD<i /></div></div>
  </header>;
}

function Toolbar({ limit, severity, timeRange, search, setLimit, setSeverity, setTimeRange, setSearch, onCsv, onJson }) {
  return <div className="toolbar controls">
    <Control icon={<CalendarDays size={17}/>} value={limit} onChange={setLimit} options={[["100","100 events"],["500","500 events"],["1000","1000 events"],["all","Unlimited"]]} />
    <Control value={severity} onChange={setSeverity} options={[["all","All Severity"],["low","Low"],["medium","Medium"],["high","High"]]} />
    <Control value={timeRange} onChange={setTimeRange} options={[["all","All Time"],["15m","15 min"],["1h","1 hour"],["6h","6 hours"],["24h","24 hours"],["7d","7 days"]]} />
    <label className="event-search"><Search size={17}/><input value={search} onChange={(e) => setSearch(e.target.value)} placeholder="Search event ID" /></label>
    <button className="export" onClick={onCsv}><Download size={17}/>CSV</button>
  </div>;
}

function Control({ icon, value, onChange, options }) {
  return <label className="select-control">{icon}<select value={value} onChange={(e) => onChange(e.target.value)}>{options.map(([v, label]) => <option value={v} key={v}>{label}</option>)}</select><ChevronDown size={17}/></label>;
}

function Metric({ tone, icon, title, value, sub }) {
  return <article className={`metric ${tone}`}><div className="metric-head"><span>{icon}</span><p>{title}</p></div><h3>{value}</h3><small>{sub}</small><Spark /></article>;
}

function Spark() {
  return <svg viewBox="0 0 170 88"><polyline points="72,78 86,68 92,72 99,50 106,62 113,43 121,55 128,48 136,36 144,42 151,29 159,48" /></svg>;
}

function RoadMap({ events, focusEvent }) {
  const [clusterView, setClusterView] = useState(true);
  return <section className="panel map-panel"><div className="panel-title"><h3>ROAD HEALTH MAP</h3><div><button className="pill"><RefreshCw size={14}/>Mappls Live</button><button className={clusterView ? "active" : ""} onClick={() => setClusterView((value) => !value)}>Cluster View</button></div></div><div className="map map-live">
    <MapplsLiveMap events={events} focusEvent={focusEvent} clusterView={clusterView} />
    <div className="legend"><p><i className="good"/>Low</p><p><i className="medium"/>Medium</p><p><i className="severe"/>High</p></div>
  </div></section>;
}

function MapplsLiveMap({ events, focusEvent, clusterView }) {
  const mapNode = useRef(null);
  const mapRef = useRef(null);
  const markersRef = useRef([]);
  const lastFocusKeyRef = useRef("");
  const [failed, setFailed] = useState(false);
  const validEvents = useMemo(() => mergeFocusEvent(events, focusEvent).filter(isValidCoordinate), [events, focusEvent]);

  useEffect(() => {
    let cancelled = false;
    setFailed(false);
    loadMapplsSdk().then(() => {
      if (cancelled || !mapNode.current) return;
      const api = getMapplsApi();
      if (!api?.Map) {
        setFailed(true);
        return;
      }

      try {
        mapNode.current.id = mapNode.current.id || "roadpulse-mappls-canvas";
        const center = getMapCenter(validEvents);
        mapRef.current = new api.Map(mapNode.current.id, {
          center: { lat: center.lat, lng: center.lng },
          zoom: focusEvent ? 16 : 14,
          zoomControl: true,
          traffic: false,
          geolocation: false
        });
        window.setTimeout(() => mapRef.current?.resize?.(), 300);
      } catch {
        setFailed(true);
      }
    }).catch(() => setFailed(true));

    return () => {
      cancelled = true;
      clearMarkers(markersRef);
      mapRef.current = null;
    };
  }, []);

  useEffect(() => {
    const api = getMapplsApi();
    if (!api || !mapRef.current) return;
    clearMarkers(markersRef);

    validEvents.forEach((event, index) => {
      try {
        const selected = sameEvent(event, focusEvent);
        const position = clusterView ? displayCoordinate(event, index, validEvents) : { lat: event.lat, lng: event.lng };
        const marker = new api.Marker({
          map: mapRef.current,
          position: { lat: position.lat, lng: position.lng },
          html: `<div class="mappls-marker ${event.severity} ${selected ? "selected" : ""}">${event.severity[0].toUpperCase()}</div>`,
          popupHtml: `<strong>${cap(event.severity)} pothole</strong><br>${coordinateText(event)}<br>${event.peak_g.toFixed(2)} G`
        });
        markersRef.current.push(marker);
      } catch {
        setFailed(true);
      }
    });

    const focusKey = focusEvent && isValidCoordinate(focusEvent) ? alertKey(focusEvent) : "";
    if (focusKey && focusKey !== lastFocusKeyRef.current) {
      try {
        mapRef.current.setCenter?.({ lat: focusEvent.lat, lng: focusEvent.lng });
        mapRef.current.setZoom?.(16);
        lastFocusKeyRef.current = focusKey;
      } catch {}
    }
  }, [validEvents, focusEvent, clusterView]);

  return <>
    <div ref={mapNode} className="mappls-canvas" />
    {failed && <FallbackMap events={events} focusEvent={focusEvent} clusterView={clusterView} />}
    {failed && <div className="map-status">Mappls unavailable. Showing built-in fallback.</div>}
    {focusEvent && <div className="map-focus-card">
      <b>#{focusEvent.event_id}</b>
      <span>{cap(focusEvent.severity)} severity</span>
      <small>{coordinateText(focusEvent)}</small>
    </div>}
    <div className="map-count">{validEvents.length} plotted</div>
  </>;
}

function clearMarkers(markersRef) {
  markersRef.current.forEach((marker) => {
    try { marker.remove?.(); } catch {}
  });
  markersRef.current = [];
}

function loadMapplsSdk() {
  if (getMapplsApi()) return Promise.resolve();
  if (window.__mapplsLoading && window.__mapplsLoadingStatus !== "failed") return window.__mapplsLoading;
  const urls = [
    `https://sdk.mappls.com/map/sdk/web?v=3.0&access_token=${MAPPLS_API_KEY}`,
    `https://apis.mappls.com/advancedmaps/api/${MAPPLS_API_KEY}/map_sdk?layer=vector&v=3.0`,
    `https://apis.mappls.com/advancedmaps/api/${MAPPLS_API_KEY}/map_sdk?layer=vector&v=2.0`,
    `https://apis.mapmyindia.com/advancedmaps/v1/${MAPPLS_API_KEY}/map_load?v=1.5`
  ];
  window.__mapplsLoadingStatus = "loading";
  window.__mapplsLoading = urls.reduce((chain, url) => chain.catch(() => loadScript(url)), Promise.reject())
    .then(() => {
      if (!getMapplsApi()) throw new Error("Mappls SDK unavailable");
      window.__mapplsLoadingStatus = "ready";
    })
    .catch((error) => {
      window.__mapplsLoadingStatus = "failed";
      throw error;
    });
  return window.__mapplsLoading;
}

function loadScript(url) {
  return new Promise((resolve, reject) => {
    const existing = [...document.scripts].find((script) => script.src === url);
    if (existing && getMapplsApi()) return resolve();
    if (existing && !getMapplsApi()) existing.remove();
    const script = document.createElement("script");
    script.src = url;
    script.async = true;
    script.onload = () => setTimeout(() => getMapplsApi() ? resolve() : reject(new Error("Map API missing")), 120);
    script.onerror = reject;
    document.head.appendChild(script);
  });
}

function getMapplsApi() { return window.mappls || window.Mappls || window.MapmyIndia || null; }
function getMapCenter(events) {
  const first = events.find((event) => isValidCoordinate(event));
  return first ? { lat: first.lat, lng: first.lng } : { lat: 17.3095, lng: 78.59177 };
}

function isValidCoordinate(event) {
  return Number.isFinite(event.lat) && Number.isFinite(event.lng) && Math.abs(event.lat) > 0.001 && Math.abs(event.lng) > 0.001;
}

function displayCoordinate(event, index, events) {
  const lats = events.map((item) => item.lat);
  const lngs = events.map((item) => item.lng);
  const tightCluster = (Math.max(...lats) - Math.min(...lats) < 0.0008) && (Math.max(...lngs) - Math.min(...lngs) < 0.0008);
  if (!tightCluster) return { lat: event.lat, lng: event.lng };

  // Display-only spread keeps overlapping GPS samples visible on the map.
  const pointsPerRing = 10;
  const ring = (index % pointsPerRing) / pointsPerRing * Math.PI * 2;
  const radius = 0.00028 + Math.floor(index / pointsPerRing) * 0.00018;
  return {
    lat: event.lat + Math.sin(ring) * radius,
    lng: event.lng + Math.cos(ring) * radius
  };
}

function FallbackMap({ events, zoom = 1, focusEvent, clusterView = true }) {
  const markerEvents = mergeFocusEvent(events, focusEvent);
  const positions = mapMarkerPositions(markerEvents, clusterView);
  return <div className="fallback-map">
    <div className="map-layer" style={{ transform: `scale(${zoom})` }}>
    <svg viewBox="0 0 720 470" preserveAspectRatio="none">
      <path className="r good" d="M0 175 C120 160 132 75 230 70 S360 85 430 25 S570 40 720 0"/>
      <path className="r medium" d="M352 0 C365 95 405 160 520 185 S640 280 700 470"/>
      <path className="r severe" d="M92 470 C150 390 173 330 215 280 S275 230 340 235 S410 270 505 260"/>
      <path className="r poor" d="M540 0 C510 80 505 145 535 205 S642 280 720 300"/>
      <path className="r good" d="M290 470 C325 410 332 330 325 250 S310 115 318 0"/>
    </svg>
    <div className="map-label north">Munganoor</div>
    <div className="map-label east">Outer Ring</div>
    <div className="map-label south">Nagarjuna Sagar Road</div>
    {markerEvents.map((event, index) => {
      const selected = sameEvent(event, focusEvent);
      return <MapMarker key={`${event.event_id}-${index}`} className={`${tone(event.severity)} small ${selected ? "selected" : ""}`} x={positions[index].x} y={positions[index].y} label={event.severity[0].toUpperCase()}/>;
    })}
    </div>
  </div>;
}

function MapMarker({ className, x, y, label }) { return <div className={`marker ${className}`} style={{ left: x, top: y }}>{label}</div>; }

function mapMarkerPositions(events, clusterView = true) {
  const fallback = [["43%","18%"],["53%","45%"],["19%","65%"],["51%","80%"],["84%","74%"],["32%","32%"],["68%","58%"],["74%","30%"],["28%","48%"],["60%","68%"]];
  const valid = events.filter(isValidCoordinate);
  if (!valid.length) return events.map((_, index) => ({ x: fallback[index % fallback.length][0], y: fallback[index % fallback.length][1] }));

  const display = events.map((event, index) => isValidCoordinate(event) ? (clusterView ? displayCoordinate(event, index, valid) : { lat: event.lat, lng: event.lng }) : null);
  const usable = display.filter(Boolean);
  const latMin = Math.min(...usable.map((event) => event.lat));
  const latMax = Math.max(...usable.map((event) => event.lat));
  const lngMin = Math.min(...usable.map((event) => event.lng));
  const lngMax = Math.max(...usable.map((event) => event.lng));
  const latSpan = Math.max(latMax - latMin, 0.0001);
  const lngSpan = Math.max(lngMax - lngMin, 0.0001);

  return events.map((event, index) => {
    const coord = display[index];
    if (!coord) return { x: fallback[index % fallback.length][0], y: fallback[index % fallback.length][1] };
    const x = 12 + ((coord.lng - lngMin) / lngSpan) * 76;
    const y = 12 + (1 - ((coord.lat - latMin) / latSpan)) * 76;
    return { x: `${x.toFixed(1)}%`, y: `${y.toFixed(1)}%` };
  });
}

function SeverityPanel({ metrics }) {
  const low = percent(metrics.low, metrics.total);
  const medium = percent(metrics.medium, metrics.total);
  return <section className="panel severity"><h3>SEVERITY OVERVIEW</h3><div className="donut" style={{ background: `conic-gradient(#14c96c 0 ${low}%, #ff9d16 ${low}% ${low + medium}%, #ff3e3e ${low + medium}% 100%)` }}><div><b>{metrics.total.toLocaleString()}</b><span>Total</span></div></div><div className="sev-list"><p><i className="low"/>Low<b>{low}%</b></p><p><i className="medium"/>Medium<b>{medium}%</b></p><p><i className="high"/>High<b>{percent(metrics.high, metrics.total)}%</b></p></div></section>;
}

function Recent({ events, page, setPage, onShowMap }) {
  const pages = Math.max(1, Math.ceil(events.length / PAGE_SIZE));
  const rows = events.slice((page - 1) * PAGE_SIZE, page * PAGE_SIZE);
  return <section className="panel recent"><h3>RECENT DETECTIONS</h3><div className="table-wrap"><table><thead><tr><th>ID</th><th>Time</th><th>Road / Location</th><th>Severity</th><th>Peak G</th><th>X</th><th>Y</th><th>Z</th><th>Magnitude</th><th>Device</th><th>Status</th><th>Map</th></tr></thead><tbody>{rows.map((e, i)=><tr key={`${e.event_id}-${i}`}><td>#{e.event_id}</td><td>{formatTime(e.timestamp_ms)}</td><td>{e.road}</td><td><span className={`tag ${e.severity}`}>{cap(e.severity)}</span></td><td>{e.peak_g.toFixed(2)} G</td><td>{e.x_g.toFixed(3)}</td><td>{e.y_g.toFixed(3)}</td><td>{e.z_g.toFixed(3)}</td><td>{e.magnitude_g.toFixed(3)}</td><td>{e.device_id}</td><td><span className={`status ${String(e.status).toLowerCase()}`}>{e.status}</span></td><td><MapLinks event={e} onShowMap={onShowMap} /></td></tr>)}</tbody></table>{!rows.length && <div className="empty">No events match the current filters.</div>}</div><div className="pager">Showing {rows.length ? ((page - 1) * PAGE_SIZE) + 1 : 0} to {Math.min(page * PAGE_SIZE, events.length)} of {events.length.toLocaleString()} entries <span><button onClick={() => setPage(Math.max(1, page - 1))}><ChevronLeft size={16}/></button><b>{page}</b><em>of {pages}</em><button onClick={() => setPage(Math.min(pages, page + 1))}><ChevronRight size={16}/></button></span></div></section>;
}

function RightRail({ events, onShowMap, verifiedAlerts = new Set(), onVerifyAlert }) {
  const alerts = events.filter((event) => ["medium", "high"].includes(event.severity) && !verifiedAlerts.has(alertKey(event))).slice(0, 3);
  return <aside className="right-rail"><section className="panel alerts"><h3>ALERTS <a>Unverified</a></h3>{alerts.length ? alerts.map((event) => <AlertCard event={event} key={`${event.event_id}-${event.timestamp_ms}`} onShowMap={onShowMap} onVerify={onVerifyAlert} compact />) : <div className="empty compact">No unverified medium or high alerts.</div>}</section></aside>;
}

function ReportsView({ events, metrics }) {
  const peakBuckets = bucketByPeak(events);
  return <section className="view-grid reports-view">
    <SeverityPanel metrics={metrics} />
    <section className="panel report-panel"><h3>SEVERITY SNAPSHOT</h3><div className="report-bars">
      {["low", "medium", "high"].map((key) => <p key={key}><span>{cap(key)}</span><i><b className={key} style={{ width: `${percent(metrics[key], metrics.total)}%` }} /></i><em>{metrics[key].toLocaleString()}</em></p>)}
    </div></section>
    <section className="panel report-panel"><h3>PEAK G-FORCE RANGES</h3><div className="report-bars">
      {peakBuckets.map((bucket) => <p key={bucket.label}><span>{bucket.label}</span><i><b className={bucket.tone} style={{ width: `${percent(bucket.count, events.length)}%` }} /></i><em>{bucket.count}</em></p>)}
    </div></section>
    <section className="panel report-panel"><h3>REPORT SUMMARY</h3><div className="summary-list"><p>Total records <b>{metrics.total.toLocaleString()}</b></p><p>High severity share <b>{percent(metrics.high, metrics.total)}%</b></p><p>Maximum impact <b>{metrics.peak.toFixed(2)} G</b></p><p>Latest detection <b>{events[0] ? formatTime(events[0].timestamp_ms) : "None"}</b></p></div></section>
  </section>;
}

function AnalyticsView({ events, metrics }) {
  const halfHourly = halfHourCounts(events);
  const maxBucket = Math.max(...halfHourly.map((bucket) => bucket.count), 1);
  return <section className="view-grid analytics-view">
    <section className="panel analytics-panel"><h3>EVENTS BY 30 MINUTES</h3><div className="hour-chart half-hour-chart">{halfHourly.map((item) => <span key={item.label} title={`${item.label} - ${item.count} events`} style={{ height: `${Math.max(8, percent(item.count, maxBucket))}%` }} />)}</div></section>
    <section className="panel analytics-panel"><h3>LIVE ANALYTICS</h3><div className="summary-list"><p>Average peak G <b>{avg(events.map((e) => e.peak_g)).toFixed(2)} G</b></p><p>Average magnitude <b>{avg(events.map((e) => e.magnitude_g)).toFixed(2)}</b></p><p>Critical ratio <b>{percent(metrics.high, metrics.total)}%</b></p><p>Events analyzed <b>{events.length.toLocaleString()}</b></p></div></section>
    <section className="panel analytics-panel wide"><h3>RECENT HIGH IMPACT EVENTS</h3><MiniEventList events={events.filter((event) => event.severity === "high").slice(0, 8)} /></section>
  </section>;
}

function DevicesView({ events }) {
  const devices = Object.values(events.reduce((acc, event) => {
    const id = event.device_id || "ESP32";
    acc[id] ||= { id, latest: 0 };
    acc[id].latest = Math.max(acc[id].latest, event.timestamp_ms || 0);
    return acc;
  }, {})).sort((a, b) => String(a.id).localeCompare(String(b.id)));
  return <section className="view-grid devices-view">{devices.map((device) => <article className="panel device-card" key={device.id}><div><CircuitBoard/><h3>{device.id}</h3></div><p>Status <b>Online</b></p><p>Last heartbeat <b>{formatTime(device.latest)}</b></p></article>)}</section>;
}

function AlertsView({ events, onShowMap, verifiedAlerts = new Set(), onVerifyAlert }) {
  const alerts = events.filter((event) => ["medium", "high"].includes(event.severity)).slice(0, 50);
  return <section className="view-grid single"><section className="panel alerts-full"><h3>MEDIUM AND HIGH SEVERITY ALERTS</h3><div className="alert-list">{alerts.length ? alerts.map((event) => <AlertCard event={event} key={`${event.event_id}-${event.timestamp_ms}`} onShowMap={onShowMap} onVerify={onVerifyAlert} verified={verifiedAlerts.has(alertKey(event))} />) : <div className="empty compact">No medium or high alerts.</div>}</div></section></section>;
}

function SettingsView({ refreshMs, setRefreshMs, themeMode, setThemeMode, setActiveView }) {
  return <section className="view-grid single"><section className="panel settings-view">
    <h3>DASHBOARD SETTINGS</h3>
    <div className="settings-grid">
      <section className="setting-card">
        <h4>Appearance</h4>
        <div className="choice-row">
          <button className={themeMode === "dark" ? "active" : ""} onClick={() => setThemeMode("dark")}>Dark</button>
          <button className={themeMode === "light" ? "active" : ""} onClick={() => setThemeMode("light")}>Light</button>
        </div>
      </section>
      <section className="setting-card">
        <h4>Data</h4>
        <div className="choice-row">
          <button className={refreshMs === 3000 ? "active" : ""} onClick={() => setRefreshMs(3000)}>3 sec</button>
          <button className={refreshMs === 10000 ? "active" : ""} onClick={() => setRefreshMs(10000)}>10 sec</button>
          <button className={refreshMs === 15000 ? "active" : ""} onClick={() => setRefreshMs(15000)}>15 sec</button>
        </div>
      </section>
      <section className="setting-card about-card">
        <h4>About</h4>
        <p>RoadPulse monitors pothole events from ESP32 LTE devices and shows live road safety data on AWS.</p>
      </section>
    </div>
  </section></section>;
}

function MiniEventList({ events }) {
  return <div className="mini-list">{events.length ? events.map((event) => <p key={`${event.event_id}-${event.timestamp_ms}`}><span><b>#{event.event_id}</b>{event.road}</span><em>{event.peak_g.toFixed(2)} G</em><small>{formatTime(event.timestamp_ms)}</small></p>) : <div className="empty compact">No matching events.</div>}</div>;
}

function AlertCard({ event, onShowMap, onVerify, verified = false, compact = false }) {
  const hasGps = isValidCoordinate(event);
  return <article className={`alert-card ${event.severity} ${verified ? "verified" : ""} ${compact ? "compact-card" : ""}`}>
    <AlertTriangle />
    <div>
      <b>{cap(event.severity)} Severity Pothole</b>
      <span>#{event.event_id} · {event.road}</span>
      <small>{coordinateText(event)}</small>
    </div>
    <time>{formatTime(event.timestamp_ms)}</time>
    <div className="alert-actions">
      <MapLinks event={event} onShowMap={onShowMap} />
      {verified ? <span>Verified</span> : <button onClick={() => onVerify?.(event)}>Verify</button>}
    </div>
  </article>;
}

function MapLinks({ event, onShowMap }) {
  const hasGps = isValidCoordinate(event);
  const mapsUrl = hasGps ? `https://www.google.com/maps/search/?api=1&query=${encodeURIComponent(`${event.lat},${event.lng}`)}` : "";
  return <div className="map-links">
    <button disabled={!hasGps} onClick={() => onShowMap?.(event)}>{hasGps ? "Show on map" : "No GPS"}</button>
    {hasGps && <a className="map-button" href={mapsUrl} target="_blank" rel="noreferrer">Open in Maps</a>}
  </div>;
}

function bucketByPeak(events) {
  return [
    { label: "1.5 - 2.5 G", tone: "low", count: events.filter((event) => event.peak_g >= 1.5 && event.peak_g < 2.5).length },
    { label: "2.5 - 3.5 G", tone: "medium", count: events.filter((event) => event.peak_g >= 2.5 && event.peak_g < 3.5).length },
    { label: "> 3.5 G", tone: "high", count: events.filter((event) => event.peak_g >= 3.5).length }
  ];
}

function halfHourCounts(events) {
  const counts = Array.from({ length: 48 }, (_, index) => {
    const hour = Math.floor(index / 2);
    const minute = index % 2 === 0 ? "00" : "30";
    return { label: `${String(hour).padStart(2, "0")}:${minute}`, count: 0 };
  });
  events.forEach((event) => {
    if (event.timestamp_ms) {
      const date = new Date(event.timestamp_ms);
      const index = (date.getHours() * 2) + (date.getMinutes() >= 30 ? 1 : 0);
      counts[index].count += 1;
    }
  });
  return counts;
}

function avg(values) {
  const nums = values.filter((value) => Number.isFinite(value));
  return nums.length ? nums.reduce((sum, value) => sum + value, 0) / nums.length : 0;
}

function coordinateText(event) {
  return isValidCoordinate(event)
    ? `Lat ${Number(event.lat).toFixed(6)}, Lng ${Number(event.lng).toFixed(6)}`
    : "GPS not available";
}

function sameEvent(a, b) {
  return !!a && !!b && String(a.event_id) === String(b.event_id) && Number(a.timestamp_ms) === Number(b.timestamp_ms);
}

function mergeFocusEvent(events, focusEvent) {
  if (!focusEvent) return events;
  return events.some((event) => sameEvent(event, focusEvent)) ? events : [focusEvent, ...events];
}

function alertKey(event) {
  return `${event.event_id}-${event.timestamp_ms}`;
}

function rangeStart(range) {
  const table = { "15m": 15 * 60e3, "1h": 60 * 60e3, "6h": 6 * 60 * 60e3, "24h": 24 * 60 * 60e3, "7d": 7 * 24 * 60 * 60e3 };
  return table[range] ? Date.now() - table[range] : 0;
}

function percent(value, total) { return total ? Math.round((value / total) * 100) : 0; }
function cap(s) { return String(s).charAt(0).toUpperCase() + String(s).slice(1).toLowerCase(); }
function tone(severity) { return severity === "high" ? "red" : severity === "medium" ? "orange" : "green"; }

function exportCsv(events) {
  const header = ["event_id","severity","peak_g","x_g","y_g","z_g","magnitude_g","timestamp_ms","road","device_id","status"];
  const csv = [header, ...events.map((e) => header.map((key) => e[key]))].map((row) => row.map(csvValue).join(",")).join("\n");
  download(`roadpulse-events-${Date.now()}.csv`, csv, "text/csv");
}

function downloadJson(events) {
  download(`roadpulse-events-${Date.now()}.json`, JSON.stringify(events, null, 2), "application/json");
}

function csvValue(value) {
  const text = String(value ?? "");
  return /[",\n]/.test(text) ? `"${text.replaceAll('"', '""')}"` : text;
}

function download(filename, content, type) {
  const blob = new Blob([content], { type });
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = filename;
  link.click();
  URL.revokeObjectURL(url);
}

createRoot(document.getElementById("root")).render(<App />);
