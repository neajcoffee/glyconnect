// Scriptable - Glucose Widget — Style natif iOS
// Paramètre : nombre d'heures (ex: 3)
const HEURES = parseInt(args.widgetParameter) || 3
const HYPO = 70
const HYPER = 180
const NTFY_URL = `https://ntfy.sh/neaj-glyconnect-data/json?poll=1&since=${HEURES * 3600}`

const C = {
  green:    new Color("#34c759"),
  orange:   new Color("#ff9500"),
  red:      new Color("#ff3b30"),
  blue:     new Color("#007aff"),
  label:    Color.dynamic(new Color("#8e8e93"), new Color("#8e8e93")),
  sep:      Color.dynamic(new Color("#e5e5ea"), new Color("#38383a")),
  bg:       Color.dynamic(new Color("#ffffff"), new Color("#1c1c1e")),
  card:     Color.dynamic(new Color("#f2f2f7"), new Color("#2c2c2e")),
  cardStat: Color.dynamic(new Color("#dddde2"), new Color("#3a3a3c")),
  footer:   Color.dynamic(new Color("#e8e8ed"), new Color("#2a2a2c")),
  text:     Color.dynamic(new Color("#1c1c1e"), new Color("#ffffff")),
}

function glucoseColor(mgdl) {
  if (mgdl < HYPO)   return C.red
  if (mgdl <= HYPER) return C.green
  return C.orange
}

function trendArrow(trend) {
  if (trend >= 30)  return "↑↑"
  if (trend >= 10)  return "↑"
  if (trend >= 3)   return "↗"
  if (trend >= -3)  return "→"
  if (trend >= -10) return "↘"
  if (trend >= -30) return "↓"
  return "↓↓"
}

function isDonneePerimee(ts) {
  return (Math.floor(Date.now() / 1000) - ts) > 40 * 60
}

function calculStats(points) {
  if (!points.length) return { tir: 0, hypo: 0, hyper: 0, min: 0, max: 0 }
  const inRange = points.filter(p => p.mgdl >= HYPO && p.mgdl <= HYPER).length
  const inHypo  = points.filter(p => p.mgdl < HYPO).length
  const inHyper = points.filter(p => p.mgdl > HYPER).length
  const mgdls   = points.map(p => p.mgdl)
  return {
    tir:   Math.round(inRange / points.length * 100),
    hypo:  Math.round(inHypo  / points.length * 100),
    hyper: Math.round(inHyper / points.length * 100),
    min:   Math.min(...mgdls),
    max:   Math.max(...mgdls),
  }
}

function calculMesuresManquantes(points, heures) {
  const attendues = Math.floor(heures * 60 / 5)
  const manquantes = Math.max(0, attendues - points.length)
  return { attendues, manquantes, recues: points.length, pct: Math.round(manquantes / attendues * 100) }
}

async function fetchGlucoseHistory() {
  const req = new Request(NTFY_URL)
  const raw = await req.loadString()
  const lines = raw.trim().split("\n").filter(l => l.trim())
  if (!lines.length) return { latest: null, points: [] }

  const all = []
  for (const line of lines) {
    try {
      const msg = JSON.parse(line)
      const data = JSON.parse(msg.message)
      const ts = data.ts || msg.time
      all.push({
        mgdl: data.mgdl,
        mmol: data.mmol,
        trend: data.trend,
        ts,
        sensorOk: data.sensorOk !== false
      })
    } catch(e) {}
  }
  all.sort((a, b) => a.ts - b.ts)
  if (!all.length) return { latest: null, points: [] }

  const latest = all[all.length - 1]
  const now = Math.floor(Date.now() / 1000)
  const minTs = now - HEURES * 3600
  const points = all.filter(p => p.ts >= minTs && p.sensorOk)

  return { latest, points }
}

