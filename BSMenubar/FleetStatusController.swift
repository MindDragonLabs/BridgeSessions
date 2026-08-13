import Cocoa

// MARK: - Fleet Peer Model

struct FleetPeer {
    let name: String
    let address: String
    let status: String      // "healthy", "self", "offline", etc.
    let uptime: String
    let cpu: String         // "15%" or "–"
    let mem: String
    let disk: String
    var isOnline: Bool { status.lowercased() == "healthy" || status.lowercased() == "self" }
    var isSelf: Bool { status.lowercased() == "self" }
}

// MARK: - Fleet Status Controller

final class FleetStatusController: NSObject {
    static let popoverSize = NSSize(width: 460, height: 340)

    private(set) var peers: [FleetPeer] = []
    private var pollTimer: DispatchSourceTimer?
    private(set) var isPolling = false
    private var lastError: String?

    private let pollQueue = DispatchQueue(label: "com.minddragon.bridgesessions.fleet-poll", qos: .utility)

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
        task.arguments = ["fleet", "--json"]
        task.standardOutput = pipe
        task.standardError = errPipe

        do {
            try task.run()
        } catch {
            DispatchQueue.main.async { self.updateError("Cannot launch bridgesessions: \(error.localizedDescription)") }
            return
        }

        DispatchQueue.global(qos: .utility).async {
            _ = errPipe.fileHandleForReading.readDataToEndOfFile()
        }

