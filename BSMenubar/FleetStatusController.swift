import Cocoa

// MARK: - Fleet Peer Model

struct FleetPeer {
    let name: String
    let address: String
    let version: String
    let status: String      // "healthy", "self", "offline", etc.
    let uptime: String
    var isOnline: Bool { status.lowercased() == "healthy" || status.lowercased() == "self" }
    var isSelf: Bool { status.lowercased() == "self" }
}

// MARK: - Fleet Status Controller

final class FleetStatusController: NSObject {
    private(set) var peers: [FleetPeer] = []
    private var pollTimer: DispatchSourceTimer?
    private(set) var isPolling = false
    private var lastError: String?

    // Dedicated serial queue for polling — prevents blocking the timer
    private let pollQueue = DispatchQueue(label: "com.minddragon.bridgesessions.fleet-poll", qos: .utility)

    // bs binary path discovery
    private let bsBinaryPath: String = {
        let candidates = [
            NSHomeDirectory() + "/.local/bin/bridgesessions",
            "/usr/local/bin/bridgesessions",
            "/opt/homebrew/bin/bridgesessions"
        ]
        for p in candidates where FileManager.default.isExecutableFile(atPath: p) {
            return p
        }
        return "/usr/local/bin/bridgesessions"
    }()

    // UI
    let viewController = NSViewController()
    private let tableView = NSTableView()
    private let statusLabel = NSTextField(labelWithString: "")
    private var scrollView: NSScrollView!

    override init() {
        super.init()
        setupUI()
    }

    // MARK: - Polling

    func startPolling() {
        guard !isPolling else { return }
        isPolling = true
        // Initial poll on background queue (not main thread)
        pollQueue.async { [weak self] in self?.pollNow() }
        let timer = DispatchSource.makeTimerSource(queue: pollQueue)
        timer.schedule(deadline: .now() + 5, repeating: 5)
        timer.setEventHandler { [weak self] in self?.pollNow() }
        timer.resume()
        pollTimer = timer
    }

    func stopPolling() {
        pollTimer?.cancel()
        pollTimer = nil
        isPolling = false
    }

    func pollNow() {
        let pipe = Pipe()
        let errPipe = Pipe()
        let task = Process()
        task.executableURL = URL(fileURLWithPath: bsBinaryPath)
        task.arguments = ["fleet"]
        task.standardOutput = pipe
        task.standardError = errPipe

        do {
            try task.run()
        } catch {
            DispatchQueue.main.async { self.updateError("Cannot launch bridgesessions: \(error.localizedDescription)") }
            return
        }

        // Drain stderr on a background thread to prevent pipe buffer deadlock
        DispatchQueue.global(qos: .utility).async {
            _ = errPipe.fileHandleForReading.readDataToEndOfFile()
        }

        // 10s timeout — kill the process if it hangs
        let timeoutQueue = DispatchQueue.global(qos: .utility)
        let timeoutWork = DispatchWorkItem {
            if task.isRunning { task.terminate() }
        }
        timeoutQueue.asyncAfter(deadline: .now() + 10, execute: timeoutWork)

        task.waitUntilExit()
        timeoutWork.cancel()

        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        let output = String(data: data, encoding: .utf8) ?? ""
        let parsed = Self.parseFleetOutput(output)

        DispatchQueue.main.async {
            self.peers = parsed
            self.lastError = parsed.isEmpty && task.terminationStatus != 0 ? "fleet command failed" : nil
            self.tableView.reloadData()
            self.updateStatusLabel()
        }
    }

    // MARK: - Parsing

    // Parses markdown-style table:
    // | Name | Address | Version | Status | Uptime |
    // |------|---------|---------|--------|--------|
    // | linux-d | 203.0.113.14:37812 | 26.08.06-beta1 | healthy | 1h |
    static func parseFleetOutput(_ output: String) -> [FleetPeer] {
        var peers: [FleetPeer] = []
        let lines = output.split(separator: "\n", omittingEmptySubsequences: true)
        for (idx, line) in lines.enumerated() {
            // Skip header (idx 0) and separator (idx 1, contains only dashes/colons)
            if idx == 0 { continue }
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.hasPrefix("|") && trimmed.contains("---") { continue }
            guard trimmed.hasPrefix("|") else { continue }

            let cols = trimmed.split(separator: "|", omittingEmptySubsequences: true)
                .map { $0.trimmingCharacters(in: .whitespaces) }
            // Expect: [Name, Address, Version, Status, Uptime]
            if cols.count >= 4 {
                let name = cols[0]
                let address = cols[1]
                let version = cols[2]
                let status = cols[3]
                let uptime = cols.count > 4 ? cols[4] : ""
                peers.append(FleetPeer(name: name, address: address, version: version, status: status, uptime: uptime))
            }
        }
        return peers
    }

