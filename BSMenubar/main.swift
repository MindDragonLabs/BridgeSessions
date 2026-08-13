import Cocoa

// Explicit entry point. swiftc @main + NSApplicationMain does not
// instantiate the AppDelegate unless Info.plist has NSPrincipalClass,
// so the status item never appears. Keep a process-lifetime retain.
private let appDelegate = AppDelegate()

autoreleasepool {
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    app.delegate = appDelegate
    app.run()
}
