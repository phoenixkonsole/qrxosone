// Draws the QRX wallpapers and the icons of the four desktop designs.
//   swift gen-assets.swift OUTDIR [png]
// OUTDIR/Wallpapers/<design>-<w>x<h>.qrxwp: "QRXW", width, height (big endian), then RGB bytes.
// OUTDIR/Icons/<theme>/<kind>.argb: "QRXI", width, height, then A R G B bytes, not premultiplied.
// With "png", a PNG beside each wallpaper for looking at.
import Foundation
import CoreGraphics
import CoreText
import ImageIO
import UniformTypeIdentifiers

let out = CommandLine.arguments[1]
let wantPNG = CommandLine.arguments.count > 2
let rgb = CGColorSpaceCreateDeviceRGB()

func color(_ r: Double, _ g: Double, _ b: Double, _ a: Double = 1) -> CGColor {
    CGColor(colorSpace: rgb, components: [r, g, b, a])!
}
func hex(_ v: UInt32, _ a: Double = 1) -> CGColor {
    color(Double((v >> 16) & 255) / 255, Double((v >> 8) & 255) / 255, Double(v & 255) / 255, a)
}
func context(_ w: Int, _ h: Int) -> CGContext {
    let c = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: rgb,
                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    c.setShouldAntialias(true)
    c.interpolationQuality = .high
    return c
}
struct RNG {
    var s: UInt64
    mutating func next() -> Double {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17
        return Double(s % 1_000_000) / 1_000_000
    }
}
func linear(_ c: CGContext, _ colors: [CGColor], _ a: CGPoint, _ b: CGPoint) {
    let g = CGGradient(colorsSpace: rgb, colors: colors as CFArray, locations: nil)!
    c.drawLinearGradient(g, start: a, end: b, options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
}
func radial(_ c: CGContext, _ colors: [CGColor], _ p: CGPoint, _ r: CGFloat) {
    let g = CGGradient(colorsSpace: rgb, colors: colors as CFArray, locations: nil)!
    c.drawRadialGradient(g, startCenter: p, startRadius: 0, endCenter: p, endRadius: r, options: [])
}
func text(_ c: CGContext, _ s: String, font: String, size: CGFloat, at p: CGPoint, color col: CGColor,
          tracking: CGFloat = 0, glow: CGColor? = nil, blur: CGFloat = 0) {
    let f = CTFontCreateWithName(font as CFString, size, nil)
    let attr: [CFString: Any] = [kCTFontAttributeName: f, kCTForegroundColorAttributeName: col, kCTKernAttributeName: tracking]
    let line = CTLineCreateWithAttributedString(CFAttributedStringCreate(nil, s as CFString, attr as CFDictionary)!)
    let w = CTLineGetTypographicBounds(line, nil, nil, nil)
    c.saveGState()
    if let glow = glow { c.setShadow(offset: .zero, blur: blur, color: glow) }
    c.textPosition = CGPoint(x: p.x - CGFloat(w) / 2, y: p.y)
    CTLineDraw(line, c)
    c.restoreGState()
}

let worlds: [(name: String, col: UInt32)] = [("PPC", 0xFF9A2E), ("x86", 0x2E9BFF), ("ARM", 0x2EFFB4), ("68K", 0xE24BFF)]

func starfield(_ c: CGContext, _ w: CGFloat, _ h: CGFloat, seed: UInt64, count: Int) {
    var r = RNG(s: seed)
    for _ in 0..<count {
        let x = r.next() * Double(w), y = r.next() * Double(h), s = 0.4 + r.next() * r.next() * 1.8
        c.setFillColor(color(0.8, 0.88, 1, 0.15 + r.next() * 0.6))
        c.fillEllipse(in: CGRect(x: x, y: y, width: s, height: s))
    }
}
/// Traces that leave a corner and run towards the middle in right angles and diagonals, like a board.
func traces(_ c: CGContext, from: CGPoint, to: CGPoint, col: UInt32, unit u: CGFloat, seed: UInt64, count: Int) {
    var r = RNG(s: seed)
    c.setLineCap(.round); c.setLineJoin(.round)
    for i in 0..<count {
        let spread = (Double(i) - Double(count) / 2) * Double(u) * 2.6
        let dx = to.x - from.x, dy = to.y - from.y, len = sqrt(dx * dx + dy * dy)
        let nx = -dy / len, ny = dx / len
        var p = CGPoint(x: from.x + nx * CGFloat(spread), y: from.y + ny * CGFloat(spread))
        let end = 0.45 + r.next() * 0.42
        let path = CGMutablePath(); path.move(to: p)
        var t = 0.0
        while t < end {
            let step = 0.05 + r.next() * 0.09
            t += step
            let tx = from.x + dx * CGFloat(t) + nx * CGFloat(spread * (1 - t)), ty = from.y + dy * CGFloat(t) + ny * CGFloat(spread * (1 - t))
            if r.next() < 0.5 { path.addLine(to: CGPoint(x: tx, y: p.y)) } else { path.addLine(to: CGPoint(x: p.x, y: ty)) }
            path.addLine(to: CGPoint(x: tx, y: ty))
            p = CGPoint(x: tx, y: ty)
        }
        let a = 0.25 + r.next() * 0.55
        c.saveGState()
        c.setShadow(offset: .zero, blur: u * 1.4, color: hex(col, 0.9))
        c.setStrokeColor(hex(col, a)); c.setLineWidth(max(1, u * (0.10 + 0.12 * r.next())))
        c.addPath(path); c.strokePath()
        c.setFillColor(hex(col, min(1, a + 0.3)))
        let n = u * 0.32
        c.fillEllipse(in: CGRect(x: p.x - n, y: p.y - n, width: n * 2, height: n * 2))
        c.restoreGState()
    }
}
func chipBadge(_ c: CGContext, at p: CGPoint, name: String, col: UInt32, u: CGFloat) {
    let s = u * 6.2
    let rect = CGRect(x: p.x - s / 2, y: p.y - s / 2, width: s, height: s)
    c.saveGState()
    c.setShadow(offset: .zero, blur: u * 2.4, color: hex(col, 0.9))
    c.setFillColor(hex(0x0A0E18, 0.92))
    c.addPath(CGPath(roundedRect: rect, cornerWidth: u * 0.8, cornerHeight: u * 0.8, transform: nil)); c.fillPath()
    c.setStrokeColor(hex(col, 1)); c.setLineWidth(max(1, u * 0.2))
    c.addPath(CGPath(roundedRect: rect, cornerWidth: u * 0.8, cornerHeight: u * 0.8, transform: nil)); c.strokePath()
    c.restoreGState()
    c.setStrokeColor(hex(col, 0.85)); c.setLineWidth(max(1, u * 0.14))
    for i in 0..<5 {
        let o = -s * 0.32 + CGFloat(i) * s * 0.16
        for (a, b) in [(CGPoint(x: p.x + o, y: p.y + s / 2), CGPoint(x: p.x + o, y: p.y + s / 2 + u * 0.8)),
                       (CGPoint(x: p.x + o, y: p.y - s / 2), CGPoint(x: p.x + o, y: p.y - s / 2 - u * 0.8)),
                       (CGPoint(x: p.x + s / 2, y: p.y + o), CGPoint(x: p.x + s / 2 + u * 0.8, y: p.y + o)),
                       (CGPoint(x: p.x - s / 2, y: p.y + o), CGPoint(x: p.x - s / 2 - u * 0.8, y: p.y + o))] {
            c.move(to: a); c.addLine(to: b); c.strokePath()
        }
    }
    text(c, name, font: "AvenirNext-Bold", size: u * 2.0, at: CGPoint(x: p.x, y: p.y - u * 0.7), color: hex(col), glow: hex(col, 0.9), blur: u * 0.8)
}
func wordmark(_ c: CGContext, center: CGPoint, u: CGFloat, tagline: Bool) {
    // Four colours along the wordmark, one world per letter stroke
    let size = u * 15
    let y = center.y - size * 0.34
    text(c, "QRX", font: "AvenirNext-Heavy", size: size, at: CGPoint(x: center.x, y: y), color: hex(0xFFFFFF, 0.0),
         tracking: u * 0.6, glow: hex(0x8FB8FF, 0.9), blur: u * 5)
    c.saveGState()
    let f = CTFontCreateWithName("AvenirNext-Heavy" as CFString, size, nil)
    let attr: [CFString: Any] = [kCTFontAttributeName: f, kCTKernAttributeName: u * 0.6]
    let line = CTLineCreateWithAttributedString(CFAttributedStringCreate(nil, "QRX" as CFString, attr as CFDictionary)!)
    let w = CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil))
    let mask = context(Int(w + u * 2), Int(size * 1.4))
    mask.setFillColor(gray: 1, alpha: 1)
    mask.textPosition = CGPoint(x: u, y: size * 0.34)
    let attr2: [CFString: Any] = [kCTFontAttributeName: f, kCTKernAttributeName: u * 0.6, kCTForegroundColorAttributeName: hex(0xFFFFFF)]
    CTLineDraw(CTLineCreateWithAttributedString(CFAttributedStringCreate(nil, "QRX" as CFString, attr2 as CFDictionary)!), mask)
    let img = mask.makeImage()!
    let rect = CGRect(x: center.x - w / 2 - u, y: y - size * 0.34, width: w + u * 2, height: size * 1.4)
    c.clip(to: rect, mask: img)
    linear(c, [hex(0xFF9A2E), hex(0xFFFFFF), hex(0x2E9BFF), hex(0xFFFFFF), hex(0x2EFFB4), hex(0xFFFFFF), hex(0xE24BFF)],
           CGPoint(x: rect.minX, y: 0), CGPoint(x: rect.maxX, y: 0))
    c.restoreGState()
    text(c, "FORMERLY KNOWN AS AEROS", font: "AvenirNext-Medium", size: u * 1.5, at: CGPoint(x: center.x, y: y - u * 2.6),
         color: hex(0xDDE7FF, 0.92), tracking: u * 0.42)
    if tagline {
        text(c, "THE WORLDS OF PPC  ·  x86  ·  ARM  ·  68K  COMBINED", font: "AvenirNext-DemiBold", size: u * 1.05,
             at: CGPoint(x: center.x, y: y - u * 4.8), color: hex(0x9FB4DA, 0.9), tracking: u * 0.28)
    }
}

