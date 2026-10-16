// Renders the FSMac app icon into an .iconset directory.
//   swift scripts/make-icon.swift build/AppIcon.iconset && iconutil -c icns build/AppIcon.iconset

import AppKit

let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "AppIcon.iconset"
try? FileManager.default.createDirectory(atPath: out, withIntermediateDirectories: true)

func color(_ hex: UInt32, _ a: CGFloat = 1) -> NSColor {
    NSColor(srgbRed: CGFloat((hex >> 16) & 0xff) / 255, green: CGFloat((hex >> 8) & 0xff) / 255,
            blue: CGFloat(hex & 0xff) / 255, alpha: a)
}

func render(_ px: Int) -> NSBitmapImageRep {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8,
                               samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                               colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    let ctx = NSGraphicsContext(bitmapImageRep: rep)!
    NSGraphicsContext.current = ctx
    let cg = ctx.cgContext
    let s = CGFloat(px) / 1024
    cg.scaleBy(x: s, y: s)

    // Background tile (macOS icon grid: 824 pt tile centred in 1024).
    let tile = CGRect(x: 100, y: 100, width: 824, height: 824)
    let tilePath = NSBezierPath(roundedRect: tile, xRadius: 185, yRadius: 185)
    cg.saveGState()
    cg.setShadow(offset: CGSize(width: 0, height: -10), blur: 24, color: color(0x000000, 0.35).cgColor)
    color(0x0B1F4B).setFill()
    tilePath.fill()
    cg.restoreGState()
    cg.saveGState()
    tilePath.addClip()
    NSGradient(colors: [color(0x0F2A6B), color(0x1D5FD1), color(0x22B8CF)],
               atLocations: [0, 0.55, 1], colorSpace: .sRGB)!
        .draw(in: tilePath, angle: 60)
    // Soft highlight across the top.
    NSGradient(starting: color(0xFFFFFF, 0.18), ending: color(0xFFFFFF, 0))!
        .draw(in: NSRect(x: 100, y: 560, width: 824, height: 364), angle: -90)
    cg.restoreGState()

    // Artwork (arrows over the drive), centred vertically in the tile: it
    // spans y = 330...839, so its middle moves from 584 to 512, a touch above
    // centre for optical balance.
    cg.saveGState()
    cg.translateBy(x: 0, y: -66)

    // Drive body.
    let body = CGRect(x: 232, y: 330, width: 560, height: 300)
    cg.saveGState()
    cg.setShadow(offset: CGSize(width: 0, height: -14), blur: 30, color: color(0x000000, 0.4).cgColor)
    let bodyPath = NSBezierPath(roundedRect: body, xRadius: 58, yRadius: 58)
    NSGradient(starting: color(0xF8FAFC), ending: color(0xCBD5E1))!.draw(in: bodyPath, angle: -90)
    cg.restoreGState()
    // Front panel.
    let panel = CGRect(x: 268, y: 362, width: 488, height: 92)
    NSGradient(starting: color(0x334155), ending: color(0x1E293B))!
        .draw(in: NSBezierPath(roundedRect: panel, xRadius: 30, yRadius: 30), angle: -90)
    // Activity lights: one per filesystem.
    for (i, c) in [color(0x38BDF8), color(0x4ADE80)].enumerated() {
        let r = CGRect(x: 650 + CGFloat(i) * 46, y: 393, width: 30, height: 30)
        cg.saveGState()
        cg.setShadow(offset: .zero, blur: 18, color: c.cgColor)
        c.setFill()
        NSBezierPath(ovalIn: r).fill()
        cg.restoreGState()
    }
    // Vent lines on the top.
    color(0x94A3B8, 0.7).setFill()
    for i in 0..<5 {
        NSBezierPath(roundedRect: CGRect(x: 300 + CGFloat(i) * 34, y: 520, width: 14, height: 70),
                     xRadius: 7, yRadius: 7).fill()
    }

    // Two-way arrows (read & write) above the drive.
    let arrow = NSBezierPath()
    arrow.lineWidth = 34
    arrow.lineCapStyle = .round
    arrow.lineJoinStyle = .round
    // Up arrow.
    arrow.move(to: CGPoint(x: 452, y: 690)); arrow.line(to: CGPoint(x: 452, y: 820))
    arrow.move(to: CGPoint(x: 404, y: 770)); arrow.line(to: CGPoint(x: 452, y: 822))
    arrow.line(to: CGPoint(x: 500, y: 770))
    // Down arrow.
    arrow.move(to: CGPoint(x: 572, y: 822)); arrow.line(to: CGPoint(x: 572, y: 692))
    arrow.move(to: CGPoint(x: 524, y: 742)); arrow.line(to: CGPoint(x: 572, y: 690))
    arrow.line(to: CGPoint(x: 620, y: 742))
    color(0xFFFFFF, 0.95).setStroke()
    arrow.stroke()
    cg.restoreGState()

    NSGraphicsContext.restoreGraphicsState()
    return rep
}

let sizes: [(String, Int)] = [
    ("icon_16x16", 16), ("icon_16x16@2x", 32), ("icon_32x32", 32), ("icon_32x32@2x", 64),
    ("icon_128x128", 128), ("icon_128x128@2x", 256), ("icon_256x256", 256),
    ("icon_256x256@2x", 512), ("icon_512x512", 512), ("icon_512x512@2x", 1024),
]
for (name, px) in sizes {
    let data = render(px).representation(using: .png, properties: [:])!
    try! data.write(to: URL(fileURLWithPath: "\(out)/\(name).png"))
}
