// FSMac daemon: supervision of fsmac-worker processes (one per volume).

import Darwin
import Foundation

/// A running fsmac-worker. Events are delivered on the queue given to init,
/// after `start()`. `stop()`, `kill9()`, `hasExited` and `waitForExit` may be
/// used from any thread.
final class WorkerProcess {
    let pid: pid_t
    private let lock = NSLock()
    private var controlFD: Int32       // closing it asks the worker to stop
    private var exited = false
    private let exitSignal = DispatchSemaphore(value: 0)
    private let statusFD: Int32
    private let statusSource: DispatchSourceRead
    private let exitSource: DispatchSourceProcess
    private var buffer = Data()
    private(set) var exitStatus: Int32 = 0

    /// Called on `queue` for every JSON status event.
    var onEvent: ([String: Any]) -> Void = { _ in }
    /// Called once on `queue` when the process has exited.
    var onExit: (Int32) -> Void = { _ in }

    struct Options {
        var readOnly = false
        var uid: uid_t = 501
        var gid: gid_t = 20
        var mountPoint = ""
        var insecure = false
        var windowsNames = true
        var hideDotFiles = true
        var fileMode = "644"
        var keepOwnership = false
        var removeHibernation = false
        var dropToUser: String?
        var verbose = false
    }

    /// Spawns a worker serving the already-open device `deviceFD` on the
    /// listening socket `listenFD`. Both descriptors are only inherited by
    /// the child; the caller may close its copies afterwards. Call `start()`
    /// once the handlers are set.
    init(executable: String, deviceFD: Int32, listenFD: Int32, logPath: String,
         options o: Options, queue: DispatchQueue) throws {
        var statusPipe: [Int32] = [-1, -1]
        var controlPipe: [Int32] = [-1, -1]
        guard pipe(&statusPipe) == 0 else { throw POSIXError(.EMFILE) }
        guard pipe(&controlPipe) == 0 else {
            close(statusPipe[0]); close(statusPipe[1])
            throw POSIXError(.EMFILE)
        }
        // Move every descriptor the child inherits above the 3...6 range it
        // receives them in, so the dup2 actions below cannot clobber each
        // other whatever numbers they started with.
        func high(_ fd: Int32) -> Int32 { fcntl(fd, F_DUPFD_CLOEXEC, 10) }
        let src = [high(deviceFD), high(listenFD), high(statusPipe[1]), high(controlPipe[0])]
        let logFD = open(logPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0o644)
        defer {
            for fd in src where fd >= 0 { close(fd) }
            close(statusPipe[1])
            close(controlPipe[0])
            if logFD >= 0 { close(logFD) }
        }
        guard !src.contains(where: { $0 < 0 }) else {
            close(statusPipe[0]); close(controlPipe[1])
            throw POSIXError(.EMFILE)
        }

        var args = [executable, "--fd", "3", "--listen-fd", "4", "--status-fd", "5",
                    "--control-fd", "6", "--uid", String(o.uid), "--gid", String(o.gid),
                    "--mount-point", o.mountPoint, "--file-mode", o.fileMode]
        if o.readOnly { args.append("--readonly") }
        if o.insecure { args.append("--insecure") }
        if !o.windowsNames { args.append("--no-windows-names") }
        if !o.hideDotFiles { args.append("--no-hide-dotfiles") }
        if o.keepOwnership { args.append("--keep-ownership") }
        if o.removeHibernation { args.append("--ntfs-remove-hiberfile") }
        if let u = o.dropToUser { args += ["--user", u] }
        if o.verbose { args.append("-v") }

        var fa: posix_spawn_file_actions_t?
        posix_spawn_file_actions_init(&fa)
        defer { posix_spawn_file_actions_destroy(&fa) }
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0)
        if logFD >= 0 {
            posix_spawn_file_actions_adddup2(&fa, logFD, 1)
            posix_spawn_file_actions_adddup2(&fa, logFD, 2)
        } else {
            posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0)
            posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0)
        }
        for (i, fd) in src.enumerated() {
            posix_spawn_file_actions_adddup2(&fa, fd, Int32(3 + i))
        }

        var attr: posix_spawnattr_t?
        posix_spawnattr_init(&attr)
        defer { posix_spawnattr_destroy(&attr) }
        // Only the descriptors set up above are inherited. Own process group:
        // signals for the daemon's group (Ctrl-C, launchd) must not kill a
        // worker mid-flush; it stops by itself when the daemon goes away.
        posix_spawnattr_setflags(&attr, Int16(POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGDEF |
                                              POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP))
        posix_spawnattr_setpgroup(&attr, 0)
        var defaults = sigset_t()
        sigfillset(&defaults)
        posix_spawnattr_setsigdefault(&attr, &defaults)
        var empty = sigset_t()
        sigemptyset(&empty)
        posix_spawnattr_setsigmask(&attr, &empty)

        var cargs = args.map { strdup($0) } + [nil]
        defer { cargs.forEach { free($0) } }
        var child: pid_t = 0
        let rc = posix_spawn(&child, executable, &fa, &attr, &cargs, environ)
        guard rc == 0 else {
            close(statusPipe[0])
            close(controlPipe[1])
            throw POSIXError(POSIXErrorCode(rawValue: rc) ?? .EINVAL)
        }
        pid = child
        statusFD = statusPipe[0]
        controlFD = controlPipe[1]
        _ = fcntl(controlFD, F_SETFD, FD_CLOEXEC)
        _ = fcntl(statusFD, F_SETFD, FD_CLOEXEC)
        _ = fcntl(statusFD, F_SETFL, O_NONBLOCK)

        statusSource = DispatchSource.makeReadSource(fileDescriptor: statusFD, queue: queue)
        exitSource = DispatchSource.makeProcessSource(identifier: child, eventMask: .exit, queue: queue)
        let sfd = statusFD
        statusSource.setCancelHandler { close(sfd) }
    }

    /// Starts delivering events (call on the event queue, after setting the
    /// handlers, so that no event is missed).
    func start() {
        statusSource.setEventHandler { [weak self] in self?.readStatus() }
        // The exit handler keeps the object alive until the child is reaped.
        exitSource.setEventHandler { self.reap() }
        statusSource.resume()
        exitSource.resume()
    }

    private func readStatus() {
        var chunk = [UInt8](repeating: 0, count: 8192)
        while true {
            let n = read(statusFD, &chunk, chunk.count)
            if n <= 0 {
                if n == 0 && !statusSource.isCancelled { statusSource.cancel() }
                break
            }
            buffer.append(chunk, count: n)
            if buffer.count > 1 << 20 { buffer.removeAll() }   // runaway line
        }
        while let nl = buffer.firstIndex(of: 0x0A) {
            let line = buffer[buffer.startIndex..<nl]
            buffer.removeSubrange(buffer.startIndex...nl)
            if let obj = try? JSONSerialization.jsonObject(with: line) as? [String: Any] {
                onEvent(obj)
            }
        }
    }

    private func reap() {
        lock.lock()
        if exited {
            lock.unlock()
            return
        }
        var status: Int32 = 0
        waitpid(pid, &status, 0)
        // Set before `exited`, so that anyone who sees the exit also sees
        // the status.
        exitStatus = (status & 0x7f) == 0 ? (status >> 8) & 0xff : 128 + (status & 0x7f)
        exited = true
        if controlFD >= 0 {
            close(controlFD)
            controlFD = -1
        }
        lock.unlock()
        // Deliver any final status lines before reporting the exit.
        if !statusSource.isCancelled { readStatus() }
        exitSource.cancel()
        if !statusSource.isCancelled { statusSource.cancel() }
        exitSignal.signal()
        onExit(exitStatus)
        // Break the reference cycle through the handlers.
        onExit = { _ in }
        onEvent = { _ in }
        exitSource.setEventHandler {}
    }

    var hasExited: Bool {
        lock.lock()
        defer { lock.unlock() }
        return exited
    }

    /// Waits until the worker has been reaped. Never call it on the event
    /// queue, which does the reaping.
    func waitForExit(timeout: TimeInterval) -> Bool {
        if exitSignal.wait(timeout: .now() + timeout) == .success {
            exitSignal.signal()   // let other waiters through too
            return true
        }
        return false
    }

    /// Sends a command line on the control pipe (see nfs/server.c).
    func send(_ line: String) {
        lock.lock()
        defer { lock.unlock() }
        guard controlFD >= 0, !exited else { return }
        let bytes = Array((line.hasSuffix("\n") ? line : line + "\n").utf8)
        _ = bytes.withUnsafeBytes { write(controlFD, $0.baseAddress, $0.count) }
    }

    /// Asks the worker to flush everything and exit.
    func stop() {
        lock.lock()
        defer { lock.unlock() }
        if controlFD >= 0 {
            close(controlFD)
            controlFD = -1
        }
        if !exited { kill(pid, SIGTERM) }
    }

    func kill9() {
        lock.lock()
        defer { lock.unlock() }
        if !exited { kill(pid, SIGKILL) }
    }
}

/// Creates a listening TCP socket on 127.0.0.1 with an ephemeral port.
func makeLoopbackListener() throws -> (fd: Int32, port: UInt16) {
    let fd = socket(AF_INET, SOCK_STREAM, 0)
    guard fd >= 0 else { throw POSIXError(.EMFILE) }
    var addr = sockaddr_in()
    addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
    addr.sin_family = sa_family_t(AF_INET)
    addr.sin_addr.s_addr = inet_addr("127.0.0.1")
    addr.sin_port = 0
    let ok = withUnsafePointer(to: &addr) {
        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) == 0 && listen(fd, 16) == 0
        }
    }
    guard ok else {
        let e = errno
        close(fd)
        throw POSIXError(POSIXErrorCode(rawValue: e) ?? .EADDRINUSE)
    }
    var bound = sockaddr_in()
    var len = socklen_t(MemoryLayout<sockaddr_in>.size)
    _ = withUnsafeMutablePointer(to: &bound) {
        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &len) }
    }
    _ = fcntl(fd, F_SETFD, FD_CLOEXEC)
    return (fd, UInt16(bigEndian: bound.sin_port))
}