/// The four worlds run from the corners into the core.
func designConvergence(_ c: CGContext, _ w: CGFloat, _ h: CGFloat) {
    let u = min(w, h) / 64
    linear(c, [hex(0x05070F), hex(0x0A1020), hex(0x04060C)], CGPoint(x: 0, y: h), CGPoint(x: 0, y: 0))
    starfield(c, w, h, seed: 88172645463325252, count: Int(w * h / 1800))
    let center = CGPoint(x: w * 0.54, y: h * 0.50)
    // Inside the corners: the drives sit on the left and the app panel on the right.
    let corners = [CGPoint(x: w * 0.20, y: h * 0.80), CGPoint(x: w * 0.80, y: h * 0.80), CGPoint(x: w * 0.20, y: h * 0.20), CGPoint(x: w * 0.80, y: h * 0.20)]
    for (i, p) in corners.enumerated() {
        let col = worlds[i].col
        c.saveGState(); c.setBlendMode(.screen)
        radial(c, [hex(col, 0.38), hex(col, 0.10), hex(col, 0)], p, max(w, h) * 0.52)
        c.restoreGState()
        traces(c, from: p, to: center, col: col, unit: u, seed: UInt64(i + 1) &* 0x9E3779B97F4A7C15 | 1, count: 15)
    }
    c.saveGState(); c.setBlendMode(.screen)
    radial(c, [hex(0xFFFFFF, 0.35), hex(0x6E8BFF, 0.18), hex(0x000000, 0)], center, u * 24)
    c.restoreGState()
    for (i, p) in corners.enumerated() { chipBadge(c, at: p, name: worlds[i].name, col: worlds[i].col, u: u) }
    wordmark(c, center: center, u: u, tagline: true)
}
/// A horizon with a grid floor and a sun made of the four colours.
func designHorizon(_ c: CGContext, _ w: CGFloat, _ h: CGFloat) {
    let u = min(w, h) / 64, horizon = h * 0.42
    linear(c, [hex(0x030310), hex(0x16103A), hex(0x4A1566)], CGPoint(x: 0, y: h), CGPoint(x: 0, y: horizon))
    starfield(c, w, h, seed: 1234567890123, count: Int(w * h / 2600))
    let sun = CGPoint(x: w * 0.54, y: horizon + u * 2)
    c.saveGState()
    c.addEllipse(in: CGRect(x: sun.x - u * 15, y: sun.y - u * 15, width: u * 30, height: u * 30)); c.clip()
    linear(c, [hex(0xE24BFF), hex(0x2E9BFF), hex(0x2EFFB4), hex(0xFF9A2E)], CGPoint(x: 0, y: sun.y + u * 15), CGPoint(x: 0, y: sun.y - u * 8))
    c.setFillColor(hex(0x16103A))
    var y = sun.y - u * 12, t = u * 0.2
    while y < sun.y + u * 3 { c.fill(CGRect(x: 0, y: y, width: w, height: t)); y += u * 1.4; t += u * 0.05 }
    c.restoreGState()
    c.saveGState(); c.setBlendMode(.screen)
    radial(c, [hex(0xFF70D0, 0.35), hex(0xFF70D0, 0)], sun, u * 30)
    c.restoreGState()
    c.saveGState()
    c.clip(to: CGRect(x: 0, y: 0, width: w, height: horizon))
    linear(c, [hex(0x0B0420), hex(0x020208)], CGPoint(x: 0, y: horizon), CGPoint(x: 0, y: 0))
    c.setShadow(offset: .zero, blur: u * 0.8, color: hex(0x2E9BFF))
    c.setStrokeColor(hex(0x38C8FF, 0.75)); c.setLineWidth(max(1, u * 0.09))
    for i in -22...22 {
        c.move(to: CGPoint(x: sun.x + CGFloat(i) * u * 0.9, y: horizon))
        c.addLine(to: CGPoint(x: sun.x + CGFloat(i) * u * 15, y: -u))
        c.strokePath()
    }
    var d: CGFloat = 1
    while d < 40 {
        let yy = horizon - horizon * (d * d) / 1600
        c.move(to: CGPoint(x: 0, y: yy)); c.addLine(to: CGPoint(x: w, y: yy)); c.strokePath()
        d += 2.2
    }
    c.restoreGState()
    c.setShadow(offset: .zero, blur: 0, color: nil)
    for (i, world) in worlds.enumerated() {
        let x = w * 0.54 + (CGFloat(i) - 1.5) * u * 11
        text(c, world.name, font: "AvenirNext-Bold", size: u * 2.0, at: CGPoint(x: x, y: horizon - u * 4), color: hex(world.col), glow: hex(world.col), blur: u * 1.4)
    }
    wordmark(c, center: CGPoint(x: w * 0.54, y: h * 0.76), u: u * 0.9, tagline: false)
    text(c, "THE WORLDS OF PPC  ·  x86  ·  ARM  ·  68K  COMBINED", font: "AvenirNext-DemiBold", size: u * 1.0,
         at: CGPoint(x: w * 0.54, y: u * 3.4), color: hex(0xBCD0F0, 0.85), tracking: u * 0.28)
}
/// Quiet: large soft fields of the four colours and a small mark.
func designAurora(_ c: CGContext, _ w: CGFloat, _ h: CGFloat) {
    let u = min(w, h) / 64
    c.setFillColor(hex(0x070A12)); c.fill(CGRect(x: 0, y: 0, width: w, height: h))
    var r = RNG(s: 42424242424242)
    c.saveGState(); c.setBlendMode(.screen)
    for pass in 0..<36 {
        let col = worlds[pass % 4].col
        let band = CGFloat(pass % 4) / 4
        let x = (band + CGFloat(r.next()) * 0.3 - 0.05) * w
        let y = h * (0.15 + CGFloat(r.next()) * 0.75)
        radial(c, [hex(col, 0.13), hex(col, 0)], CGPoint(x: x, y: y), u * CGFloat(14 + r.next() * 22))
    }
    c.restoreGState()
    c.setStrokeColor(hex(0xFFFFFF, 0.05)); c.setLineWidth(1)
    var i: CGFloat = 0
    while i < w + h { c.move(to: CGPoint(x: i, y: 0)); c.addLine(to: CGPoint(x: i - h, y: h)); c.strokePath(); i += u * 2 }
    let s = u * 5
    text(c, "QRX", font: "AvenirNext-Heavy", size: s, at: CGPoint(x: w - u * 10, y: u * 6.2), color: hex(0xFFFFFF, 0.92), tracking: u * 0.2, glow: hex(0x8FB8FF, 0.8), blur: u * 1.2)
    text(c, "FORMERLY KNOWN AS AEROS", font: "AvenirNext-Medium", size: u * 0.72, at: CGPoint(x: w - u * 10, y: u * 4.4), color: hex(0xDDE7FF, 0.8), tracking: u * 0.16)
    text(c, "PPC · x86 · ARM · 68K", font: "AvenirNext-DemiBold", size: u * 0.78, at: CGPoint(x: w - u * 10, y: u * 3.0), color: hex(0x9FB4DA, 0.8), tracking: u * 0.1)
}

