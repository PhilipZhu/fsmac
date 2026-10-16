// FSMac daemon: the control socket used by the app and the CLI.
//
// Newline-delimited JSON over a Unix domain socket. Everything runs on the
// volume manager's queue. Mounting, unmounting and ejecting are open to any
// local user (as in Finder); changing settings, checking and formatting
// require an administrator.

import Darwin
import Foundation

// <membership.h> is not exposed to Swift.
@_silgen_name("mbr_uid_to_uuid")
private func mbr_uid_to_uuid(_ uid: uid_t, _ uu: UnsafeMutablePointer<UInt8>) -> Int32
@_silgen_name("mbr_gid_to_uuid")
private func mbr_gid_to_uuid(_ gid: gid_t, _ uu: UnsafeMutablePointer<UInt8>) -> Int32
@_silgen_name("mbr_check_membership")
private func mbr_check_membership(_ user: UnsafeMutablePointer<UInt8>, _ group: UnsafeMutablePointer<UInt8>,
                                  _ isMember: UnsafeMutablePointer<Int32>) -> Int32

final class IPCServer {
    private let path: String
    private let manager: VolumeManager
    private let ctx: Context
    private let q: DispatchQueue
    private var listenFD: Int32 = -1
    private var acceptSource: DispatchSourceRead?
    private var clients: [ObjectIdentifier: Client] = [:]

    init(path: String, manager: VolumeManager, ctx: Context) {
        self.path = path
        self.manager = manager
        self.ctx = ctx
        self.q = manager.q
    }