        let timeoutWork = DispatchWorkItem {
            if task.isRunning { task.terminate() }
        }
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 10, execute: timeoutWork)

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

    static func parseFleetOutput(_ output: String) -> [FleetPeer] {
        let trimmed = output.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.hasPrefix("{") {
            let jsonPeers = parseFleetJSON(trimmed)
            if !jsonPeers.isEmpty { return jsonPeers }
        }
        if trimmed.contains("|") {
            let md = parseFleetMarkdown(trimmed)
            if !md.isEmpty { return md }
        }
        return parseFleetTextTable(trimmed)
    }

    static func parseFleetJSON(_ output: String) -> [FleetPeer] {
        guard let data = output.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return []
        }
        var peers: [FleetPeer] = []
        for (key, value) in obj {
            guard let d = value as? [String: Any] else { continue }
            let name: String = {
                if let n = d["name"] as? String, !n.isEmpty { return n }
                return key
            }()
            var uptime = ""
            if let n = d["uptime_s"] as? NSNumber {
                uptime = formatUptime(n.intValue)
            }
            peers.append(FleetPeer(
                name: name,
                address: d["addr"] as? String ?? "",
                status: d["status"] as? String ?? "",
                uptime: uptime,
                cpu: formatPct(d["cpu_pct"]),
                mem: formatPct(d["mem_pct"]),
                disk: formatPct(d["disk_pct"])
            ))
        }
        return sortPeers(peers)
    }

    static func parseFleetMarkdown(_ output: String) -> [FleetPeer] {
        var peers: [FleetPeer] = []
        let lines = output.split(separator: "\n", omittingEmptySubsequences: true)
        for (idx, line) in lines.enumerated() {
            if idx == 0 { continue }
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.hasPrefix("|") && trimmed.contains("---") { continue }
            guard trimmed.hasPrefix("|") else { continue }
            let cols = trimmed.split(separator: "|", omittingEmptySubsequences: true)
                .map { $0.trimmingCharacters(in: .whitespaces) }
            if cols.count >= 4 {
                peers.append(FleetPeer(name: cols[0], address: cols[1], status: cols[3],
                                       uptime: cols.count > 4 ? cols[4] : "",
                                       cpu: "–", mem: "–", disk: "–"))
            }
        }
        return sortPeers(peers)
    }

    // NAME ADDRESS VERSION STATUS UP CPU MEM DISK LOAD OS CUA
    static func parseFleetTextTable(_ output: String) -> [FleetPeer] {
        var peers: [FleetPeer] = []
        for line in output.split(separator: "\n", omittingEmptySubsequences: true) {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.isEmpty { continue }
            if trimmed.hasPrefix("NAME") { continue }
            if trimmed.allSatisfy({ $0 == "-" || $0 == " " }) { continue }
            if trimmed.contains("listed") { continue }
            let cols = trimmed.split(whereSeparator: { $0.isWhitespace }).map(String.init)
            guard cols.count >= 4 else { continue }
            peers.append(FleetPeer(
                name: cols[0],
                address: cols[1],
                status: cols[3],
                uptime: cols.count > 4 ? cols[4] : "",
                cpu: cols.count > 5 ? dashIfEmpty(cols[5]) : "–",
                mem: cols.count > 6 ? dashIfEmpty(cols[6]) : "–",
                disk: cols.count > 7 ? dashIfEmpty(cols[7]) : "–"
            ))
        }
        return sortPeers(peers)
    }

    static func formatPct(_ raw: Any?) -> String {
        if raw == nil || raw is NSNull { return "–" }
        if let n = raw as? NSNumber { return String(format: "%.0f%%", n.doubleValue) }
        if let s = raw as? String, !s.isEmpty, s != "-" { return s.hasSuffix("%") ? s : "\(s)%" }
        return "–"
    }

    static func dashIfEmpty(_ s: String) -> String {
        (s.isEmpty || s == "-") ? "–" : s
    }

    static func formatUptime(_ seconds: Int) -> String {
        if seconds < 0 { return "" }
        if seconds < 60 { return "\(seconds)s" }
        if seconds < 3600 { return "\(seconds / 60)m" }
        if seconds < 86400 { return "\(seconds / 3600)h" }
        return "\(seconds / 86400)d"
    }

    static func sortPeers(_ peers: [FleetPeer]) -> [FleetPeer] {
        peers.sorted { a, b in
            if a.isSelf != b.isSelf { return a.isSelf }
            if a.isOnline != b.isOnline { return a.isOnline }
            return a.name.localizedCaseInsensitiveCompare(b.name) == .orderedAscending
        }
    }

    // MARK: - UI

    private func setupUI() {
        let size = Self.popoverSize
        let container = NSView(frame: NSRect(origin: .zero, size: size))
        container.wantsLayer = true

        scrollView = NSScrollView()
        scrollView.translatesAutoresizingMaskIntoConstraints = false
        scrollView.hasVerticalScroller = true
        scrollView.autohidesScrollers = true
        scrollView.borderType = .noBorder
        scrollView.drawsBackground = false

        tableView.dataSource = self
        tableView.delegate = self
        tableView.backgroundColor = .clear
        tableView.rowHeight = 22
        tableView.columnAutoresizingStyle = .lastColumnOnlyAutoresizingStyle
        tableView.usesAlternatingRowBackgroundColors = false
        tableView.allowsColumnReordering = false
        tableView.allowsColumnResizing = false

        func col(_ id: String, _ title: String, _ width: CGFloat, min minW: CGFloat? = nil) -> NSTableColumn {
            let c = NSTableColumn(identifier: .init(id))
            c.title = title
            c.width = width
            c.minWidth = minW ?? width
            c.maxWidth = (minW == nil) ? width : 400
            return c
        }

        tableView.addTableColumn(col("name", "Peer", 150, min: 110))
        tableView.addTableColumn(col("cpu", "CPU", 48))
        tableView.addTableColumn(col("mem", "MEM", 48))
        tableView.addTableColumn(col("disk", "DISK", 48))
        tableView.addTableColumn(col("status", "Status", 90, min: 72))

        scrollView.documentView = tableView

        statusLabel.translatesAutoresizingMaskIntoConstraints = false
        statusLabel.font = .systemFont(ofSize: 11)
        statusLabel.textColor = .secondaryLabelColor

        container.addSubview(scrollView)
        container.addSubview(statusLabel)

        NSLayoutConstraint.activate([
            scrollView.topAnchor.constraint(equalTo: container.topAnchor, constant: 4),
            scrollView.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: 8),
            scrollView.trailingAnchor.constraint(equalTo: container.trailingAnchor, constant: -8),
            scrollView.bottomAnchor.constraint(equalTo: statusLabel.topAnchor, constant: -4),
            statusLabel.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: 12),
            statusLabel.trailingAnchor.constraint(equalTo: container.trailingAnchor, constant: -12),
            statusLabel.bottomAnchor.constraint(equalTo: container.bottomAnchor, constant: -8),
            statusLabel.heightAnchor.constraint(equalToConstant: 16)
        ])

        viewController.view = container
        viewController.preferredContentSize = size
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
        var align: NSTextAlignment = .left
        var mono = false
        switch id {
        case "name":
            text = peer.name + (peer.isSelf ? " (self)" : "")
        case "cpu":
            text = peer.cpu; align = .right; mono = true
        case "mem":
            text = peer.mem; align = .right; mono = true
        case "disk":
            text = peer.disk; align = .right; mono = true
        case "status":
            text = peer.isOnline ? "● \(peer.status)" : "○ \(peer.status)"
        default:
            break
        }
        let cell = NSTextField(labelWithString: text)
        cell.font = mono
            ? .monospacedDigitSystemFont(ofSize: 11, weight: .regular)
            : .systemFont(ofSize: 11, weight: peer.isSelf ? .semibold : .regular)
        cell.alignment = align
        if id == "status" {
            cell.textColor = peer.isOnline ? .systemGreen : .systemGray
        } else {
            cell.textColor = .labelColor
        }
        cell.lineBreakMode = .byTruncatingTail
        return cell
    }
}
