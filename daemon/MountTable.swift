// FSMac daemon: NFS mounts of our workers and the system mount table.

import Darwin
import Foundation

/// Source path of an FSMac mount ("127.0.0.1:/fsmac/disk4s1/My Disk"). The
/// prefix identifies our mounts in the mount table; macOS takes the volume
/// name shown in Finder from the last component.
func nfsSource(bsd: String, label: String) -> String {
    var name = label.isEmpty ? "Untitled" : label
    name = name.replacingOccurrences(of: "/", with: ":")
    return "127.0.0.1:/fsmac/\(bsd)/\(name)"
}

struct MountEntry {
    var from: String
    var on: String
    var type: String
}

func currentMounts() -> [MountEntry] {
    var buf: UnsafeMutablePointer<statfs>?
    let n = getmntinfo(&buf, MNT_NOWAIT)
    guard n > 0, let buf else { return [] }
    var out: [MountEntry] = []
    for i in 0..<Int(n) {
        var s = buf[i]
        let from = withUnsafeBytes(of: &s.f_mntfromname) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
        let on = withUnsafeBytes(of: &s.f_mntonname) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
        let type = withUnsafeBytes(of: &s.f_fstypename) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
        out.append(MountEntry(from: from, on: on, type: type))
    }
    return out
}

/// Mount options for the macOS NFS client. Notes:
///  - hard mounts: a long-running operation (e.g. deleting a huge file) must
///    never turn into an I/O error; if a worker dies the daemon force
///    unmounts instead.
///  - nordirplus: the client otherwise answers lookups of files missing from
///    a cached READDIRPLUS listing negatively, which would hide the virtual
///    AppleDouble companions that carry extended attributes.
///  - nfc: names are sent in NFC, the normal form on Windows and Linux.
///  - resvport: workers only talk to privileged ports, i.e. to root - the
///    kernel's NFS client - so other local users can't speak NFS to them
///    with forged credentials. Not the default on recent macOS, and needs
///    root; the unprivileged development mode uses noresvport instead.
func nfsMountOptions(port: UInt16, userMode: Bool, readOnly: Bool) -> String {
    var o = ["vers=3", "tcp", "port=\(port)", "mountport=\(port)", "hard", "intr",
             "timeo=600", "retrans=10", "deadtimeout=0", "locallocks", "nfc",
             "rsize=1048576", "wsize=1048576", "readahead=32", "nordirplus",
             "nodev", "nosuid", "mutejukebox"]
    o.append(userMode ? "noresvport" : "resvport")
    if readOnly { o.append("rdonly") }
    return o.joined(separator: ",")
}

/// Picks /Volumes/<label>, adding " 1", " 2", ... if taken or `reserved`.
func chooseMountPoint(root: URL, label: String, reserved: Set<String>) -> URL {
    var base = label.isEmpty ? "Untitled" : label
    base = base.replacingOccurrences(of: "/", with: ":")
    if base.hasPrefix(".") { base = "_" + base.dropFirst() }
    let fm = FileManager.default
    let mounted = Set(currentMounts().map { $0.on })
    for i in 0..<100 {
        let name = i == 0 ? base : "\(base) \(i)"
        let url = root.appendingPathComponent(name)
        if mounted.contains(url.path) || reserved.contains(url.path) { continue }
        var isDir: ObjCBool = false
        if !fm.fileExists(atPath: url.path, isDirectory: &isDir) { return url }
        if isDir.boolValue, (try? fm.contentsOfDirectory(atPath: url.path))?.isEmpty == true {
            return url
        }
    }
    return root.appendingPathComponent("\(base) \(UUID().uuidString.prefix(4))")
}

func mountNFS(bsd: String, label: String, port: UInt16, at mountPoint: URL, userMode: Bool,
              readOnly: Bool) -> String? {
    do {
        try FileManager.default.createDirectory(at: mountPoint, withIntermediateDirectories: true)
    } catch {
        return "cannot create \(mountPoint.path): \(error.localizedDescription)"
    }
    let r = runProgram("/sbin/mount_nfs",
                       ["-o", nfsMountOptions(port: port, userMode: userMode, readOnly: readOnly),
                        nfsSource(bsd: bsd, label: label), mountPoint.path], timeout: 60)
    if r.status != 0 {
        removeMountPoint(mountPoint.path)
        return r.output.trimmingCharacters(in: .whitespacesAndNewlines)
    }
    return nil
}

/// Unmounts a path. Returns nil or an error description.
func unmountPath(_ path: String, force: Bool) -> String? {
    if unmount(path, force ? MNT_FORCE : 0) == 0 { return nil }
    let e = errno
    if e == EBUSY { return "busy" }
    if e == EINVAL || e == ENOENT { return nil } // not mounted any more
    return String(cString: strerror(e))
}

/// Removes a mount point directory - only if it is empty and nothing is
/// mounted on it.
func removeMountPoint(_ path: String) {
    if currentMounts().contains(where: { $0.on == path }) { return }
    rmdir(path)
}

/// Lists the processes that have files open on a volume (best effort).
func processesUsing(_ path: String) -> [String] {
    let r = runProgram("/usr/sbin/lsof", ["-F", "c", "+f", "--", path], timeout: 10)
    var names: [String] = []
    for line in r.output.split(separator: "\n") where line.hasPrefix("c") {
        let n = String(line.dropFirst())
        if !names.contains(n) { names.append(n) }
    }
    return names
}

/// Notifies about mount and unmount events anywhere in the system.
final class MountWatcher {
    private let kq: Int32
    private let source: DispatchSourceRead
    var onChange: () -> Void = {}

    init(queue: DispatchQueue) {
        kq = kqueue()
        var ev = kevent(ident: 0, filter: Int16(EVFILT_FS), flags: UInt16(EV_ADD | EV_CLEAR),
                        fflags: 0, data: 0, udata: nil)
        _ = kevent(kq, &ev, 1, nil, 0, nil)
        source = DispatchSource.makeReadSource(fileDescriptor: kq, queue: queue)
        source.setEventHandler { [weak self] in
            guard let self else { return }
            var out = kevent()
            var ts = timespec(tv_sec: 0, tv_nsec: 0)
            while kevent(self.kq, nil, 0, &out, 1, &ts) > 0 {}
            self.onChange()
        }
        source.resume()
    }
}
