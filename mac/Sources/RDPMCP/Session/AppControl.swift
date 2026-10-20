import Darwin
import Foundation

/// Control-socket van de app zelf (`~/.rdp-mcp/app-<pid>.ctl`). Hiermee leest de
/// MCP-server het adresboek en opent of sluit hij sessies. Wachtwoorden gaan nooit
/// over deze socket: aanmelden gebeurt in de app, met de sleutelhanger of het
/// aanmeldvenster. Protocol: zie PROTOCOL.md.
final class AppControl {
    private let manager: SessionManager
    private let path: String
    private var listener: Int32 = -1

    init(manager: SessionManager) {
        self.manager = manager
        let dir = ProcessInfo.processInfo.environment["RDP_MCP_DIR"] ?? NSHomeDirectory() + "/.rdp-mcp"
        path = dir + "/app-\(getpid()).ctl"
    }

    func start() {
        let dir = (path as NSString).deletingLastPathComponent
        mkdir(dir, 0o700)
        // overblijfsels van instanties die niet netjes stopten
        for name in (try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? []
        where name.hasPrefix("app-") && name.hasSuffix(".ctl") {
            if let pid = Int32(name.dropFirst(4).dropLast(4)), kill(pid, 0) != 0, errno == ESRCH {
                unlink(dir + "/" + name)
            }
        }
        unlink(path)
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { return }
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let bytes = Array(path.utf8CString)
        guard bytes.count <= MemoryLayout.size(ofValue: addr.sun_path) else {
            Darwin.close(fd)
            return
        }
        withUnsafeMutableBytes(of: &addr.sun_path) { buf in
            bytes.withUnsafeBytes { buf.copyMemory(from: $0) }
        }
        let bound = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard bound == 0, chmod(path, 0o600) == 0, listen(fd, 4) == 0 else {
            NSLog("rdp-mcp: app control socket not started (%s)", strerror(errno))
            Darwin.close(fd)
            unlink(path)
            return
        }
        listener = fd
        Thread.detachNewThread { [weak self] in
            while true {
                let client = accept(fd, nil, nil)
                if client < 0 {
                    if errno == EINTR { continue }
                    return // socket gesloten door stop()
                }
                var one: Int32 = 1
                setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
                Thread.detachNewThread { self?.serve(client) }
            }
        }
    }

    func stop() {
        if listener >= 0 {
            Darwin.close(listener)
            listener = -1
        }
        unlink(path)
    }

    // MARK: - verbinding met de MCP-server (eigen thread)

    private func serve(_ client: Int32) {
        defer { Darwin.close(client) }
        var pending = [UInt8]()
        var chunk = [UInt8](repeating: 0, count: 4096)
        while true {
            if let nl = pending.firstIndex(of: 0x0A) {
                let line = String(decoding: pending[..<nl], as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
                pending.removeSubrange(...nl)
                guard write(client, handle(line)) else { return }
                continue
            }
            let n = read(client, &chunk, chunk.count)
            if n <= 0 || pending.count > 65536 { return }
            pending.append(contentsOf: chunk[..<n])
        }
    }

    private func handle(_ line: String) -> Data {
        let parts = line.split(separator: " ", maxSplits: 1).map(String.init)
        let arg = parts.count > 1 ? parts[1].trimmingCharacters(in: .whitespaces) : ""
        switch parts.first?.uppercased() ?? "" {
        case "HOSTS":
            let json = onMain { self.hostsJSON() }
            return reply("OK \(json.count)", payload: json)
        case "CONNECT":
            guard let id = UUID(uuidString: arg) else { return reply("ERR syntax CONNECT <host-id>") }
            return onMain { await self.connect(id) }
        case "STATE":
            guard !arg.isEmpty else { return reply("ERR syntax STATE <session-id>") }
            return onMain { self.state(arg) }
        case "CLOSE":
            guard !arg.isEmpty else { return reply("ERR syntax CLOSE <session-id>") }
            return onMain { self.close(arg) }
        case "":
            return reply("ERR syntax empty line")
        case let cmd:
            return reply("ERR unknown-command unknown command '\(cmd)'")
        }
    }

    // MARK: - commando's (main thread)

    @MainActor private func hostsJSON() -> Data {
        let hosts: [[String: Any]] = manager.book.entries.map { e in
            [
                "id": e.id.uuidString,
                "name": e.displayName,
                "group": e.group,
                "host": e.host,
                "username": e.domain.isEmpty ? e.username : "\(e.domain)\\\(e.username)",
                "saved_password": e.hasSavedPassword,
                "touch_id": e.hasSavedPassword && e.requireTouchID,
                "session": manager.session(for: e)?.socketID.map { $0 as Any } ?? NSNull(),
            ]
        }
        return (try? JSONSerialization.data(withJSONObject: hosts, options: [.sortedKeys])) ?? Data("[]".utf8)
    }

    @MainActor private func connect(_ id: UUID) async -> Data {
        guard let entry = manager.book.entry(id: id) else {
            return reply("ERR not-found no host with id \(id.uuidString) in the address book")
        }
        if let open = manager.session(for: entry), let sid = open.socketID {
            manager.selectedID = open.id
            return reply("OK \(sid) existing")
        }
        guard let session = await manager.connect(entry, openedByAI: true) else {
            return reply("ERR cancelled the user did not confirm with Touch ID")
        }
        guard let sid = session.socketID else {
            if case .failed(let message) = session.status {
                return reply("ERR failed \(oneLine(message))")
            }
            return reply("ERR failed the session could not start")
        }
        return reply("OK \(sid) new")
    }

    @MainActor private func state(_ sid: String) -> Data {
        guard let session = find(sid) else { return reply("ERR not-found no open session \(sid)") }
        switch session.status {
        case .connecting: return reply(session.isWaitingForUser ? "OK waiting-for-user" : "OK connecting")
        case .connected: return reply("OK connected")
        case .disconnected(let message): return reply("OK disconnected \(oneLine(message ?? ""))")
        case .failed(let message): return reply("OK failed \(oneLine(message))")
        }
    }

    @MainActor private func close(_ sid: String) -> Data {
        guard let session = find(sid) else { return reply("ERR not-found no open session \(sid)") }
        guard session.openedByAI else {
            return reply("ERR not-allowed the user opened this session; only the user can close it")
        }
        manager.close(session)
        return reply("OK")
    }

    @MainActor private func find(_ sid: String) -> RDPSession? {
        manager.sessions.first { $0.socketID == sid }
    }

    // MARK: - hulpjes

    /// Voert body uit op de main actor en wacht op het resultaat (de socketthread mag blokkeren)
    private func onMain<T>(_ body: @escaping @MainActor () async -> T) -> T {
        var result: T?
        let done = DispatchSemaphore(value: 0)
        Task { @MainActor in
            result = await body()
            done.signal()
        }
        done.wait()
        return result!
    }

    private func reply(_ line: String, payload: Data = Data()) -> Data {
        Data((line + "\n").utf8) + payload
    }

    private func oneLine(_ text: String) -> String {
        text.replacingOccurrences(of: "\n", with: " ").replacingOccurrences(of: "\r", with: " ")
    }

    private func write(_ fd: Int32, _ data: Data) -> Bool {
        data.withUnsafeBytes { buf in
            var offset = 0
            while offset < buf.count {
                let n = Darwin.write(fd, buf.baseAddress! + offset, buf.count - offset)
                if n < 0 && errno == EINTR { continue }
                if n <= 0 { return false }
                offset += n
            }
            return true
        }
    }
}
