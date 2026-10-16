// fsmac: command-line interface to FSMac.

import Foundation

let usage = """
usage: fsmac <command> [options]

Volumes:
  list [--json]                       show NTFS and ext volumes (default command)
  mount <volume> [--read-only] [--remove-hibernation]
  unmount <volume> [--force] [--eject]
  eject <volume|disk>                 unmount everything on the disk and eject it
  check <volume> [--repair]           check (or repair) the filesystem
  format <disk> --type ntfs|ext4 [--label NAME] [--force] [--yes]
                                      erase a partition or a whole disk
  policy <volume> read-write|read-only|ignore|default [--no-auto-mount]
                                      remember how to handle this volume

Settings:
  config                              show settings
  config <key> <value>                change a setting
  watch                               print volume events as they happen

Service:
  status                              show whether the FSMac helper is running
  restart-helper                      unmount everything and restart the helper
  install-helper                      install the helper as a LaunchDaemon (sudo)
  uninstall-helper                    remove it again (sudo)
  log                                 show the helper's log

<volume> is a device (disk4s1), a volume name, or a mount point.
"""

func fail(_ msg: String, code: Int32 = 1) -> Never {
    FileHandle.standardError.write("fsmac: \(msg)\n".data(using: .utf8)!)
    exit(code)
}

var args = Array(CommandLine.arguments.dropFirst())
func flag(_ names: String...) -> Bool {
    if let i = args.firstIndex(where: { names.contains($0) }) {
        args.remove(at: i)
        return true
    }
    return false
}
func option(_ name: String) -> String? {
    if let i = args.firstIndex(of: name), i + 1 < args.count {
        let v = args[i + 1]
        args.removeSubrange(i...(i + 1))
        return v
    }
    return nil
}

let clientQueue = DispatchQueue(label: "fsmac.cli")

func connect() -> DaemonClient {
    do {
        let c = try DaemonClient(queue: clientQueue)
        c.onDisconnect = { fail("lost the connection to the FSMac helper") }
        return c
    } catch {
        fail("""
            the FSMac helper is not running.
            Open FSMac.app to set it up, or run `sudo fsmac install-helper`.
            """)
    }
}

/// Sends a request and waits for its response.
@discardableResult
func call(_ c: DaemonClient, _ req: Request, output: ((String) -> Void)? = nil) -> Response {
    let sem = DispatchSemaphore(value: 0)
    var result: Response!
    c.send(req, output: output) { r in
        result = r
        sem.signal()
    }
    sem.wait()
    if !result.ok { fail(result.error ?? "failed") }
    return result
}

func volumes(_ c: DaemonClient) -> [VolumeInfo] {
    call(c, .make(Command.list)).volumes ?? []
}

func resolve(_ c: DaemonClient, _ name: String?) -> VolumeInfo {
    guard let name else { fail("missing volume\n\n\(usage)", code: 2) }
    let all = volumes(c)
    let n = name.hasPrefix("/dev/") ? String(name.dropFirst(5)) : name
    let path = (name as NSString).standardizingPath
    if let v = all.first(where: { $0.id == n || $0.id == n.replacingOccurrences(of: "rdisk", with: "disk") }) {
        return v
    }
    if let v = all.first(where: { $0.mountPoint == path }) { return v }
    let byLabel = all.filter { $0.label.caseInsensitiveCompare(name) == .orderedSame }
    if byLabel.count == 1 { return byLabel[0] }
    if byLabel.count > 1 { fail("several volumes are called “\(name)”; use the device name") }
    fail("no NTFS or ext volume “\(name)” (see `fsmac list`)")
}

func stateText(_ v: VolumeInfo) -> String {
    switch v.state {
    case .mounted: return v.readOnly ? "mounted (read-only)" : "mounted"
    case .mountedByMacOS: return "mounted by macOS (read-only)"
    case .unmounted: return v.policy.mode == .ignore ? "ignored" : "not mounted"
    case .checking: return "checking"
    case .mounting: return "mounting"
    case .unmounting: return "unmounting"
    case .error: return "error"
    }
}

func pad(_ s: String, _ n: Int) -> String {
    s.count >= n ? s : s + String(repeating: " ", count: n - s.count)
}