    func start() throws {
        unlink(path)
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw POSIXError(.EMFILE) }
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let bytes = Array(path.utf8)
        guard bytes.count < MemoryLayout.size(ofValue: addr.sun_path) else {
            close(fd)
            throw POSIXError(.ENAMETOOLONG)
        }
        withUnsafeMutableBytes(of: &addr.sun_path) { p in
            p.copyBytes(from: bytes)
            p[bytes.count] = 0
        }
        addr.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
        let rc = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard rc == 0, listen(fd, 16) == 0 else {
            let e = errno
            close(fd)
            throw POSIXError(POSIXErrorCode(rawValue: e) ?? .EADDRINUSE)
        }
        // Any local user may connect; requests are authorised per peer.
        chmod(path, ctx.userMode ? 0o600 : 0o666)
        _ = fcntl(fd, F_SETFD, FD_CLOEXEC)
        _ = fcntl(fd, F_SETFL, O_NONBLOCK)
        listenFD = fd
        let src = DispatchSource.makeReadSource(fileDescriptor: fd, queue: q)
        src.setEventHandler { [weak self] in self?.acceptClients() }
        src.resume()
        acceptSource = src
    }

    func stop() {
        acceptSource?.cancel()
        if listenFD >= 0 { close(listenFD) }
        unlink(path)
    }

    private static let maxClients = 64
    private static let maxClientsPerUser = 16
    private var acceptPaused = false

    private func acceptClients() {
        while true {
            let cfd = accept(listenFD, nil, nil)
            if cfd < 0 {
                if errno == EMFILE || errno == ENFILE {
                    // Out of descriptors: stop accepting for a moment rather
                    // than spinning on the still-readable listener.
                    pauseAccepting()
                }
                break
            }
            _ = fcntl(cfd, F_SETFD, FD_CLOEXEC)
            _ = fcntl(cfd, F_SETFL, O_NONBLOCK)
            var on: Int32 = 1
            setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
            var cred = xucred()
            var len = socklen_t(MemoryLayout<xucred>.size)
            guard getsockopt(cfd, SOL_LOCAL, LOCAL_PEERCRED, &cred, &len) == 0 else {
                close(cfd)
                continue
            }
            if ctx.userMode && cred.cr_uid != getuid() {
                close(cfd)
                continue
            }
            let mine = clients.values.filter { $0.uid == cred.cr_uid }.count
            if clients.count >= Self.maxClients || mine >= Self.maxClientsPerUser {
                close(cfd)
                continue
            }
            let client = Client(fd: cfd, uid: cred.cr_uid, admin: isAdmin(cred), queue: q)
            client.onRequest = { [weak self, weak client] req in
                guard let self, let client else { return }
                self.handle(req, from: client)
            }
            client.onClose = { [weak self, weak client] in
                guard let self, let client else { return }
                self.clients.removeValue(forKey: ObjectIdentifier(client))
            }
            clients[ObjectIdentifier(client)] = client
            client.start()
        }
    }

    private func pauseAccepting() {
        guard !acceptPaused, let src = acceptSource else { return }
        acceptPaused = true
        src.suspend()
        q.asyncAfter(deadline: .now() + 1) { [weak self] in
            guard let self, self.acceptPaused else { return }
            self.acceptPaused = false
            src.resume()
        }
    }

    private func isAdmin(_ cred: xucred) -> Bool {
        if cred.cr_uid == 0 || ctx.userMode { return true }
        let groups = withUnsafeBytes(of: cred.cr_groups) { Array($0.bindMemory(to: gid_t.self)) }
        if groups.prefix(Int(cred.cr_ngroups)).contains(80) { return true }  // admin
        // xucred holds at most 16 groups; ask Directory Services as well.
        var uu = [UInt8](repeating: 0, count: 16)
        var gu = [UInt8](repeating: 0, count: 16)
        var member: Int32 = 0
        return mbr_uid_to_uuid(cred.cr_uid, &uu) == 0 &&
            mbr_gid_to_uuid(80, &gu) == 0 &&
            mbr_check_membership(&uu, &gu, &member) == 0 && member != 0
    }

    // MARK: - Events

    func broadcastVolumes() {
        var ev = Response(id: nil, ok: true)
        ev.event = Event.volumes
        ev.volumes = manager.snapshot()
        broadcast(ev)
    }

    func notify(title: String, message: String) {
        var ev = Response(id: nil, ok: true)
        ev.event = Event.notify
        ev.title = title
        ev.message = message
        broadcast(ev)
    }

    func log(level: String, message: String) {
        var ev = Response(id: nil, ok: true)
        ev.event = Event.log
        ev.title = level
        ev.message = message
        broadcast(ev)
    }

    private func broadcast(_ r: Response) {
        guard let data = try? JSONEncoder().encode(r) else { return }
        for c in clients.values where c.subscribed { c.send(data) }
    }

    // MARK: - Requests

    private func handle(_ req: Request, from c: Client) {
        func reply(_ err: String?, _ fill: (inout Response) -> Void = { _ in }) {
            var r = Response(id: req.id, ok: err == nil)
            r.error = err
            if err == nil { fill(&r) }
            c.send(r)
        }
        func needAdmin() -> Bool {
            if c.admin { return true }
            reply("This requires an administrator account.")
            return false
        }
        /// Overriding what an administrator configured, forcing an unmount
        /// or discarding Windows' hibernation data: administrators and the
        /// user at the console only.
        func needPrivileged(_ what: String) -> Bool {
            if c.admin || c.uid == ctx.consoleUser().uid { return true }
            reply("\(what) requires an administrator or the user logged in at the console.")
            return false
        }
        let progress: (String) -> Void = { [weak c] line in
            var ev = Response(id: req.id, ok: true)
            ev.event = Event.output
            ev.output = line
            c?.sendAsync(ev)
        }

        switch req.cmd {
        case Command.hello:
            reply(nil) {
                $0.version = FSMac.version
                $0.userMode = ctx.userMode
                $0.admin = c.admin
            }
        case Command.list:
            reply(nil) { $0.volumes = manager.snapshot() }
            manager.refreshSpace()   // subscribers get the new figures
        case Command.subscribe:
            c.subscribed = true
            reply(nil) {
                $0.volumes = manager.snapshot()
                $0.settings = manager.settings
            }
        case Command.mount:
            guard let disk = req.disk else { return reply("missing disk") }
            guard let v = manager.volumes[disk]?.info else { return reply("no such volume: \(disk)") }
            let readOnly = req.readOnly ?? false
            let overrides = v.policy.mode == .ignore || (v.policy.mode == .readOnly && !readOnly)
            if overrides && !needPrivileged("Mounting this volume against its settings") { return }
            if req.removeHibernation == true && !needPrivileged("Discarding Windows' hibernation data") { return }
            manager.mount(disk, readOnly: readOnly,
                          removeHibernation: req.removeHibernation ?? false) { reply($0) }
        case Command.unmount:
            guard let disk = req.disk else { return reply("missing disk") }
            if req.force == true && !needPrivileged("Force unmounting") { return }
            manager.unmount(disk, force: req.force ?? false, eject: req.eject ?? false) { reply($0) }
        case Command.eject:
            guard let disk = req.disk else { return reply("missing disk") }
            // Only disks holding FSMac volumes; Finder ejects everything else.
            guard let whole = manager.volumes[disk]?.info.wholeDisk ??
                    manager.volumes.values.first(where: { $0.info.wholeDisk == disk })?.info.wholeDisk else {
                return reply("\(disk) has no NTFS or ext volumes")
            }
            manager.ejectDisk(whole) { reply($0) }
        case Command.check:
            guard needAdmin() else { return }
            guard let disk = req.disk else { return reply("missing disk") }
            manager.check(disk, repair: req.repair ?? false, output: progress) { err, summary in
                reply(err) { $0.message = summary }
            }
        case Command.format:
            guard needAdmin() else { return }
            guard let disk = req.disk, let type = req.fsType else { return reply("missing disk or type") }
            manager.format(disk, fsType: type, label: req.label ?? "Untitled",
                           force: req.force ?? false, output: progress) { reply($0) }
        case Command.getSettings:
            reply(nil) { $0.settings = manager.settings }
        case Command.setSettings:
            guard needAdmin() else { return }
            guard let s = req.settings else { return reply("missing settings") }
            manager.setSettings(s)
            reply(nil) { $0.settings = manager.settings }
        case Command.setPolicy:
            guard needAdmin() else { return }
            guard let uuid = req.uuid else { return reply("missing uuid") }
            manager.setPolicy(uuid: uuid, policy: req.policy)
            reply(nil) { $0.settings = manager.settings }
        case Command.rescan:
            manager.rescan()
            reply(nil)
        case Command.restart:
            guard needPrivileged("Restarting the helper") else { return }
            // launchd starts the helper again (KeepAlive), from the app's
            // current files.
            manager.shutdown {
                self.q.async {
                    reply(nil)
                    Log.info("restarting on request")
                    self.q.asyncAfter(deadline: .now() + 0.5) { exit(0) }
                }
            }
        case Command.prepareUninstall:
            guard needAdmin() else { return }
            manager.shutdown {
                if !self.ctx.userMode { FilesystemBundle.remove() }
                self.q.async { reply(nil) }
            }
        default:
            reply("unknown command \(req.cmd)")
        }
    }
}