function drawEmptyChart(w, h) {
  const ctx = new DrawContext()
  ctx.size = new Size(w, h)
  ctx.opaque = false
  ctx.respectScreenScale = true

  const pL = 28, pR = 4, pT = 6, pB = 18
  const cW = w - pL - pR
  const cH = h - pT - pB

  ctx.setFillColor(new Color("#34c759", 0.06))
  ctx.fillRect(new Rect(pL, pT, cW, cH))

  ctx.setStrokeColor(new Color("#c7c7cc", 0.2))
  ctx.setLineWidth(0.5)
  const border = new Path()
  border.move(new Point(pL, pT))
  border.addLine(new Point(pL + cW, pT))
  border.addLine(new Point(pL + cW, pT + cH))
  border.addLine(new Point(pL, pT + cH))
  border.closeSubpath()
  ctx.addPath(border)
  ctx.strokePath()

  ctx.setFont(Font.systemFont(11))
  ctx.setTextColor(new Color("#8e8e93"))
  ctx.setTextAlignedCenter()
  ctx.drawTextInRect(
    "Pas assez de données",
    new Rect(pL, pT + cH / 2 - 8, cW, 16)
  )

  return ctx.getImage()
}

function drawChart(points, w, h) {
  if (points.length < 2) return null

  const ctx = new DrawContext()
  ctx.size = new Size(w, h)
  ctx.opaque = false
  ctx.respectScreenScale = true

  const pL = 28, pR = 4, pT = 6, pB = 18
  const cW = w - pL - pR
  const cH = h - pT - pB
  const minVal = 40, maxVal = 300
  const now = Math.floor(Date.now() / 1000)
  const minTs = now - HEURES * 3600

  function xp(ts) { return pL + ((ts - minTs) / (HEURES * 3600)) * cW }
  function yp(mgdl) {
    const v = Math.min(Math.max(mgdl, minVal), maxVal)
    return pT + cH - ((v - minVal) / (maxVal - minVal)) * cH
  }

  ctx.setFillColor(new Color("#34c759", 0.08))
  ctx.fillRect(new Rect(pL, yp(HYPER), cW, yp(HYPO) - yp(HYPER)))

  const yReperes = [70, 140, 210, 280]
  for (const val of yReperes) {
    const py = yp(val)
    ctx.setStrokeColor(new Color("#c7c7cc", 0.25))
    ctx.setLineWidth(0.4)
    const hl = new Path()
    hl.move(new Point(pL, py))
    hl.addLine(new Point(pL + cW, py))
    ctx.addPath(hl)
    ctx.strokePath()
    ctx.setFont(Font.systemFont(7))
    ctx.setTextColor(new Color("#8e8e93", 0.8))
    ctx.setTextAlignedRight()
    ctx.drawTextInRect(String(val), new Rect(0, py - 5, pL - 4, 10))
    ctx.setTextAlignedLeft()
  }

  ctx.setStrokeColor(new Color("#ff3b30", 0.35))
  ctx.setLineWidth(0.75)
  for (let px = pL; px < pL + cW; px += 8) {
    const p = new Path()
    p.move(new Point(px, yp(HYPO)))
    p.addLine(new Point(Math.min(px + 4, pL + cW), yp(HYPO)))
    ctx.addPath(p)
    ctx.strokePath()
  }

  ctx.setStrokeColor(new Color("#ff9500", 0.35))
  for (let px = pL; px < pL + cW; px += 8) {
    const p = new Path()
    p.move(new Point(px, yp(HYPER)))
    p.addLine(new Point(Math.min(px + 4, pL + cW), yp(HYPER)))
    ctx.addPath(p)
    ctx.strokePath()
  }

  const sd = new Date(minTs * 1000)
  let fh = new Date(sd)
  fh.setMinutes(0, 0, 0)
  fh.setHours(fh.getHours() + 1)
  let tickTs = Math.floor(fh.getTime() / 1000)
  while (tickTs <= now) {
    const px = xp(tickTs)
    ctx.setStrokeColor(new Color("#c7c7cc", 0.4))
    ctx.setLineWidth(0.5)
    const vl = new Path()
    vl.move(new Point(px, pT))
    vl.addLine(new Point(px, pT + cH))
    ctx.addPath(vl)
    ctx.strokePath()
    ctx.setFont(Font.systemFont(8))
    ctx.setTextColor(new Color("#8e8e93"))
    ctx.setTextAlignedLeft()
    const d = new Date(tickTs * 1000)
    ctx.drawTextInRect(`${d.getHours()}h`, new Rect(px - 8, pT + cH + 3, 16, 12))
    tickTs += 3600
  }

  const fillPath = new Path()
  let first = true
  for (const pt of points) {
    if (first) {
      fillPath.move(new Point(xp(pt.ts), pT + cH))
      fillPath.addLine(new Point(xp(pt.ts), yp(pt.mgdl)))
      first = false
    } else {
      fillPath.addLine(new Point(xp(pt.ts), yp(pt.mgdl)))
    }
  }
  const last = points[points.length - 1]
  fillPath.addLine(new Point(xp(last.ts), pT + cH))
  fillPath.closeSubpath()
  ctx.setFillColor(new Color("#007aff", 0.07))
  ctx.addPath(fillPath)
  ctx.fillPath()

  const curve = new Path()
  let started = false
  for (const pt of points) {
    if (!started) { curve.move(new Point(xp(pt.ts), yp(pt.mgdl))); started = true }
    else curve.addLine(new Point(xp(pt.ts), yp(pt.mgdl)))
  }
  ctx.setStrokeColor(new Color("#007aff", 0.7))
  ctx.setLineWidth(1.5)
  ctx.addPath(curve)
  ctx.strokePath()

  for (const pt of points) {
    const px = xp(pt.ts), py = yp(pt.mgdl), r = 2.5
    ctx.setFillColor(C.bg)
    ctx.fillEllipse(new Rect(px - r - 0.8, py - r - 0.8, (r + 0.8) * 2, (r + 0.8) * 2))
    ctx.setFillColor(glucoseColor(pt.mgdl))
    ctx.fillEllipse(new Rect(px - r, py - r, r * 2, r * 2))
  }

  return ctx.getImage()
}