func printList(_ vols: [VolumeInfo]) {
    if vols.isEmpty {
        print("No NTFS or ext volumes connected.")
        return
    }
    let rows = vols.map { v -> [String] in
        let size: String
        if let t = v.totalBytes, let f = v.freeBytes, v.state == .mounted {
            size = "\(f.formattedBytes) free of \(t.formattedBytes)"
        } else {
            size = v.size.formattedBytes
        }
        return [v.id, v.fsDisplayName, v.displayName, size, stateText(v), v.mountPoint ?? ""]
    }
    let header = ["DEVICE", "TYPE", "NAME", "SIZE", "STATE", "MOUNT POINT"]
    var widths = header.map { $0.count }
    for r in rows { for (i, c) in r.enumerated() { widths[i] = max(widths[i], c.count) } }
    print(header.enumerated().map { pad($1, widths[$0]) }.joined(separator: "  ")
        .trimmingCharacters(in: .whitespaces))
    for (r, v) in zip(rows, vols) {
        print(r.enumerated().map { pad($1, widths[$0]) }.joined(separator: "  ")
            .trimmingCharacters(in: .whitespaces))
        for msg in [v.error, v.notice].compactMap({ $0 }) {
            print("    ! \(msg)")
        }
    }
}

func printJSON<T: Encodable>(_ value: T) {
    let enc = JSONEncoder()
    enc.outputFormatting = [.prettyPrinted, .sortedKeys]
    print(String(data: try! enc.encode(value), encoding: .utf8)!)
}

// MARK: - Helper installation

let plistPath = "/Library/LaunchDaemons/\(FSMac.daemonLabel).plist"

func helperPath() -> String {
    // Inside FSMac.app the CLI lives next to the daemon in Contents/Helpers.
    let sibling = FSMac.executableURL.deletingLastPathComponent().appendingPathComponent("fsmacd").path
    if FileManager.default.isExecutableFile(atPath: sibling) { return sibling }
    return "/Applications/FSMac.app/Contents/Helpers/fsmacd"
}

func installHelper() {
    guard getuid() == 0 else { fail("install-helper must be run with sudo") }
    let daemon = helperPath()
    guard FileManager.default.isExecutableFile(atPath: daemon) else { fail("cannot find \(daemon)") }
    let plist: [String: Any] = [
        "Label": FSMac.daemonLabel,
        "ProgramArguments": [daemon],
        "RunAtLoad": true,
        "KeepAlive": true,
        "ProcessType": "Interactive",
        "ExitTimeOut": 120,
        "StandardErrorPath": "/Library/Logs/FSMac/fsmacd.stderr.log",
        "AssociatedBundleIdentifiers": [FSMac.appBundleID],
    ]
    try? FileManager.default.createDirectory(atPath: "/Library/Logs/FSMac", withIntermediateDirectories: true)
    _ = runCommand("/bin/launchctl", ["bootout", "system/\(FSMac.daemonLabel)"])
    let data = try! PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0)
    do {
        try data.write(to: URL(fileURLWithPath: plistPath))
    } catch {
        fail("cannot write \(plistPath): \(error.localizedDescription)")
    }
    chmod(plistPath, 0o644)
    let (status, out) = runCommand("/bin/launchctl", ["bootstrap", "system", plistPath])
    guard status == 0 else { fail("launchctl bootstrap failed: \(out)") }
    print("Installed and started the FSMac helper (\(daemon)).")
}

func uninstallHelper() {
    guard getuid() == 0 else { fail("uninstall-helper must be run with sudo") }
    // launchd sends SIGTERM; the helper unmounts volumes before exiting.
    _ = runCommand("/bin/launchctl", ["bootout", "system/\(FSMac.daemonLabel)"])
    try? FileManager.default.removeItem(atPath: plistPath)
    try? FileManager.default.removeItem(atPath: "/Library/Filesystems/fsmac_ext.fs")
    print("Removed the FSMac helper.")
}

@discardableResult
func runCommand(_ path: String, _ arguments: [String]) -> (Int32, String) {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: path)
    p.arguments = arguments
    let pipe = Pipe()
    p.standardOutput = pipe
    p.standardError = pipe
    do { try p.run() } catch { return (-1, error.localizedDescription) }
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    p.waitUntilExit()
    return (p.terminationStatus, String(data: data, encoding: .utf8) ?? "")
}