/// One connected client.
private final class Client {
    let fd: Int32
    let uid: uid_t
    let admin: Bool
    var subscribed = false
    var onRequest: (Request) -> Void = { _ in }
    var onClose: () -> Void = {}
    private let q: DispatchQueue
    private var readSource: DispatchSourceRead?
    private var writeSource: DispatchSourceWrite?
    private var writeSuspended = true
    private var inbox = Data()
    private var outbox = Data()
    private var closed = false

    init(fd: Int32, uid: uid_t, admin: Bool, queue: DispatchQueue) {
        self.fd = fd
        self.uid = uid
        self.admin = admin
        self.q = queue
    }

    func start() {
        // The descriptor is closed once both sources have been cancelled.
        var pending = 2
        let fd = self.fd
        let done = { pending -= 1; if pending == 0 { Darwin.close(fd) } }
        let rs = DispatchSource.makeReadSource(fileDescriptor: fd, queue: q)
        rs.setEventHandler { [weak self] in self?.readAvailable() }
        rs.setCancelHandler(handler: done)
        rs.resume()
        readSource = rs
        let ws = DispatchSource.makeWriteSource(fileDescriptor: fd, queue: q)
        ws.setEventHandler { [weak self] in self?.flush() }
        ws.setCancelHandler(handler: done)
        writeSource = ws   // resumed only while there is something to send
    }

    private func readAvailable() {
        var buf = [UInt8](repeating: 0, count: 65536)
        while true {
            let n = read(fd, &buf, buf.count)
            if n > 0 {
                inbox.append(buf, count: n)
                if inbox.count > 16 << 20 { return close() }
                continue
            }
            if n == 0 || (errno != EAGAIN && errno != EINTR) { return close() }
            break
        }
        while let nl = inbox.firstIndex(of: 0x0A) {
            let line = inbox[inbox.startIndex..<nl]
            inbox.removeSubrange(inbox.startIndex...nl)
            if line.isEmpty { continue }
            do {
                onRequest(try JSONDecoder().decode(Request.self, from: line))
            } catch {
                var r = Response(id: nil, ok: false)
                r.error = "malformed request"
                send(r)
            }
        }
    }

    func send(_ r: Response) {
        if let data = try? JSONEncoder().encode(r) { send(data) }
    }

    /// For callers on other queues.
    func sendAsync(_ r: Response) {
        q.async { [weak self] in self?.send(r) }
    }

    func send(_ data: Data) {
        guard !closed else { return }
        outbox.append(data)
        outbox.append(0x0A)
        if outbox.count > 8 << 20 { return close() }    // client stopped reading
        flush()
    }

    private func flush() {
        while !outbox.isEmpty {
            let n = outbox.withUnsafeBytes { write(fd, $0.baseAddress, $0.count) }
            if n > 0 {
                outbox.removeFirst(n)
                continue
            }
            if n < 0 && (errno == EAGAIN || errno == EINTR) { break }
            return close()
        }
        if outbox.isEmpty, !writeSuspended {
            writeSource?.suspend()
            writeSuspended = true
        } else if !outbox.isEmpty, writeSuspended {
            writeSource?.resume()
            writeSuspended = false
        }
    }

    private func close() {
        guard !closed else { return }
        closed = true
        readSource?.cancel()
        if writeSuspended { writeSource?.resume() }
        writeSource?.cancel()
        onClose()
    }
}
