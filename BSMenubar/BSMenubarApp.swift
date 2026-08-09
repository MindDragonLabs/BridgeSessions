import Cocoa

// MARK: - App Delegate

@main
class AppDelegate: NSObject, NSApplicationDelegate {
    var statusController: StatusItemController!
    var fleetController: FleetStatusController!
    var settingsController: SettingsController!
    var helperManager: HelperProcessManager!

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Suppress crash dialog on child process issues
        NSApp.setActivationPolicy(.accessory)

        helperManager = HelperProcessManager()
        settingsController = SettingsController(helperManager: helperManager)
        fleetController = FleetStatusController()
        statusController = StatusItemController(
            fleetController: fleetController,
            settingsController: settingsController,
            helperManager: helperManager
        )

        // Start the cua-helper child process
        helperManager.start()

        // Start fleet polling
        fleetController.startPolling()

        // Apply autolaunch setting
        settingsController.applyLaunchAgentState()

        NSLog("[BSMenubar] Launched — helper=%@ fleet=%@",
              helperManager.isRunning ? "running" : "stopped",
              fleetController.isPolling ? "polling" : "idle")
    }

    func applicationWillTerminate(_ notification: Notification) {
        fleetController.stopPolling()
        helperManager.stop()
    }
}