// MARK: - Commands

let command = args.isEmpty ? "list" : args.removeFirst()
switch command {
case "list", "ls":
    let json = flag("--json")
    let c = connect()
    let vols = volumes(c)
    if json { printJSON(vols) } else { printList(vols) }

case "mount":
    let ro = flag("--read-only", "-r")
    let hib = flag("--remove-hibernation")
    let c = connect()
    let v = resolve(c, args.first)
    call(c, .make(Command.mount) {
        $0.disk = v.id
        $0.readOnly = ro
        $0.removeHibernation = hib
    })
    let after = resolve(c, v.id)
    print("Mounted “\(after.displayName)” at \(after.mountPoint ?? "?")\(after.readOnly ? " (read-only)" : "").")
    if let n = after.notice { print("Note: \(n)") }

case "unmount", "umount":
    let force = flag("--force", "-f")
    let eject = flag("--eject")
    let c = connect()
    let v = resolve(c, args.first)
    call(c, .make(Command.unmount) {
        $0.disk = v.id
        $0.force = force
        $0.eject = eject
    })
    print(eject ? "Ejected “\(v.displayName)”." : "Unmounted “\(v.displayName)”.")

case "eject":
    guard let name = args.first else { fail("missing volume or disk", code: 2) }
    let c = connect()
    let n = name.hasPrefix("/dev/") ? String(name.dropFirst(5)) : name
    let disk = n.range(of: #"^disk\d+$"#, options: .regularExpression) != nil ? n : resolve(c, name).id
    call(c, .make(Command.eject) { $0.disk = disk })
    print("Ejected \(disk). It is safe to disconnect it.")

case "check", "fsck":
    let repair = flag("--repair")
    let c = connect()
    let v = resolve(c, args.first)
    print("\(repair ? "Repairing" : "Checking") “\(v.displayName)” (\(v.id))…")
    let sem = DispatchSemaphore(value: 0)
    var res: Response!
    c.send(.make(Command.check) { $0.disk = v.id; $0.repair = repair },
           output: { print($0) }) { res = $0; sem.signal() }
    sem.wait()
    clientQueue.sync {}
    if res.ok {
        print(res.message ?? "Done.")
    } else {
        fail(res.error ?? "check failed")
    }

case "format", "erase":
    let type = option("--type") ?? option("-t")
    let label = option("--label") ?? option("-L") ?? "Untitled"
    let force = flag("--force")
    let yes = flag("--yes", "-y")
    guard let target = args.first else { fail("missing disk", code: 2) }
    guard let type, ["ntfs", "ext4"].contains(type) else { fail("--type must be ntfs or ext4", code: 2) }
    let disk = target.hasPrefix("/dev/") ? String(target.dropFirst(5)) : target
    if !yes {
        print("This erases ALL data on \(disk). Type the device name to continue: ", terminator: "")
        guard readLine()?.trimmingCharacters(in: .whitespaces) == disk else { fail("cancelled") }
    }
    let c = connect()
    let sem = DispatchSemaphore(value: 0)
    var res: Response!
    c.send(.make(Command.format) {
        $0.disk = disk
        $0.fsType = type
        $0.label = label
        $0.force = force
    }, output: { print($0) }) { res = $0; sem.signal() }
    sem.wait()
    clientQueue.sync {}
    if !res.ok { fail(res.error ?? "format failed") }

case "policy":
    guard args.count >= 2 else { fail("usage: fsmac policy <volume> read-write|read-only|ignore|default", code: 2) }
    let noAuto = flag("--no-auto-mount")
    let c = connect()
    let v = resolve(c, args[0])
    let mode: MountMode?
    switch args[1] {
    case "read-write", "rw": mode = .readWrite
    case "read-only", "ro": mode = .readOnly
    case "ignore", "none": mode = .ignore
    case "default": mode = nil
    default: fail("unknown mode \(args[1])", code: 2)
    }
    call(c, .make(Command.setPolicy) {
        $0.uuid = v.uuid
        $0.policy = mode.map { VolumePolicy(mode: $0, autoMount: !noAuto) }
    })
    print(mode == nil ? "“\(v.displayName)” follows the default settings again."
                      : "“\(v.displayName)”: \(mode!.title)\(noAuto ? ", not mounted automatically" : "").")

case "config", "settings":
    let c = connect()
    var s = call(c, .make(Command.getSettings)).settings ?? Settings()
    if args.isEmpty {
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        print(String(data: try! enc.encode(s), encoding: .utf8)!)
        break
    }
    guard args.count == 2 else { fail("usage: fsmac config <key> <value>", code: 2) }
    let (key, value) = (args[0], args[1])
    func bool() -> Bool {
        switch value.lowercased() {
        case "1", "yes", "true", "on": return true
        case "0", "no", "false", "off": return false
        default: fail("\(key) takes yes or no", code: 2)
        }
    }
    func mode() -> MountMode {
        switch value {
        case "read-write", "rw", "readWrite": return .readWrite
        case "read-only", "ro", "readOnly": return .readOnly
        case "ignore", "none": return .ignore
        default: fail("\(key) takes read-write, read-only or ignore", code: 2)
        }
    }
    switch key {
    case "ntfsMode": s.ntfsMode = mode()
    case "extMode": s.extMode = mode()
    case "autoMount": s.autoMount = bool()
    case "autoRepairExt": s.autoRepairExt = bool()
    case "windowsCompatibleNames": s.windowsCompatibleNames = bool()
    case "hideDotFilesOnNTFS": s.hideDotFilesOnNTFS = bool()
    case "keepExtOwnership": s.keepExtOwnership = bool()
    case "ejectWhenUnmounted": s.ejectWhenUnmounted = bool()
    case "ntfsFileMode":
        guard value.range(of: "^[0-7]{3}$", options: .regularExpression) != nil else {
            fail("ntfsFileMode takes an octal mode such as 644", code: 2)
        }
        s.ntfsFileMode = value
    default: fail("unknown setting \(key)", code: 2)
    }
    call(c, .make(Command.setSettings) { $0.settings = s })
    print("\(key) = \(value)")

case "watch":
    let c = connect()
    c.onEvent = { ev in
        let t = ISO8601DateFormatter().string(from: Date())
        switch ev.event {
        case Event.volumes:
            print("[\(t)] volumes:")
            for v in ev.volumes ?? [] {
                print("    \(v.id) \(v.fsDisplayName) “\(v.displayName)”: \(stateText(v)) \(v.mountPoint ?? "")")
            }
        case Event.notify:
            print("[\(t)] \(ev.title ?? ""): \(ev.message ?? "")")
        case Event.log:
            print("[\(t)] \(ev.title ?? "log"): \(ev.message ?? "")")
        default:
            break
        }
    }
    let r = call(c, .make(Command.subscribe))
    printList(r.volumes ?? [])
    dispatchMain()

case "status":
    do {
        let c = try DaemonClient(queue: clientQueue)
        let r = call(c, .make(Command.hello))
        print("FSMac helper \(r.version ?? "?") is running\(r.userMode == true ? " in user (development) mode" : "").")
        print("Socket: \(FSMac.socketPath)")
    } catch {
        print("The FSMac helper is not running.")
        exit(1)
    }

case "restart-helper":
    let c = connect()
    c.onDisconnect = nil
    call(c, .make(Command.restart))
    print("The helper is restarting.")

case "install-helper":
    installHelper()

case "uninstall-helper":
    uninstallHelper()

case "log", "logs":
    let paths = ["/Library/Logs/FSMac/fsmacd.log",
                 NSHomeDirectory() + "/Library/Logs/FSMac/fsmacd.log"]
    guard let p = paths.first(where: { FileManager.default.fileExists(atPath: $0) }) else {
        fail("no log found")
    }
    let t = Process()
    t.executableURL = URL(fileURLWithPath: "/usr/bin/tail")
    t.arguments = flag("-f") ? ["-n", "100", "-f", p] : ["-n", "100", p]
    try? t.run()
    t.waitUntilExit()

case "version", "--version":
    print("fsmac \(FSMac.version)")

case "help", "-h", "--help":
    print(usage)

default:
    fail("unknown command \(command)\n\n\(usage)", code: 2)
}
exit(0)
