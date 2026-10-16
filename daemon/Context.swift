// FSMac daemon: runtime context, logging and configuration.

import Foundation
import os
import SystemConfiguration

/// Where things live and how privileged operations are performed.
final class Context {
    /// System mode: running as root under launchd (normal operation).
    /// User mode: unprivileged development/testing daemon that only handles
    /// disks the user can open (attached disk images) and mounts them
    /// below ~/FSMacVolumes.
    let userMode: Bool
    let helpersDir: URL
    let socketPath: String
    let supportDir: URL
    let logDir: URL
    let mountRoot: URL
    let configURL: URL

    init(userMode: Bool) {
        self.userMode = userMode
        helpersDir = FSMac.executableURL.deletingLastPathComponent()
        if userMode {
            let home = URL(fileURLWithPath: NSHomeDirectory())
            supportDir = home.appendingPathComponent("Library/Application Support/FSMac")
            logDir = home.appendingPathComponent("Library/Logs/FSMac")
            mountRoot = home.appendingPathComponent("FSMacVolumes")
            socketPath = FSMac.userSocketPath
        } else {
            supportDir = URL(fileURLWithPath: FSMac.supportDirectory)
            logDir = URL(fileURLWithPath: FSMac.logDirectory)
            mountRoot = URL(fileURLWithPath: "/Volumes")
            socketPath = FSMac.systemSocketPath
        }
        configURL = supportDir.appendingPathComponent("config.json")
        for dir in [supportDir, logDir, mountRoot] {
            try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        }
    }

    func helper(_ name: String) -> String {
        helpersDir.appendingPathComponent(name).path
    }

    /// FSMac.app/Contents when running from the app bundle.
    var bundleContents: URL { helpersDir.deletingLastPathComponent() }

    /// The user logged in at the console (owner shown for mounted files).
    func consoleUser() -> (uid: uid_t, gid: gid_t) {
        if userMode {
            return (getuid(), getgid())
        }
        var uid: uid_t = 0
        var gid: gid_t = 0
        if SCDynamicStoreCopyConsoleUser(nil, &uid, &gid) != nil, uid != 0 {
            return (uid, gid)
        }
        return (501, 20)
    }
}

/// The Disk Arbitration filesystem bundle (see tools/fsmac-fsutil.c).
enum FilesystemBundle {
    static let installed = URL(fileURLWithPath: "/Library/Filesystems/fsmac_ext.fs")

    static func source(_ ctx: Context) -> URL {
        ctx.bundleContents.appendingPathComponent("Library/Filesystems/fsmac_ext.fs")
    }

    private static func version(_ url: URL) -> String? {
        (NSDictionary(contentsOf: url.appendingPathComponent("Contents/Info.plist"))?["CFBundleVersion"]) as? String
    }

    /// Installs or updates the bundle (system mode only).
    static func install(_ ctx: Context) {
        let src = source(ctx)
        guard let want = version(src) else { return }   // not running from the app
        if version(installed) == want { return }
        let fm = FileManager.default
        do {
            try? fm.createDirectory(at: installed.deletingLastPathComponent(), withIntermediateDirectories: true)
            if fm.fileExists(atPath: installed.path) { try fm.removeItem(at: installed) }
            try fm.copyItem(at: src, to: installed)
            Log.info("installed \(installed.path) (\(want))")
        } catch {
            Log.error("cannot install \(installed.path): \(error.localizedDescription)")
        }
    }

    static func remove() {
        if FileManager.default.fileExists(atPath: installed.path) {
            try? FileManager.default.removeItem(at: installed)
            Log.info("removed \(installed.path)")
        }
    }
}

