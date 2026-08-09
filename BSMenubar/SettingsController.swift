import Cocoa
import ApplicationServices

// MARK: - Settings Controller (autolaunch, permissions)

final class SettingsController {
    private let helperManager: HelperProcessManager

    private let launchAgentLabel = "com.minddragon.bridgesessions.menubar"
    private var launchAgentPath: String {
        NSHomeDirectory() + "/Library/LaunchAgents/\(launchAgentLabel).plist"
    }

    init(helperManager: HelperProcessManager) {
        self.helperManager = helperManager
    }

    // MARK: - Launch at Login (LaunchAgent)

    var isAutolaunchEnabled: Bool {
        FileManager.default.fileExists(atPath: launchAgentPath)
    }

    func toggleAutolaunch() {
        if isAutolaunchEnabled {
            disableAutolaunch()
        } else {
            enableAutolaunch()
        }
    }

    func enableAutolaunch() {
        let dir = (launchAgentPath as NSString).deletingLastPathComponent
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)

        // Path to this running .app bundle
        let appPath = Bundle.main.bundlePath
        let plist = """
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0">
        <dict>
          <key>Label</key>
          <string>\(launchAgentLabel)</string>
          <key>ProgramArguments</key>
          <array>
            <string>open</string>
            <string>-a</string>
            <string>\(appPath)</string>
          </array>
          <key>RunAtLoad</key>
          <true/>
          <key>KeepAlive</key>
          <false/>
        </dict>
        </plist>
        """
        do {
            try plist.write(toFile: launchAgentPath, atomically: true, encoding: .utf8)
        } catch {
            NSLog("[BSMenubar] Failed to write LaunchAgent: %@", error.localizedDescription)
        }
    }

    func disableAutolaunch() {
        try? FileManager.default.removeItem(atPath: launchAgentPath)
    }

    /// Call at launch to sync the loaded LaunchAgent with the plist on disk.
    func applyLaunchAgentState() {
        // No-op: LaunchAgent is loaded by launchd at login automatically when RunAtLoad=true.
        // We only need the plist to exist on disk.
    }

    // MARK: - Permissions

    /// Checks Screen Recording + Accessibility. Opens System Settings if missing.
    func checkPermissions() {
        let screenOK = checkScreenRecordingPermission()
        let axOK = AXIsProcessTrusted()

        NSLog("[BSMenubar] Permissions: Screen=%@ AX=%@",
              screenOK ? "granted" : "missing",
              axOK ? "granted" : "missing")

        if !screenOK {
            openSystemSettings(panel: "Privacy_ScreenCapture")
        }
        if !axOK {
            // Prompt for Accessibility (shows the system dialog)
            let opts = kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String
            let options: NSDictionary = [opts: true]
            _ = AXIsProcessTrustedWithOptions(options)
        }

        // Show a summary notification
        showPermissionSummary(screenOK: screenOK, axOK: axOK)
    }

    /// Attempt a screencapture to detect Screen Recording permission.
    private func checkScreenRecordingPermission() -> Bool {
        let tmpPath = "/tmp/bs-menubar-perm-check.png"
        try? FileManager.default.removeItem(atPath: tmpPath)

        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/usr/sbin/screencapture")
        task.arguments = ["-x", tmpPath]
        do { try task.run() } catch { return false }
        task.waitUntilExit()

        guard task.terminationStatus == 0,
              let img = NSImage(contentsOfFile: tmpPath) else {
            return false
        }
        let valid = img.size.width > 1 && img.size.height > 1
        try? FileManager.default.removeItem(atPath: tmpPath)
        return valid
    }

    private func openSystemSettings(panel: String) {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?\(panel)") {
            NSWorkspace.shared.open(url)
        }
    }

    private func showPermissionSummary(screenOK: Bool, axOK: Bool) {
        DispatchQueue.main.async {
            let alert = NSAlert()
            alert.messageText = "Permission Status"
            alert.informativeText = """
                Screen Recording: \(screenOK ? "✓ Granted" : "✗ Missing — System Settings opened")
                Accessibility: \(axOK ? "✓ Granted" : "✗ Missing — grant in System Settings")
                """
            alert.alertStyle = screenOK && axOK ? .informational : .warning
            alert.addButton(withTitle: "OK")
            // Use runModal for accessory apps (no main window for sheet attachment)
            NSApp.activate(ignoringOtherApps: true)
            alert.runModal()
        }
    }
}