function addStatCard(stack, label, value, color) {
  const card = stack.addStack()
  card.layoutVertically()
  card.setPadding(5, 8, 5, 8)
  card.cornerRadius = 8
  card.backgroundColor = C.cardStat
  card.centerAlignContent()
  const lbl = card.addText(label)
  lbl.font = Font.systemFont(8)
  lbl.textColor = C.label
  lbl.centerAlignText()
  const val = card.addText(value)
  val.font = Font.boldRoundedSystemFont(14)
  val.textColor = color || C.text
  val.centerAlignText()
}

function drawSegmentBar(hypo, tir, hyper, w, h) {
  const ctx = new DrawContext()
  ctx.size = new Size(w, h)
  ctx.opaque = false
  ctx.respectScreenScale = true
  const total = w
  const wHypo  = Math.round(hypo  / 100 * total)
  const wTir   = Math.round(tir   / 100 * total)
  const wHyper = total - wHypo - wTir
  if (wHypo > 0)  { ctx.setFillColor(C.red);    ctx.fillRect(new Rect(0, 0, wHypo, h)) }
  if (wTir > 0)   { ctx.setFillColor(C.green);  ctx.fillRect(new Rect(wHypo, 0, wTir, h)) }
  if (wHyper > 0) { ctx.setFillColor(C.orange); ctx.fillRect(new Rect(wHypo + wTir, 0, wHyper, h)) }
  return ctx.getImage()
}

// ── WIDGET CIRCULAR ───────────────────────────────
function buildCircularWidget(latest) {
  const widget = new ListWidget()
  widget.addSpacer()

  if (!latest || isDonneePerimee(latest.ts) || !latest.sensorOk) {
    const t = widget.addText(!latest?.sensorOk ? "OFF" : "--")
    t.font = Font.boldRoundedSystemFont(20)
    t.textColor = !latest?.sensorOk ? C.red : Color.white()
    t.centerAlignText()
    t.minimumScaleFactor = 0.3
    t.lineLimit = 1
    widget.addSpacer()
    return widget
  }

  const valRow = widget.addStack()
  valRow.layoutHorizontally()
  valRow.centerAlignContent()
  valRow.addSpacer()

  const val = valRow.addText(String(latest.mgdl))
  val.font = Font.boldRoundedSystemFont(20)
  val.textColor = Color.white()
  val.minimumScaleFactor = 0.3
  val.lineLimit = 1

  const arrow = valRow.addText(trendArrow(latest.trend))
  arrow.font = Font.boldRoundedSystemFont(20)
  arrow.textColor = Color.white()
  arrow.minimumScaleFactor = 0.3
  arrow.lineLimit = 1

  valRow.addSpacer()
  widget.addSpacer(1)

  const timeDate = widget.addDate(new Date(latest.ts * 1000))
  timeDate.applyRelativeStyle()
  timeDate.font = Font.systemFont(7)
  timeDate.textColor = Color.white()
  timeDate.centerAlignText()
  timeDate.minimumScaleFactor = 0.3

  widget.addSpacer()
  return widget
}

