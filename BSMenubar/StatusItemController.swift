import Cocoa
import Darwin

// MARK: - Menubar Status Item + Menu

final class StatusItemController: NSObject {
    private let statusItem: NSStatusItem
    private let fleetController: FleetStatusController
    private let settingsController: SettingsController
    private let helperManager: HelperProcessManager

    private var popover: NSPopover?
    private var monitorTimer: Timer?

    init(fleetController: FleetStatusController,
         settingsController: SettingsController,
         helperManager: HelperProcessManager) {
        self.fleetController = fleetController
        self.settingsController = settingsController
        self.helperManager = helperManager
        self.statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        super.init()
        configureButton()
        rebuildMenu()
        // Refresh menu state every 2s so helper status is live
        monitorTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            self?.rebuildMenu()
        }
    }

    // Draw "B" as attributed text (no asset dependency)
    private func configureButton() {
        guard let button = statusItem.button else { return }
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.menuBarFont(ofSize: 16),
            .foregroundColor: NSColor.labelColor
        ]
        button.attributedTitle = NSAttributedString(string: "B", attributes: attrs)
        button.toolTip = "BridgeSessions Helper"
    }

    // Status line for helper process
    private var helperStatusText: String {
        if helperManager.isRunning {
            return "● Helper Running"
        } else if helperManager.isRestarting {
            return "◌ Helper Restarting…"
        } else {
            return "○ Helper Stopped"
        }
    }

    private func rebuildMenu() {
        let menu = NSMenu()

        // Header: helper status (non-selectable)
        let statusItem = NSMenuItem(title: helperStatusText, action: nil, keyEquivalent: "")
        statusItem.isEnabled = false
        menu.addItem(statusItem)

        menu.addItem(.separator())

        // Fleet Status popup
        let fleetItem = NSMenuItem(title: "Fleet Status…", action: #selector(showFleetPopover), keyEquivalent: "")
        fleetItem.target = self
        menu.addItem(fleetItem)

        menu.addItem(.separator())

        // Settings submenu
        let settingsMenu = NSMenu()
        settingsMenu.title = "Settings"

        // Launch at Login toggle
        let launchItem = NSMenuItem(title: "Launch at Login", action: #selector(toggleAutolaunch), keyEquivalent: "")
        launchItem.target = self
        launchItem.state = settingsController.isAutolaunchEnabled ? .on : .off
        settingsMenu.addItem(launchItem)

        settingsMenu.addItem(.separator())

        // Check Permissions
        let permItem = NSMenuItem(title: "Check Permissions…", action: #selector(checkPermissions), keyEquivalent: "")
        permItem.target = self
        settingsMenu.addItem(permItem)

        // Restart Helper
        let restartItem = NSMenuItem(title: "Restart Helper", action: #selector(restartHelper), keyEquivalent: "")
        restartItem.target = self
        settingsMenu.addItem(restartItem)

        let settingsMenuItem = NSMenuItem(title: "Settings", action: nil, keyEquivalent: "")
        settingsMenuItem.submenu = settingsMenu
        menu.addItem(settingsMenuItem)

        menu.addItem(.separator())

        // Restart mesh daemon (launchd) — parity with Windows/Linux tray
        let restartDaemon = NSMenuItem(title: "Restart Daemon", action: #selector(restartDaemon), keyEquivalent: "")
        restartDaemon.target = self
        menu.addItem(restartDaemon)

        // Open logs folder
        let openLogs = NSMenuItem(title: "Open Logs…", action: #selector(openLogs), keyEquivalent: "")
        openLogs.target = self
        menu.addItem(openLogs)

        menu.addItem(.separator())

        // Quit
        let quitItem = NSMenuItem(title: "Quit BridgeSessions Helper", action: #selector(quit), keyEquivalent: "q")
        quitItem.target = self
        menu.addItem(quitItem)

        self.statusItem.menu = menu
    }

    // MARK: - Actions

    @objc private func showFleetPopover() {
        guard let button = statusItem.button else { return }
        if popover == nil {
            let pop = NSPopover()
            pop.behavior = .transient
            pop.contentViewController = fleetController.viewController
            pop.contentViewController?.view.frame.size = NSSize(width: 440, height: 280)
            self.popover = pop
        }
        popover?.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
    }

    @objc private func toggleAutolaunch() {
        settingsController.toggleAutolaunch()
        rebuildMenu()
    }

    @objc private func checkPermissions() {
        settingsController.checkPermissions()
    }

    @objc private func restartHelper() {
        helperManager.restart()
    }

    @objc private func restartDaemon() {
        let uid = getuid()
        let label = "gui/\(uid)/com.bridgesessions.mesh"
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        task.arguments = ["kickstart", "-k", label]
        task.standardOutput = FileHandle.nullDevice
        task.standardError = FileHandle.nullDevice
        do {
            try task.run()
            task.waitUntilExit()
            NSLog("[BSMenubar] Restart Daemon: launchctl kickstart %@ (status=%d)", label, task.terminationStatus)
        } catch {
            NSLog("[BSMenubar] Restart Daemon failed: %@", error.localizedDescription)
        }
    }

    @objc private func openLogs() {
        let home = NSHomeDirectory()
        let candidates = [
            home + "/.bridgesessions",
            home + "/Library/Logs/BridgeSessions",
            "/tmp"
        ]
        let fm = FileManager.default
        let path = candidates.first(where: { fm.fileExists(atPath: $0) }) ?? home + "/.bridgesessions"
        NSWorkspace.shared.open(URL(fileURLWithPath: path))
    }

    @objc private func quit() {
        NSApp.terminate(nil)
    }
}