func writeWallpaper(_ name: String, _ w: Int, _ h: Int, _ draw: (CGContext, CGFloat, CGFloat) -> Void) throws {
    let c = context(w, h)
    draw(c, CGFloat(w), CGFloat(h))
    var data = Data("QRXW".utf8)
    for v in [UInt32(w), UInt32(h)] { withUnsafeBytes(of: v.bigEndian) { data.append(contentsOf: $0) } }
    let p = c.data!.assumingMemoryBound(to: UInt8.self)
    var row = [UInt8](repeating: 0, count: w * 3)
    for y in 0..<h {
        for x in 0..<w {
            let o = (y * w + x) * 4
            row[x * 3] = p[o]; row[x * 3 + 1] = p[o + 1]; row[x * 3 + 2] = p[o + 2]
        }
        data.append(contentsOf: row)
    }
    let dir = out + "/Wallpapers"
    try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
    try data.write(to: URL(fileURLWithPath: "\(dir)/\(name)-\(w)x\(h).qrxwp"))
    if wantPNG {
        let d = CGImageDestinationCreateWithURL(URL(fileURLWithPath: "\(dir)/\(name)-\(w)x\(h).png") as CFURL, UTType.png.identifier as CFString, 1, nil)!
        CGImageDestinationAddImage(d, c.makeImage()!, nil)
        CGImageDestinationFinalize(d)
    }
}