/// Logging to the unified log and to a plain file for diagnostics.
enum Log {
    private static let logger = Logger(subsystem: "com.fsmac", category: "daemon")
    private static var file: FileHandle?
    private static let lock = NSLock()
    private static let fmt: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH:mm:ss"
        return f
    }()

    static func open(_ url: URL) {
        if !FileManager.default.fileExists(atPath: url.path) {
            FileManager.default.createFile(atPath: url.path, contents: nil)
        }
        // Keep the log from growing without bound.
        if let size = (try? FileManager.default.attributesOfItem(atPath: url.path))?[.size] as? UInt64,
           size > 8 << 20 {
            let old = url.deletingPathExtension().appendingPathExtension("old.log")
            try? FileManager.default.removeItem(at: old)
            try? FileManager.default.moveItem(at: url, to: old)
            FileManager.default.createFile(atPath: url.path, contents: nil)
        }
        file = try? FileHandle(forWritingTo: url)
        file?.seekToEndOfFile()
    }

    static func write(_ level: String, _ msg: String) {
        let line = "\(fmt.string(from: Date())) [\(level)] \(msg)\n"
        lock.lock()
        file?.write(line.data(using: .utf8)!)
        lock.unlock()
        FileHandle.standardError.write(line.data(using: .utf8)!)
    }

    static func info(_ msg: String) {
        logger.info("\(msg, privacy: .public)")
        write("info", msg)
    }

    static func warn(_ msg: String) {
        logger.warning("\(msg, privacy: .public)")
        write("warn", msg)
    }

    static func error(_ msg: String) {
        logger.error("\(msg, privacy: .public)")
        write("error", msg)
    }
}

/// Persistent settings.
final class Config {
    private let url: URL
    private(set) var settings: Settings

    init(url: URL) {
        self.url = url
        if let data = try? Data(contentsOf: url),
           let s = try? JSONDecoder().decode(Settings.self, from: data) {
            settings = s
        } else {
            settings = Settings()
        }
    }

    func update(_ s: Settings) {
        settings = s
        save()
    }

    func setPolicy(uuid: String, policy: VolumePolicy?) {
        settings.volumes[uuid] = policy
        save()
    }

    private func save() {
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        do {
            try enc.encode(settings).write(to: url, options: .atomic)
        } catch {
            Log.error("cannot save settings: \(error.localizedDescription)")
        }
    }
}

/// Runs a helper program and captures its output.
struct ProcessResult {
    var status: Int32
    var output: String
}

func runProgram(_ path: String, _ args: [String], timeout: TimeInterval = 600,
                onLine: ((String) -> Void)? = nil) -> ProcessResult {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: path)
    p.arguments = args
    let pipe = Pipe()
    p.standardOutput = pipe
    p.standardError = pipe
    p.standardInput = FileHandle.nullDevice
    do {
        try p.run()
    } catch {
        return ProcessResult(status: -1, output: "cannot run \(path): \(error.localizedDescription)")
    }
    // One reader drains the pipe in order until EOF, splitting lines.
    var output = Data()
    let lock = NSLock()
    let done = DispatchSemaphore(value: 0)
    let reader = pipe.fileHandleForReading
    Thread.detachNewThread {
        var partial = Data()
        while true {
            let d = reader.availableData
            if d.isEmpty { break }
            lock.lock()
            output.append(d)
            lock.unlock()
            guard let onLine else { continue }
            partial.append(d)
            while let nl = partial.firstIndex(of: 0x0A) {
                let line = partial[partial.startIndex..<nl]
                partial.removeSubrange(partial.startIndex...nl)
                onLine(String(decoding: line, as: UTF8.self))
            }
        }
        if let onLine, !partial.isEmpty { onLine(String(decoding: partial, as: UTF8.self)) }
        done.signal()
    }
    let deadline = Date().addingTimeInterval(timeout)
    while p.isRunning {
        if Date() > deadline {
            p.terminate()
            Thread.sleep(forTimeInterval: 2)
            if p.isRunning { kill(p.processIdentifier, SIGKILL) }
            break
        }
        Thread.sleep(forTimeInterval: 0.05)
    }
    p.waitUntilExit()
    // A grandchild could keep the pipe open; don't wait for it forever.
    _ = done.wait(timeout: .now() + 5)
    lock.lock()
    let text = String(decoding: output, as: UTF8.self)
    lock.unlock()
    return ProcessResult(status: p.terminationStatus, output: text)
}
