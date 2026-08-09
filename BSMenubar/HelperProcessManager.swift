import Foundation

// MARK: - Helper Process Manager (cua-helper lifecycle)

final class HelperProcessManager {
    private var process: Process?
    private var restartWorkItem: DispatchWorkItem?
    private let queue = DispatchQueue.global(qos: .utility)
    private(set) var isRestarting = false

    // Discover the bridgesessions binary
    private let bsBinaryPath: String = {
        let candidates = [
            NSHomeDirectory() + "/.local/bin/bridgesessions",
            "/usr/local/bin/bridgesessions",
            "/opt/homebrew/bin/bridgesessions"
        ]
        for p in candidates where FileManager.default.isExecutableFile(atPath: p) {
            return p
        }
        return NSHomeDirectory() + "/.local/bin/bridgesessions"
    }()

    var isRunning: Bool {
        return process?.isRunning ?? false
    }

    // MARK: - Start

    func start() {
        guard process == nil || process?.isRunning == false else { return }
        isRestarting = false

        let p = Process()
        p.executableURL = URL(fileURLWithPath: bsBinaryPath)
        p.arguments = ["--cua-helper"]

        // Redirect stdout/stderr to /dev/null to prevent pipe buffer deadlock
        // (undrained pipe fills at 64KB and blocks the helper)
        let devNull = FileHandle(forWritingAtPath: "/dev/null") ?? FileHandle.standardOutput
        p.standardOutput = devNull
        p.standardError = devNull

        p.terminationHandler = { [weak self] proc in
            NSLog("[BSMenubar] cua-helper exited (status=%d)", proc.terminationStatus)
            DispatchQueue.main.async {
                self?.process = nil
                // Auto-restart after 3s if not deliberately stopped
                self?.scheduleRestart()
            }
        }

        do {
            try p.run()
            process = p
            NSLog("[BSMenubar] cua-helper started (pid=%d, bin=%@)", p.processIdentifier, bsBinaryPath)
        } catch {
            NSLog("[BSMenubar] Failed to start cua-helper: %@", error.localizedDescription)
            process = nil
            // Retry after delay
            scheduleRestart()
        }
    }

    // MARK: - Stop

    func stop() {
        cancelRestart()
        process?.terminate()
        process = nil
        NSLog("[BSMenubar] cua-helper stopped")
    }

    // MARK: - Restart

    func restart() {
        NSLog("[BSMenubar] Restarting cua-helper…")
        cancelRestart()
        process?.terminate()
        process = nil
        isRestarting = true
        // Small delay to let the port free up
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { [weak self] in
            self?.start()
        }
    }

    // MARK: - Auto-restart

    private func scheduleRestart() {
        cancelRestart()
        guard !isRestarting else { return }
        isRestarting = true
        let work = DispatchWorkItem { [weak self] in
            self?.start()  // clears isRestarting on success
        }
        restartWorkItem = work
        queue.asyncAfter(deadline: .now() + 3, execute: work)
    }

    private func cancelRestart() {
        restartWorkItem?.cancel()
        restartWorkItem = nil
    }
}