    // MARK: - UI

    private func setupUI() {
        let container = NSView(frame: NSRect(x: 0, y: 0, width: 440, height: 280))
        container.wantsLayer = true

        scrollView = NSScrollView()
        scrollView.translatesAutoresizingMaskIntoConstraints = false
        scrollView.hasVerticalScroller = true
        scrollView.autohidesScrollers = true

        tableView.dataSource = self
        tableView.delegate = self
        tableView.headerView = nil
        tableView.backgroundColor = .clear
        tableView.rowHeight = 22
        tableView.columnAutoresizingStyle = .uniformColumnAutoresizingStyle

        let nameCol = NSTableColumn(identifier: .init("name"))
        nameCol.title = "Peer"
        let addrCol = NSTableColumn(identifier: .init("addr"))
        addrCol.title = "Address"
        let verCol = NSTableColumn(identifier: .init("ver"))
        verCol.title = "Version"
        let stCol = NSTableColumn(identifier: .init("status"))
        stCol.title = "Status"

        tableView.addTableColumn(nameCol)
        tableView.addTableColumn(addrCol)
        tableView.addTableColumn(verCol)
        tableView.addTableColumn(stCol)
        tableView.sizeToFit()

        scrollView.documentView = tableView

        statusLabel.translatesAutoresizingMaskIntoConstraints = false
        statusLabel.font = .systemFont(ofSize: 11)
        statusLabel.textColor = .secondaryLabelColor

        container.addSubview(scrollView)
        container.addSubview(statusLabel)

        NSLayoutConstraint.activate([
            scrollView.topAnchor.constraint(equalTo: container.topAnchor, constant: 8),
            scrollView.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: 8),
            scrollView.trailingAnchor.constraint(equalTo: container.trailingAnchor, constant: -8),
            scrollView.bottomAnchor.constraint(equalTo: statusLabel.topAnchor, constant: -4),
            statusLabel.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: 12),
            statusLabel.trailingAnchor.constraint(equalTo: container.trailingAnchor, constant: -12),
            statusLabel.bottomAnchor.constraint(equalTo: container.bottomAnchor, constant: -8),
            statusLabel.heightAnchor.constraint(equalToConstant: 16)
        ])

        viewController.view = container
        updateStatusLabel()
    }

    private func updateStatusLabel() {
        if let err = lastError {
            statusLabel.stringValue = "⚠ \(err)"
            statusLabel.textColor = .systemOrange
            return
        }
        let online = peers.filter { $0.isOnline }.count
        statusLabel.stringValue = "\(online) of \(peers.count) peers online — updated every 5s"
        statusLabel.textColor = .secondaryLabelColor
    }

    private func updateError(_ msg: String) {
        lastError = msg
        tableView.reloadData()
        updateStatusLabel()
    }
}

// MARK: - Table DataSource / Delegate

extension FleetStatusController: NSTableViewDataSource, NSTableViewDelegate {
    func numberOfRows(in tableView: NSTableView) -> Int {
        return peers.count
    }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        guard row < peers.count else { return nil }
        let peer = peers[row]
        let id = tableColumn?.identifier.rawValue ?? ""
        var text = ""
        switch id {
        case "name":    text = peer.name + (peer.isSelf ? " (self)" : "")
        case "addr":    text = peer.address
        case "ver":     text = peer.version
        case "status":  text = peer.isOnline ? "● \(peer.status)" : "○ \(peer.status)"
        default: break
        }
        let cell = NSTextField(labelWithString: text)
        cell.font = .systemFont(ofSize: 11, weight: peer.isSelf ? .semibold : .regular)
        cell.textColor = (id == "status") ? (peer.isOnline ? .systemGreen : .systemGray) : .labelColor
        cell.lineBreakMode = .byTruncatingTail
        return cell
    }
}