// ---- icons: drawn at 4x and reduced, in a unit square 48 high
enum Kind: String, CaseIterable { case drawer, disk, ram, apps, file, program, windows }

func roundRect(_ c: CGContext, _ r: CGRect, _ radius: CGFloat) {
    c.addPath(CGPath(roundedRect: r, cornerWidth: radius, cornerHeight: radius, transform: nil))
}
func fill(_ c: CGContext, _ r: CGRect, _ radius: CGFloat, _ top: CGColor, _ bottom: CGColor) {
    c.saveGState(); roundRect(c, r, radius); c.clip()
    linear(c, [top, bottom], CGPoint(x: 0, y: r.maxY), CGPoint(x: 0, y: r.minY))
    c.restoreGState()
}
func stroke(_ c: CGContext, _ r: CGRect, _ radius: CGFloat, _ col: CGColor, _ w: CGFloat) {
    c.setStrokeColor(col); c.setLineWidth(w); roundRect(c, r, radius); c.strokePath()
}
struct Palette { let a: UInt32, b: UInt32, line: UInt32, paper: UInt32, accent: UInt32, radius: CGFloat, width: CGFloat, glow: Bool }
let themes: [String: Palette] = [
    "qrx":     Palette(a: 0x12203C, b: 0x0A1020, line: 0x37E6C8, paper: 0x18263F, accent: 0xE24BFF, radius: 5, width: 1.6, glow: true),
    "windows": Palette(a: 0xFFD966, b: 0xE8A91C, line: 0x5A4210, paper: 0xFFFFFF, accent: 0x1E6FD9, radius: 1, width: 1.2, glow: false),
    "mac":     Palette(a: 0x8CC8FF, b: 0x3A8EEB, line: 0x1F5FB0, paper: 0xF7F9FC, accent: 0x3A8EEB, radius: 7, width: 0.9, glow: false),
    "amiga":   Palette(a: 0x6E8FD0, b: 0x2F5AA8, line: 0x0A1A3A, paper: 0xE9E9E9, accent: 0xFF8A1E, radius: 0, width: 1.8, glow: false),
]
func drawIcon(_ c: CGContext, _ kind: Kind, _ t: Palette) {
    let pa = hex(t.a), pb = hex(t.b), line = hex(t.line), paper = hex(t.paper), accent = hex(t.accent)
    if t.glow { c.setShadow(offset: .zero, blur: 3, color: hex(t.line, 0.8)) }
    else { c.setShadow(offset: CGSize(width: 0, height: -1), blur: 1.6, color: color(0, 0, 0, 0.35)) }
    switch kind {
    case .drawer:
        // A folder with a tab. Amiga's is a drawer with a handle.
        if t.radius == 0 {
            fill(c, CGRect(x: 5, y: 8, width: 38, height: 30), 0, pa, pb)
            c.setShadow(offset: .zero, blur: 0, color: nil)
            stroke(c, CGRect(x: 5, y: 8, width: 38, height: 30), 0, line, t.width)
            c.setFillColor(paper); c.fill(CGRect(x: 8, y: 25, width: 32, height: 10))
            stroke(c, CGRect(x: 8, y: 25, width: 32, height: 10), 0, line, 1)
            c.setFillColor(accent); c.fill(CGRect(x: 19, y: 28, width: 10, height: 4))
            c.setFillColor(paper); c.fill(CGRect(x: 8, y: 11, width: 32, height: 10))
            stroke(c, CGRect(x: 8, y: 11, width: 32, height: 10), 0, line, 1)
            c.setFillColor(accent); c.fill(CGRect(x: 19, y: 14, width: 10, height: 4))
        } else {
            let tab = CGMutablePath()
            tab.move(to: CGPoint(x: 5, y: 32)); tab.addLine(to: CGPoint(x: 5, y: 39))
            tab.addLine(to: CGPoint(x: 19, y: 39)); tab.addLine(to: CGPoint(x: 23, y: 34)); tab.addLine(to: CGPoint(x: 43, y: 34)); tab.addLine(to: CGPoint(x: 43, y: 30))
            c.setFillColor(pb); c.addPath(tab); c.fillPath()
            if t.glow { c.setStrokeColor(line); c.setLineWidth(t.width); c.addPath(tab); c.strokePath() }
            fill(c, CGRect(x: 4, y: 7, width: 40, height: 27), t.radius, pa, pb)
            c.setShadow(offset: .zero, blur: 0, color: nil)
            stroke(c, CGRect(x: 4, y: 7, width: 40, height: 27), t.radius, line, t.width)
            if t.glow { c.setFillColor(hex(0xE24BFF)); c.fill(CGRect(x: 10, y: 12, width: 28, height: 2)) }
            else { c.setFillColor(color(1, 1, 1, 0.35)); c.fill(CGRect(x: 7, y: 29, width: 34, height: 2)) }
        }
    case .disk, .ram, .apps:
        fill(c, CGRect(x: 5, y: 12, width: 38, height: 25), t.radius, kind == .ram ? hex(t.b) : hex(t.paper == 0xFFFFFF ? 0xD8D8D8 : t.a), kind == .ram ? hex(t.a) : hex(t.paper == 0xFFFFFF ? 0x9A9A9A : t.b))
        c.setShadow(offset: .zero, blur: 0, color: nil)
        stroke(c, CGRect(x: 5, y: 12, width: 38, height: 25), t.radius, line, t.width)
        if kind == .ram {
            c.setFillColor(accent)
            for i in 0..<6 { c.fill(CGRect(x: 9 + CGFloat(i) * 5.2, y: 19, width: 3.4, height: 12)) }
            c.setFillColor(line)
            for i in 0..<8 { c.fill(CGRect(x: 8 + CGFloat(i) * 4.4, y: 9, width: 2.2, height: 3)) }
        } else {
            if kind == .disk {
                c.setFillColor(hex(t.line, 0.75))
                c.fill(CGRect(x: 9, y: 29, width: 18, height: 1.2)); c.fill(CGRect(x: 9, y: 25, width: 18, height: 1.2))
            }
            c.setFillColor(kind == .apps ? hex(0xFF9A2E) : accent)
            c.fillEllipse(in: CGRect(x: 34, y: 17, width: 5, height: 5))
            if kind == .apps {
                // A grid of four: the worlds the apps come from
                let cols: [UInt32] = [0xFF9A2E, 0x2E9BFF, 0x2EFFB4, 0xE24BFF]
                for (i, v) in cols.enumerated() {
                    c.setFillColor(hex(v))
                    c.fill(CGRect(x: 10 + CGFloat(i % 2) * 7, y: 16 + CGFloat(i / 2) * 7, width: 5.5, height: 5.5))
                }
            }
        }
    case .file:
        let p = CGMutablePath()
        p.move(to: CGPoint(x: 11, y: 5)); p.addLine(to: CGPoint(x: 37, y: 5)); p.addLine(to: CGPoint(x: 37, y: 33))
        p.addLine(to: CGPoint(x: 28, y: 43)); p.addLine(to: CGPoint(x: 11, y: 43)); p.closeSubpath()
        c.setFillColor(paper); c.addPath(p); c.fillPath()
        c.setShadow(offset: .zero, blur: 0, color: nil)
        c.setStrokeColor(line); c.setLineWidth(t.width); c.addPath(p); c.strokePath()
        c.setFillColor(hex(t.line, 0.45))
        for i in 0..<5 { c.fill(CGRect(x: 15, y: 10 + CGFloat(i) * 5, width: 18, height: 1.6)) }
        c.setFillColor(accent)
        let f = CGMutablePath(); f.move(to: CGPoint(x: 28, y: 43)); f.addLine(to: CGPoint(x: 28, y: 33)); f.addLine(to: CGPoint(x: 37, y: 33)); f.closeSubpath()
        c.addPath(f); c.fillPath()
    case .program, .windows:
        fill(c, CGRect(x: 5, y: 7, width: 38, height: 34), t.radius, hex(t.paper == 0xFFFFFF ? 0xF4F4F4 : t.a), hex(t.paper == 0xFFFFFF ? 0xC8C8C8 : t.b))
        c.setShadow(offset: .zero, blur: 0, color: nil)
        stroke(c, CGRect(x: 5, y: 7, width: 38, height: 34), t.radius, line, t.width)
        c.setFillColor(accent); c.fill(CGRect(x: 6, y: 33, width: 36, height: 7))
        c.setFillColor(paper); c.fill(CGRect(x: 36, y: 35, width: 3, height: 3)); c.fill(CGRect(x: 31, y: 35, width: 3, height: 3))
        if kind == .windows {
            let cols: [UInt32] = [0x2E9BFF, 0xFF9A2E, 0x2EFFB4, 0xE24BFF]
            for (i, v) in cols.enumerated() {
                c.setFillColor(hex(v))
                c.fill(CGRect(x: 17 + CGFloat(i % 2) * 8, y: 13 + CGFloat(i / 2) * 8, width: 6.5, height: 6.5))
            }
        } else {
            c.setStrokeColor(t.paper == 0xFFFFFF ? hex(0x303030) : line); c.setLineWidth(2); c.setLineCap(.round)
            c.move(to: CGPoint(x: 13, y: 26)); c.addLine(to: CGPoint(x: 19, y: 21)); c.addLine(to: CGPoint(x: 13, y: 16)); c.strokePath()
            c.move(to: CGPoint(x: 22, y: 15)); c.addLine(to: CGPoint(x: 31, y: 15)); c.strokePath()
        }
    }
}
func writeIcon(theme: String, kind: Kind, size: Int = 48) throws {
    let scale = 4
    let big = context(size * scale, size * scale)
    big.scaleBy(x: CGFloat(scale) * CGFloat(size) / 48, y: CGFloat(scale) * CGFloat(size) / 48)
    drawIcon(big, kind, themes[theme]!)
    let small = context(size, size)
    small.interpolationQuality = .high
    small.draw(big.makeImage()!, in: CGRect(x: 0, y: 0, width: size, height: size))
    let p = small.data!.assumingMemoryBound(to: UInt8.self)
    var data = Data("QRXI".utf8)
    for v in [UInt32(size), UInt32(size)] { withUnsafeBytes(of: v.bigEndian) { data.append(contentsOf: $0) } }
    for i in 0..<(size * size) {
        let o = i * 4, a = Int(p[o + 3])
        func un(_ v: UInt8) -> UInt8 { a == 0 ? 0 : UInt8(min(255, Int(v) * 255 / a)) }
        data.append(contentsOf: [UInt8(a), un(p[o]), un(p[o + 1]), un(p[o + 2])])
    }
    let dir = "\(out)/Icons/\(theme)"
    try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
    try data.write(to: URL(fileURLWithPath: "\(dir)/\(kind.rawValue).argb"))
}