// ── WIDGET LOCK SCREEN RECTANGULAR ───────────────
function buildLockScreenWidget(latest) {
  const widget = new ListWidget()
  widget.addSpacer()

  if (!latest || isDonneePerimee(latest.ts) || !latest.sensorOk) {
    const t = widget.addText(!latest?.sensorOk ? "Capteur OFF" : "GLYCÉMIE  --")
    t.font = Font.boldRoundedSystemFont(16)
    t.textColor = Color.white()
    t.centerAlignText()
    t.minimumScaleFactor = 0.3
    t.lineLimit = 1
    widget.addSpacer()
    return widget
  }

  const valRow = widget.addStack()
  valRow.layoutHorizontally()
  valRow.centerAlignContent()

  const valTxt = valRow.addText(String(latest.mgdl))
  valTxt.font = Font.boldRoundedSystemFont(22)
  valTxt.textColor = Color.white()
  valTxt.minimumScaleFactor = 0.3
  valTxt.lineLimit = 1

  valRow.addSpacer(4)

  const arrowTxt = valRow.addText(trendArrow(latest.trend))
  arrowTxt.font = Font.boldRoundedSystemFont(22)
  arrowTxt.textColor = Color.white()
  arrowTxt.minimumScaleFactor = 0.3
  arrowTxt.lineLimit = 1

  widget.addSpacer(2)

  const timeRow = widget.addStack()
  timeRow.layoutHorizontally()
  timeRow.centerAlignContent()

  const ilYa = timeRow.addText("il y a ")
  ilYa.font = Font.systemFont(12)
  ilYa.textColor = Color.white()
  ilYa.minimumScaleFactor = 0.3
  ilYa.lineLimit = 1

  const timeDate = timeRow.addDate(new Date(latest.ts * 1000))
  timeDate.applyRelativeStyle()
  timeDate.font = Font.systemFont(12)
  timeDate.textColor = Color.white()
  timeDate.minimumScaleFactor = 0.3

  widget.addSpacer()
  return widget
}

// ── WIDGET SMALL ──────────────────────────────────
function buildSmallWidget(latest) {
  const widget = new ListWidget()
  widget.setPadding(14, 16, 14, 16)
  widget.backgroundGradient = (() => {
    const g = new LinearGradient()
    g.colors = [Color.dynamic(new Color("#f8f8f8"), new Color("#2c2c2e")), C.bg]
    g.locations = [0, 1]
    g.startPoint = new Point(0, 0)
    g.endPoint = new Point(0, 1)
    return g
  })()

  if (!latest) {
    widget.addSpacer()
    const t = widget.addText("--")
    t.font = Font.boldRoundedSystemFont(48)
    t.textColor = C.label
    t.centerAlignText()
    widget.addSpacer()
    return widget
  }

  const perimee = isDonneePerimee(latest.ts)
  const sensorOff = !latest.sensorOk
  const color = perimee ? C.label : sensorOff ? C.red : glucoseColor(latest.mgdl)

  widget.addSpacer()

  const lbl = widget.addText("GLYCÉMIE")
  lbl.font = Font.semiboldRoundedSystemFont(9)
  lbl.textColor = C.label
  lbl.centerAlignText()

  widget.addSpacer(4)

  const valRow = widget.addStack()
  valRow.layoutHorizontally()
  valRow.centerAlignContent()
  valRow.addSpacer()

  const bigVal = valRow.addText(perimee ? "--" : sensorOff ? "OFF" : String(latest.mgdl))
  bigVal.font = Font.boldRoundedSystemFont(48)
  bigVal.textColor = color
  bigVal.minimumScaleFactor = 0.3
  bigVal.lineLimit = 1

  if (!perimee && !sensorOff) {
    const arrow = valRow.addText(trendArrow(latest.trend))
    arrow.font = Font.boldRoundedSystemFont(48)
    arrow.textColor = C.text
    arrow.minimumScaleFactor = 0.3
    arrow.lineLimit = 1
  }

  valRow.addSpacer()
  widget.addSpacer(4)

  if (!perimee && !sensorOff) {
    const timeRow = widget.addStack()
    timeRow.layoutHorizontally()
    timeRow.centerAlignContent()
    timeRow.addSpacer()

    const ilYa = timeRow.addText("il y a ")
    ilYa.font = Font.systemFont(11)
    ilYa.textColor = C.label
    ilYa.minimumScaleFactor = 0.3
    ilYa.lineLimit = 1

    const timeDate = timeRow.addDate(new Date(latest.ts * 1000))
    timeDate.applyRelativeStyle()
    timeDate.font = Font.systemFont(11)
    timeDate.textColor = C.label
    timeDate.minimumScaleFactor = 0.3

    timeRow.addSpacer()
  }

  widget.addSpacer()
  return widget
}

