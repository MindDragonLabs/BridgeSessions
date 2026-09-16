// gen-icon.swift — render the BridgeSessions "B" app icon at all iconset sizes.
// Usage: swift gen-icon.swift <output-iconset-dir>
// Offline: AppKit + CoreGraphics only, no network, no assets.
import AppKit

let outDir = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "AppIcon.iconset"
try? FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true)

let sizes: [(Int, String)] = [
    (16, "icon_16x16.png"), (32, "icon_16x16@2x.png"),
    (32, "icon_32x32.png"), (64, "icon_32x32@2x.png"),
    (128, "icon_128x128.png"), (256, "icon_128x128@2x.png"),
    (256, "icon_256x256.png"), (512, "icon_256x256@2x.png"),
    (512, "icon_512x512.png"), (1024, "icon_512x512@2x.png"),
]

func drawIcon(_ px: CGFloat) -> NSImage {
    let img = NSImage(size: NSSize(width: px, height: px))
    img.lockFocus()
    // Dark navy gradient, full bleed (macOS crops to rounded corners).
    let colors = [NSColor(calibratedRed: 0.09, green: 0.13, blue: 0.24, alpha: 1).cgColor,
                  NSColor(calibratedRed: 0.05, green: 0.07, blue: 0.15, alpha: 1).cgColor]
    if let grad = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                             colors: colors as CFArray, locations: [0, 1]) {
        let ctx = NSGraphicsContext.current!.cgContext
        ctx.drawLinearGradient(grad, start: CGPoint(x: px / 2, y: px),
                               end: CGPoint(x: px / 2, y: 0), options: [])
    }
    // Bold white "B", centered.
    let font = NSFont.boldSystemFont(ofSize: px * 0.68)
    let attrs: [NSAttributedString.Key: Any] = [
        .font: font,
        .foregroundColor: NSColor.white,
    ]
    let str = NSAttributedString(string: "B", attributes: attrs)
    let bSize = str.size()
    str.draw(at: NSPoint(x: (px - bSize.width) / 2,
                         y: (px - bSize.height) / 2 - px * 0.02))
    img.unlockFocus()
    return img
}

for (px, name) in sizes {
    let img = drawIcon(CGFloat(px))
    let tiff = img.tiffRepresentation!
    let rep = NSBitmapImageRep(data: tiff)!
    let png = rep.representation(using: .png, properties: [:])!
    try! png.write(to: URL(fileURLWithPath: "\(outDir)/\(name)"))
}
print("iconset written to \(outDir)")