let sizes = [(1024, 768), (1280, 720), (1280, 1024), (1920, 1080)]
for (w, h) in sizes {
    try writeWallpaper("Convergence", w, h, designConvergence)
    try writeWallpaper("Horizon", w, h, designHorizon)
    try writeWallpaper("Aurora", w, h, designAurora)
}
for t in themes.keys { for k in Kind.allCases { try writeIcon(theme: t, kind: k) } }
// A sheet of all icons, for looking at
if wantPNG {
    let names = ["qrx", "windows", "mac", "amiga"], cell = 96
    let sheet = context(cell * Kind.allCases.count, cell * names.count)
    sheet.setFillColor(hex(0x24304A)); sheet.fill(CGRect(x: 0, y: 0, width: cell * Kind.allCases.count, height: cell * names.count))
    for (r, n) in names.enumerated() { for (i, k) in Kind.allCases.enumerated() {
        sheet.saveGState()
        sheet.translateBy(x: CGFloat(i * cell + 0), y: CGFloat(r * cell))
        sheet.scaleBy(x: 2, y: 2)
        drawIcon(sheet, k, themes[n]!)
        sheet.restoreGState()
    } }
    let d = CGImageDestinationCreateWithURL(URL(fileURLWithPath: "\(out)/Icons/sheet.png") as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(d, sheet.makeImage()!, nil)
    CGImageDestinationFinalize(d)
}
print("done")