// ── WIDGET MEDIUM ─────────────────────────────────
function buildMediumWidget(latest, points) {
  const widget = new ListWidget()
  widget.setPadding(0, 0, 0, 0)

  const gradient = new LinearGradient()
  gradient.colors = [
    Color.dynamic(new Color("#f8f8f8"), new Color("#2c2c2e")),
    C.bg,
    C.bg,
  ]
  gradient.locations = [0, 0.15, 1]
  gradient.startPoint = new Point(0, 0)
  gradient.endPoint = new Point(0, 1)
  widget.backgroundGradient = gradient

  if (!latest) {
    widget.addSpacer()
    const t = widget.addText("Aucune mesure")
    t.font = Font.mediumRoundedSystemFont(13)
    t.textColor = C.label
    t.centerAlignText()
    widget.addSpacer()
    return widget
  }

  const perimee = isDonneePerimee(latest.ts)
  const sensorOff = !latest.sensorOk
  const color = perimee ? C.label : sensorOff ? C.red : glucoseColor(latest.mgdl)

  const body = widget.addStack()
  body.layoutHorizontally()
  body.setPadding(14, 16, 14, 16)
  body.centerAlignContent()

  const left = body.addStack()
  left.layoutVertically()

  const lbl = left.addText("GLYCÉMIE")
  lbl.font = Font.semiboldRoundedSystemFont(9)
  lbl.textColor = C.label

  left.addSpacer(2)

  const valRow = left.addStack()
  valRow.layoutHorizontally()
  valRow.bottomAlignContent()

  const bigVal = valRow.addText(perimee ? "--" : sensorOff ? "OFF" : String(latest.mgdl))
  bigVal.font = Font.boldRoundedSystemFont(48)
  bigVal.textColor = color
  bigVal.minimumScaleFactor = 0.3
  bigVal.lineLimit = 1

  if (!perimee && !sensorOff) {
    const arrow = valRow.addText(trendArrow(latest.trend))
    arrow.font = Font.boldRoundedSystemFont(48)
    arrow.textColor = C.text
    arrow.minimumScaleFactor = 0.3
    arrow.lineLimit = 1
  }

  left.addSpacer(6)

  if (!perimee && !sensorOff) {
    const timeRow = left.addStack()
    timeRow.layoutHorizontally()

    const ilYa = timeRow.addText("il y a ")
    ilYa.font = Font.systemFont(10)
    ilYa.textColor = C.label
    ilYa.minimumScaleFactor = 0.3
    ilYa.lineLimit = 1

    const timeDate = timeRow.addDate(new Date(latest.ts * 1000))
    timeDate.applyRelativeStyle()
    timeDate.font = Font.systemFont(10)
    timeDate.textColor = C.label
    timeDate.minimumScaleFactor = 0.3
  }

  body.addSpacer()

  const chartW = 210
  const chartH = 110
  const chartImage = drawChart(points, chartW, chartH) ?? drawEmptyChart(chartW, chartH)
  const img = body.addImage(chartImage)
  img.imageSize = new Size(chartW, chartH)

  return widget
}

