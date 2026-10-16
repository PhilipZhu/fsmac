// FSMac: client side of the daemon's control socket (used by the app and
// the CLI).

import Foundation

public enum ClientError: Error, LocalizedError {
    case notRunning
    case disconnected
    case daemon(String)

    public var errorDescription: String? {
        switch self {
        case .notRunning: return "The FSMac helper is not running."
        case .disconnected: return "Lost the connection to the FSMac helper."
        case .daemon(let msg): return msg
        }
    }
}

/// A connection to fsmacd. Responses and events are delivered on `queue`.
public final class DaemonClient: @unchecked Sendable {
    /// State shared with the reader thread, which owns the descriptor and
    /// outlives the client if needed.
    private final class Connection: @unchecked Sendable {
        let fd: Int32
        let queue: DispatchQueue
        /// Guards everything below; never held while blocking.
        let lock = NSLock()
        /// Serialises writers; a stuck write is released by close().
        let sendLock = NSLock()
        var closed = false
        var nextID = 1
        var pending: [Int: (Response) -> Void] = [:]
        var progress: [Int: (String) -> Void] = [:]
        var onEvent: ((Response) -> Void)?
        var onDisconnect: (() -> Void)?

        init(fd: Int32, queue: DispatchQueue) {
            self.fd = fd
            self.queue = queue
        }

        /// Wakes the reader (and any stuck writer); the reader then closes
        /// the descriptor and fails what is pending.
        func shutdownSocket() {
            lock.lock()
            if !closed { shutdown(fd, SHUT_RDWR) }
            lock.unlock()
        }
    }

    private let conn: Connection

    /// Unsolicited events (volume list changes, notifications, logs).
    public var onEvent: ((Response) -> Void)? {
        get { conn.lock.lock(); defer { conn.lock.unlock() }; return conn.onEvent }
        set { conn.lock.lock(); conn.onEvent = newValue; conn.lock.unlock() }
    }
    /// Called once when the daemon goes away.
    public var onDisconnect: (() -> Void)? {
        get { conn.lock.lock(); defer { conn.lock.unlock() }; return conn.onDisconnect }
        set { conn.lock.lock(); conn.onDisconnect = newValue; conn.lock.unlock() }
    }

    public init(path: String = FSMac.socketPath, queue: DispatchQueue = .main) throws {
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw ClientError.notRunning }
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let bytes = Array(path.utf8)
        guard bytes.count < MemoryLayout.size(ofValue: addr.sun_path) else {
            Darwin.close(fd)
            throw ClientError.notRunning
        }
        withUnsafeMutableBytes(of: &addr.sun_path) { p in
            p.copyBytes(from: bytes)
            p[bytes.count] = 0
        }
        let rc = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard rc == 0 else {
            Darwin.close(fd)
            throw ClientError.notRunning
        }
        var on: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
        let conn = Connection(fd: fd, queue: queue)
        self.conn = conn
        let t = Thread { DaemonClient.readLoop(conn) }
        t.name = "fsmac-client"
        t.start()
    }

    deinit {
        conn.shutdownSocket()
    }

    /// Ends the connection; pending requests fail and onDisconnect is called.
    public func close() {
        conn.shutdownSocket()
    }

    /// Sends a request; `completion` receives the final response, `output`
    /// any progress lines (check, format).
    public func send(_ req: Request, output: ((String) -> Void)? = nil,
                     completion: @escaping (Response) -> Void) {
        var req = req
        let c = conn
        c.lock.lock()
        if c.closed {
            c.lock.unlock()
            var r = Response(id: req.id, ok: false)
            r.error = ClientError.disconnected.errorDescription
            c.queue.async { completion(r) }
            return
        }
        req.id = c.nextID
        c.nextID += 1
        guard var data = try? JSONEncoder().encode(req) else {
            c.lock.unlock()
            var r = Response(id: req.id, ok: false)
            r.error = "cannot encode the request"
            c.queue.async { completion(r) }
            return
        }
        c.pending[req.id] = completion
        if let output { c.progress[req.id] = output }
        c.lock.unlock()
        data.append(0x0A)

        // The reader closes the descriptor only while holding sendLock, so
        // it cannot be closed (and its number reused) during this write.
        c.sendLock.lock()
        c.lock.lock()
        let closed = c.closed
        c.lock.unlock()
        var ok = !closed
        if ok {
            ok = data.withUnsafeBytes { buf -> Bool in
                var off = 0
                while off < buf.count {
                    let n = write(c.fd, buf.baseAddress! + off, buf.count - off)
                    if n <= 0 {
                        if n < 0 && errno == EINTR { continue }
                        return false
                    }
                    off += n
                }
                return true
            }
        }
        c.sendLock.unlock()
        // A failed write means the connection is gone; the reader reports it
        // (including this request) once it sees the end of the stream.
        if !ok { c.shutdownSocket() }
    }

    /// Async convenience: throws the daemon's error message on failure.
    @discardableResult
    public func call(_ req: Request, output: ((String) -> Void)? = nil) async throws -> Response {
        try await withCheckedThrowingContinuation { cont in
            send(req, output: output) { r in
                if r.ok { cont.resume(returning: r) }
                else { cont.resume(throwing: ClientError.daemon(r.error ?? "unknown error")) }
            }
        }
    }

    private static func readLoop(_ c: Connection) {
        var buffer = Data()
        var chunk = [UInt8](repeating: 0, count: 65536)
        let decoder = JSONDecoder()
        while true {
            let n = read(c.fd, &chunk, chunk.count)
            if n < 0 && errno == EINTR { continue }
            if n <= 0 { break }
            buffer.append(chunk, count: n)
            if buffer.count > 64 << 20 { break }
            while let nl = buffer.firstIndex(of: 0x0A) {
                let line = buffer[buffer.startIndex..<nl]
                buffer.removeSubrange(buffer.startIndex...nl)
                if let r = try? decoder.decode(Response.self, from: line) { dispatch(r, c) }
            }
        }
        c.sendLock.lock()
        c.lock.lock()
        c.closed = true
        Darwin.close(c.fd)
        let waiting = c.pending
        c.pending.removeAll()
        c.progress.removeAll()
        let onDisconnect = c.onDisconnect
        c.lock.unlock()
        c.sendLock.unlock()
        c.queue.async {
            for (id, cb) in waiting {
                var r = Response(id: id, ok: false)
                r.error = ClientError.disconnected.errorDescription
                cb(r)
            }
            onDisconnect?()
        }
    }

    private static func dispatch(_ r: Response, _ c: Connection) {
        c.lock.lock()
        defer { c.lock.unlock() }
        if r.event == Event.output, let id = r.id {
            if let cb = c.progress[id], let line = r.output { c.queue.async { cb(line) } }
            return
        }
        if r.event != nil {
            if let cb = c.onEvent { c.queue.async { cb(r) } }
            return
        }
        guard let id = r.id, let cb = c.pending.removeValue(forKey: id) else { return }
        c.progress.removeValue(forKey: id)
        c.queue.async { cb(r) }
    }
}

extension Request {
    public static func make(_ cmd: String, _ fill: (inout Request) -> Void = { _ in }) -> Request {
        var r = Request(id: 0, cmd: cmd)
        fill(&r)
        return r
    }
}

public extension UInt64 {
    /// "1.2 GB" style sizes, as Finder shows them.
    var formattedBytes: String {
        ByteCountFormatter.string(fromByteCount: Int64(clamping: self), countStyle: .file)
    }
}