// ── WIDGET LARGE ──────────────────────────────────
async function buildLargeWidget(result) {
  const fetchDate = new Date()

  const widget = new ListWidget()
  widget.setPadding(0, 0, 0, 0)

  const gradient = new LinearGradient()
  gradient.colors = [
    Color.dynamic(new Color("#f8f8f8"), new Color("#2c2c2e")),
    C.bg,
    C.bg,
    C.sep,
    C.footer,
    C.footer,
  ]
  gradient.locations = [0, 0.15, 0.6999, 0.7001, 0.702, 1]
  gradient.startPoint = new Point(0, 0)
  gradient.endPoint = new Point(0, 1)
  widget.backgroundGradient = gradient

  const { latest, points } = result
  const stats = calculStats(points)
  const mm = calculMesuresManquantes(points, HEURES)

  if (!latest) {
    widget.addSpacer()
    const t = widget.addText(`Aucune mesure depuis ${HEURES}h`)
    t.font = Font.mediumRoundedSystemFont(13)
    t.textColor = C.label
    t.centerAlignText()
    widget.addSpacer()
    return widget
  }

  const perimee = isDonneePerimee(latest.ts)
  const sensorOff = !latest.sensorOk
  const color = perimee ? C.label : sensorOff ? C.red : glucoseColor(latest.mgdl)

  const body = widget.addStack()
  body.layoutVertically()
  body.setPadding(10, 22, 8, 22)

  const header = body.addStack()
  header.layoutHorizontally()
  header.centerAlignContent()

  const valBlock = header.addStack()
  valBlock.layoutVertically()

  const lbl = valBlock.addText("GLYCÉMIE")
  lbl.font = Font.semiboldRoundedSystemFont(9)
  lbl.textColor = C.label

  valBlock.addSpacer(2)

  const valRow = valBlock.addStack()
  valRow.layoutHorizontally()
  valRow.bottomAlignContent()

  const bigVal = valRow.addText(perimee ? "--" : sensorOff ? "OFF" : String(latest.mgdl))
  bigVal.font = Font.boldRoundedSystemFont(58)
  bigVal.textColor = color
  bigVal.minimumScaleFactor = 0.3
  bigVal.lineLimit = 1

  if (!perimee && !sensorOff) {
    const arrow = valRow.addText(trendArrow(latest.trend))
    arrow.font = Font.boldRoundedSystemFont(58)
    arrow.textColor = C.text
    arrow.minimumScaleFactor = 0.3
    arrow.lineLimit = 1
  }

  header.addSpacer()

  const CARD_W = 72

  const right = header.addStack()
  right.layoutVertically()

  const mmolCard = right.addStack()
  mmolCard.layoutVertically()
  mmolCard.setPadding(4, 8, 4, 8)
  mmolCard.cornerRadius = 8
  mmolCard.backgroundColor = C.card
  mmolCard.size = new Size(CARD_W, 0)
  mmolCard.centerAlignContent()
  const mmolLbl = mmolCard.addText("mmol/L")
  mmolLbl.font = Font.systemFont(8)
  mmolLbl.textColor = C.label
  mmolLbl.centerAlignText()
  const mmolVal = mmolCard.addText(perimee || sensorOff ? "--" : latest.mmol.toFixed(1))
  mmolVal.font = Font.boldRoundedSystemFont(15)
  mmolVal.textColor = C.text
  mmolVal.centerAlignText()

  right.addSpacer(5)

  const tirCard = right.addStack()
  tirCard.layoutVertically()
  tirCard.setPadding(4, 8, 4, 8)
  tirCard.cornerRadius = 8
  tirCard.backgroundColor = C.card
  tirCard.size = new Size(CARD_W, 0)
  tirCard.centerAlignContent()
  const tirLbl = tirCard.addText(`TIR ${HEURES}h`)
  tirLbl.font = Font.systemFont(8)
  tirLbl.textColor = C.label
  tirLbl.centerAlignText()
  const tirVal = tirCard.addText(points.length ? `${stats.tir}%` : "--")
  tirVal.font = Font.boldRoundedSystemFont(15)
  tirVal.textColor = points.length ? (stats.tir >= 70 ? C.green : stats.tir >= 50 ? C.orange : C.red) : C.label
  tirVal.centerAlignText()

  if (!perimee && !sensorOff) {
    body.addSpacer(4)
    const timeRow = body.addStack()
    timeRow.layoutHorizontally()
    timeRow.centerAlignContent()

    const ilYa = timeRow.addText("il y a ")
    ilYa.font = Font.systemFont(10)
    ilYa.textColor = C.label

    const timeDate = timeRow.addDate(new Date(latest.ts * 1000))
    timeDate.applyRelativeStyle()
    timeDate.font = Font.systemFont(10)
    timeDate.textColor = C.label
  } else {
    body.addSpacer(20)
  }

  body.addSpacer(6)
  const sepStack = body.addStack()
  sepStack.backgroundColor = C.sep
  sepStack.size = new Size(0, 0.5)
  body.addSpacer(6)

  const chartW = 310
  const chartH = 130
  const chartImage = drawChart(points, chartW, chartH) ?? drawEmptyChart(chartW, chartH)
  const img = body.addImage(chartImage)
  img.imageSize = new Size(chartW, chartH)
  img.centerAlignImage()

  widget.addSpacer(8)

  const footer = widget.addStack()
  footer.layoutVertically()
  footer.setPadding(6, 22, 6, 22)

  const bottomRow = footer.addStack()
  bottomRow.layoutHorizontally()
  bottomRow.centerAlignContent()
  bottomRow.spacing = 4

  if (points.length) {
    addStatCard(bottomRow, "MIN", `${stats.min}`, glucoseColor(stats.min))
    addStatCard(bottomRow, "MAX", `${stats.max}`, glucoseColor(stats.max))
  } else {
    addStatCard(bottomRow, "MIN", "--", C.label)
    addStatCard(bottomRow, "MAX", "--", C.label)
  }

  bottomRow.addSpacer()

  const barBlock = bottomRow.addStack()
  barBlock.layoutVertically()

  const barLbl = barBlock.addText("RÉPARTITION")
  barLbl.font = Font.systemFont(8)
  barLbl.textColor = C.label

  barBlock.addSpacer(4)

  const barW = 130
  const barH = 8
  const barImg = barBlock.addImage(drawSegmentBar(stats.hypo, stats.tir, stats.hyper, barW, barH))
  barImg.imageSize = new Size(barW, barH)
  barImg.cornerRadius = 4

  barBlock.addSpacer(3)

  const legend = barBlock.addStack()
  legend.layoutHorizontally()
  legend.size = new Size(barW, 12)

  const leftLbl = legend.addText(`${stats.hypo}%`)
  leftLbl.font = Font.systemFont(8)
  leftLbl.textColor = C.red

  legend.addSpacer()

  const centerLbl = legend.addText(`${stats.tir}%`)
  centerLbl.font = Font.systemFont(8)
  centerLbl.textColor = C.green

  legend.addSpacer()

  const rightLbl = legend.addText(`${stats.hyper}%`)
  rightLbl.font = Font.systemFont(8)
  rightLbl.textColor = C.orange

  footer.addSpacer(6)
  const sep3 = footer.addStack()
  sep3.backgroundColor = C.sep
  sep3.size = new Size(0, 0.5)
  footer.addSpacer(6)

  const mmRow = footer.addStack()
  mmRow.layoutHorizontally()
  mmRow.centerAlignContent()

  const mmLbl = mmRow.addText(`${mm.recues} mesures reçues sur `)
  mmLbl.font = Font.systemFont(9)
  mmLbl.textColor = C.label

  const mmVal = mmRow.addText(`${mm.attendues} attendues`)
  mmVal.font = Font.boldRoundedSystemFont(9)
  mmVal.textColor = C.label

  footer.addSpacer(4)

  const lastRow = footer.addStack()
  lastRow.layoutHorizontally()
  lastRow.centerAlignContent()

  const lastLbl = lastRow.addText("Dernière mesure à ")
  lastLbl.font = Font.systemFont(9)
  lastLbl.textColor = C.label

  const lastDate = lastRow.addDate(new Date(latest.ts * 1000))
  lastDate.applyTimeStyle()
  lastDate.font = Font.systemFont(9)
  lastDate.textColor = C.label

  footer.addSpacer(4)

  const updateRow = footer.addStack()
  updateRow.layoutHorizontally()
  updateRow.centerAlignContent()

  const updateLbl = updateRow.addText("Mis à jour il y a ")
  updateLbl.font = Font.systemFont(9)
  updateLbl.textColor = C.label

  const updateDate = updateRow.addDate(fetchDate)
  updateDate.applyRelativeStyle()
  updateDate.font = Font.systemFont(9)
  updateDate.textColor = C.label

  return widget
}

// ── MAIN ──────────────────────────────────────────
const result = await fetchGlucoseHistory()
const family = config.widgetFamily

let widget
if (family === "accessoryCircular") {
  widget = buildCircularWidget(result.latest)
} else if (family === "accessoryRectangular" || family === "accessoryInline") {
  widget = buildLockScreenWidget(result.latest)
} else if (family === "small") {
  widget = buildSmallWidget(result.latest)
} else if (family === "medium") {
  widget = buildMediumWidget(result.latest, result.points)
} else {
  widget = await buildLargeWidget(result)
}

if (config.runsInWidget) {
  Script.setWidget(widget)
} else {
  await widget.presentLarge()
}

Script.complete()
