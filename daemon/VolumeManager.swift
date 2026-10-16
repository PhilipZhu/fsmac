// FSMac daemon: volume discovery, policy and the mount/unmount lifecycle.
//
// Threading: all state lives on the serial queue `q`. Disk Arbitration
// notifications, worker events and IPC requests are delivered there, and
// nothing running on `q` may block on I/O, on a worker or on Disk
// Arbitration. Slow work (probing, fsck, starting workers, mount_nfs,
// unmounting) runs on `work` and touches state only through `q.sync`/
// `q.async`. Disk Arbitration approval callbacks run on DiskWatcher's own
// queue and may block there in the same way as `work`.

import DiskArbitration
import Foundation

final class Volume {
    var info: VolumeInfo
    var worker: WorkerProcess?
    var readyEvent: [String: Any]?
    var readySignal: DispatchSemaphore?
    var lastWorkerError: String?
    /// An operation is in progress; others are refused until it ends.
    var busy = false
    /// The user unmounted it: don't mount it again automatically.
    var userUnmounted = false
    /// Let macOS mount it with its own (read-only) driver.
    var nativeAllowed = false
    /// Read-only was asked for explicitly (kept when remounting).
    var requestedReadOnly = false
    /// The disk was disconnected.
    var gone = false
    /// Mount point of a mount in progress (not yet in `info`).
    var pendingMountPoint: String?
    /// Abandon the mount in progress.
    var cancelMount = false

    init(info: VolumeInfo) {
        self.info = info
    }
}

enum ProbeOutcome {
    case found(ProbeResult)
    case notOurs
    /// macOS refused to let us read the disk (privacy protection).
    case denied
}

struct ProbeResult {
    var fsType: String
    var label: String
    var uuid: String
    var clean = true
    var errors = false
    var needsRecovery = false
}

final class VolumeManager {
    let ctx: Context
    let config: Config
    let q = DispatchQueue(label: "com.fsmac.state")
    let work = DispatchQueue(label: "com.fsmac.work", attributes: .concurrent)
    /// statfs on our own mounts can block for as long as a worker is busy.
    private let spaceQueue = DispatchQueue(label: "com.fsmac.space", attributes: .concurrent)
    private var watcher: DiskWatcher!
    private var mountWatcher: MountWatcher!
    private var consoleWatcher: ConsoleUserWatcher?
    private(set) var volumes: [String: Volume] = [:]
    private var probing: Set<String> = []
    /// Mount points handed out but not (or no longer) in the mount table.
    private var reserved: Set<String> = []
    /// NTFS volumes we could not handle; macOS may mount them.
    private var releasedNative: Set<String> = []
    /// Whole disks we are ejecting ourselves.
    private var ejecting: Set<String> = []
    /// Volumes whose free space is being read (statfs may block).
    private var spaceInFlight: Set<String> = []
    /// Bumped whenever a BSD name disappears, to drop stale probe results.
    private var generation: [String: Int] = [:]
    /// Disks macOS would not let us read (Full Disk Access missing), retried
    /// until access is granted.
    private var unreadable: [String: DiskDescription] = [:]
    private var accessRetry: DispatchSourceTimer?
    private var accessRetryRunning = false
    private(set) var shuttingDown = false

    /// Volume list changed (called on q).
    var onChange: () -> Void = {}
    /// Something worth telling the user (title, body); called on q.
    var onMessage: (String, String) -> Void = { _, _ in }
    /// Worker log line (level, message) for subscribed clients.
    var onLog: (String, String) -> Void = { _, _ in }

    init(ctx: Context, config: Config) {
        self.ctx = ctx
        self.config = config
    }

    var settings: Settings { config.settings }

    func start() {
        q.sync {
            cleanupStaleMounts()
            watcher = DiskWatcher(queue: q)
            watcher.onAppeared = { [weak self] d in self?.diskAppeared(d) }
            watcher.onDisappeared = { [weak self] d in self?.diskDisappeared(d) }
            watcher.onChanged = { [weak self] d in self?.diskChanged(d) }
            watcher.shouldBlockMount = { [weak self] d in
                guard let self else { return nil }
                return self.q.sync { self.shouldBlockMount(d) }
            }
            watcher.shouldBlockEject = { [weak self] d in self?.approveEject(d) }
            watcher.start()
            mountWatcher = MountWatcher(queue: q)
            mountWatcher.onChange = { [weak self] in self?.checkMountTable() }
            if !ctx.userMode {
                consoleWatcher = ConsoleUserWatcher(queue: q) { [weak self] in self?.consoleUserChanged() }
            }
        }
    }

    /// Files are shown as owned by the console user; follow logins, logouts
    /// and fast user switching.
    private func consoleUserChanged() {
        let user = ctx.consoleUser()
        for v in volumes.values {
            v.worker?.send("owner \(user.uid) \(user.gid)")
        }
        Log.info("console user is now \(user.uid)")
    }

    func snapshot() -> [VolumeInfo] {
        let blocked = unreadable.values.compactMap { d -> VolumeInfo? in
            guard let type = likelyType(d) else { return nil }
            return VolumeInfo(
                id: d.bsdName, wholeDisk: d.wholeBSDName, fsType: type,
                label: d.volumeName ?? "", uuid: "", size: d.size,
                deviceName: d.deviceModel.isEmpty ? d.mediaName : d.deviceModel,
                isInternal: d.isInternal, isEjectable: d.isEjectable || d.isRemovable,
                isWritableMedia: d.isWritable,
                state: d.volumePath != nil ? .mountedByMacOS : .error,
                mountPoint: d.volumePath, readOnly: true, notice: nil,
                error: "macOS doesn't allow FSMac to read this disk yet. Give FSMac Full Disk Access.",
                totalBytes: nil, freeBytes: nil,
                policy: settings.policy(fsType: type, uuid: ""), needsPermission: true)
        }
        return (volumes.values.map { $0.info } + blocked)
            .sorted { ($0.wholeDisk, $0.id) < ($1.wholeDisk, $1.id) }
    }

    /// What an unreadable partition probably holds, from macOS's own probe
    /// and the partition type; nil if it is probably not ours.
    private func likelyType(_ d: DiskDescription) -> String? {
        switch d.volumeKind {
        case "ntfs": return "ntfs"
        case "fsmac_ext": return "ext4"
        case nil:
            let linux: Set<String> = ["Linux", "Linux_Ext2FS", "0FC63DAF-8483-4772-8E79-3D69D8477DE4",
                                      "933AC7E1-2EB4-4F13-B844-0E14E2AEF915", "3B8F8425-20E0-4F3B-907F-1A25A76F98E8",
                                      "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709", "B921B045-1DF0-41C3-AF44-4C6F280D3FAE"]
            return linux.contains(d.content) ? "ext4" : nil
        default: return nil
        }
    }

    /// True when a disk is waiting for Full Disk Access.
    var needsDiskAccess: Bool { unreadable.values.contains { likelyType($0) != nil } }

    private func changed() {
        onChange()
    }

    // MARK: - Discovery

    private func cleanupStaleMounts() {
        for m in currentMounts() where m.from.hasPrefix("127.0.0.1:/fsmac/") {
            Log.warn("removing stale mount \(m.on) from a previous session")
            _ = unmountPath(m.on, force: true)
            removeMountPoint(m.on)
        }
    }

    /// `stale`: Disk Arbitration's description predates a change we made
    /// (formatting), so its volume name must not be trusted.
    private func diskAppeared(_ disk: DADisk, stale: Bool = false) {
        guard !shuttingDown, let d = DiskDescription(disk), !d.isNetwork, d.size > 0 else { return }
        if d.isWhole && !d.isLeaf { return }          // partitioned disk
        guard d.mayHoldSupportedFS else { return }
        if let v = volumes[d.bsdName] {
            refreshNative(v, d)
            return
        }
        guard !probing.contains(d.bsdName) else { return }
        probing.insert(d.bsdName)
        let gen = generation[d.bsdName] ?? 0
        work.async { [weak self] in
            guard let self else { return }
            let result = self.probe(d, trustVolumeName: !stale)
            self.q.async {
                self.probing.remove(d.bsdName)
                guard !self.shuttingDown else { return }
                if self.generation[d.bsdName] ?? 0 != gen {
                    // The disk went away while it was probed. If a disk with
                    // the same name is back, look at that one afresh.
                    if let disk = self.watcher.disk(bsdName: d.bsdName), DiskDescription(disk) != nil {
                        self.diskAppeared(disk)
                    }
                    return
                }
                switch result {
                case .found(let r):
                    self.register(d, r)
                case .notOurs, .denied:
                    // Not something we can handle (now): if we held back
                    // macOS's own NTFS driver, let it have the volume.
                    if d.volumeKind == "ntfs" {
                        self.releasedNative.insert(d.bsdName)
                        self.work.async { _ = self.watcher.mountNatively(d.bsdName) }
                    }
                    if case .denied = result { self.accessDenied(d) }
                }
            }
        }
    }

    /// Reads the start of the device to recognise NTFS and ext2/3/4.
    private func probe(_ d: DiskDescription, trustVolumeName: Bool) -> ProbeOutcome {
        // Disk Arbitration can report a disk before devfs has its nodes.
        for _ in 0..<50 where access("/dev/r" + d.bsdName, F_OK) != 0 {
            Thread.sleep(forTimeInterval: 0.1)
        }
        let fd = open("/dev/r" + d.bsdName, O_RDONLY)
        guard fd >= 0 else {
            let e = errno
            if !ctx.userMode || e != EACCES {
                Log.warn("cannot read \(d.bsdName): \(String(cString: strerror(e)))")
            }
            // EPERM for root means privacy protection, not file permissions.
            return e == EPERM && !ctx.userMode ? .denied : .notOurs
        }
        var buf = [UInt8](repeating: 0, count: 4096)
        let n = pread(fd, &buf, 4096, 0)
        close(fd)
        guard n == 4096 else { return .notOurs }
        let isNTFS = Array(buf[3..<11]) == Array("NTFS    ".utf8)
        let isExt = buf[1080] == 0x53 && buf[1081] == 0xEF
        guard isNTFS || isExt else { return .notOurs }

        // NTFS labels are read with a full (read-only) mount unless Disk
        // Arbitration already knows the name.
        let knownName = trustVolumeName ? d.volumeName : nil
        let args = isNTFS && knownName != nil ? ["-q", "/dev/r" + d.bsdName] : ["/dev/r" + d.bsdName]
        let r = runProgram(ctx.helper("fsmac-probe"), args, timeout: 60)
        guard r.status == 0,
              let line = r.output.split(separator: "\n").last(where: { $0.hasPrefix("{") }),
              let obj = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any],
              let type = obj["type"] as? String else {
            Log.warn("probe of \(d.bsdName) failed: \(r.output)")
            return .notOurs
        }
        var res = ProbeResult(fsType: type, label: obj["label"] as? String ?? "",
                              uuid: obj["uuid"] as? String ?? "")
        if isNTFS, let name = knownName, res.label.isEmpty { res.label = name }
        if !isNTFS {
            res.clean = obj["clean"] as? Bool ?? true
            res.errors = obj["errors"] as? Bool ?? false
            res.needsRecovery = obj["needs_recovery"] as? Bool ?? false
            // fsmac-probe reports "ext4" for the whole family; refine it.
            res.fsType = extFlavor(buf) ?? "ext4"
        }
        return .found(res)
    }

    // MARK: - Full Disk Access

    private func accessDenied(_ d: DiskDescription) {
        let first = !needsDiskAccess
        unreadable[d.bsdName] = d
        changed()
        if first && needsDiskAccess {
            Log.warn("macOS denies raw disk access; FSMac needs Full Disk Access")
            onMessage("FSMac Needs Full Disk Access",
                      "macOS doesn't let FSMac read the disk you connected. Open FSMac in the menu bar to allow it.")
        }
        guard accessRetry == nil else { return }
        // Access granted in System Settings applies at once; keep trying so
        // the disk mounts without any further step.
        let t = DispatchSource.makeTimerSource(queue: q)
        t.schedule(deadline: .now() + 3, repeating: 3)
        t.setEventHandler { [weak self] in self?.retryUnreadable() }
        t.resume()
        accessRetry = t
    }

    private func retryUnreadable() {
        if unreadable.isEmpty || shuttingDown {
            accessRetry?.cancel()
            accessRetry = nil
            return
        }
        guard !accessRetryRunning else { return }
        accessRetryRunning = true
        let names = Array(unreadable.keys)
        work.async { [weak self] in
            let readable = names.filter { name in
                let fd = open("/dev/r" + name, O_RDONLY)
                if fd >= 0 { close(fd); return true }
                return false
            }
            self?.q.async {
                guard let self else { return }
                self.accessRetryRunning = false
                for name in readable where self.unreadable.removeValue(forKey: name) != nil {
                    Log.info("\(name) is readable now")
                    if let disk = self.watcher.disk(bsdName: name) { self.diskAppeared(disk) }
                }
                // Full Disk Access also lets us install the filesystem bundle,
                // which may have been refused at startup.
                if !readable.isEmpty && !self.ctx.userMode {
                    self.work.async { FilesystemBundle.install(self.ctx) }
                }
                if !readable.isEmpty { self.changed() }
            }
        }
    }

    private func extFlavor(_ sb: [UInt8]) -> String? {
        guard sb.count >= 2048 else { return nil }
        func u32(_ o: Int) -> UInt32 {
            UInt32(sb[o]) | UInt32(sb[o + 1]) << 8 | UInt32(sb[o + 2]) << 16 | UInt32(sb[o + 3]) << 24
        }
        let compat = u32(1024 + 0x5C), incompat = u32(1024 + 0x60), ro = u32(1024 + 0x64)
        let ext4Incompat: UInt32 = 0x40 | 0x80 | 0x200 | 0x2000 | 0x8000  // extents, 64bit, flex_bg, ...
        let ext4RO: UInt32 = 0x8 | 0x20 | 0x40 | 0x400                     // huge_file, dir_nlink, extra_isize, csum
        if incompat & ext4Incompat != 0 || ro & ext4RO != 0 { return "ext4" }
        if compat & 0x4 != 0 { return "ext3" }                            // has_journal
        return "ext2"
    }

    private func register(_ d: DiskDescription, _ p: ProbeResult) {
        releasedNative.remove(d.bsdName)
        unreadable.removeValue(forKey: d.bsdName)
        let policy = settings.policy(fsType: p.fsType, uuid: p.uuid)
        let info = VolumeInfo(
            id: d.bsdName, wholeDisk: d.wholeBSDName, fsType: p.fsType, label: p.label,
            uuid: p.uuid, size: d.size,
            deviceName: d.deviceModel.isEmpty ? d.mediaName : d.deviceModel,
            isInternal: d.isInternal, isEjectable: d.isEjectable || d.isRemovable,
            isWritableMedia: d.isWritable,
            state: d.volumePath != nil ? .mountedByMacOS : .unmounted,
            mountPoint: d.volumePath, readOnly: d.volumePath != nil, notice: nil,
            error: nil, totalBytes: d.size, freeBytes: nil, policy: policy)
        let v = Volume(info: info)
        volumes[d.bsdName] = v
        if p.errors || !p.clean || p.needsRecovery {
            v.info.notice = p.needsRecovery
                ? "The journal needs to be replayed (the disk was not unmounted cleanly on Linux)."
                : "The filesystem was not unmounted cleanly."
        }
        Log.info("found \(p.fsType) volume \(d.bsdName) '\(p.label)' (\(d.deviceModel), \(d.size >> 20) MB)")
        changed()
        autoAction(v)
    }

    private func refreshNative(_ v: Volume, _ d: DiskDescription) {
        if let path = d.volumePath {
            if v.info.state == .unmounted || v.info.state == .error {
                v.info.state = .mountedByMacOS
                v.info.mountPoint = path
                v.info.readOnly = true
                changed()
            }
        } else if v.info.state == .mountedByMacOS {
            v.info.state = .unmounted
            v.info.mountPoint = nil
            changed()
        }
    }

    private func diskChanged(_ disk: DADisk) {
        guard let d = DiskDescription(disk) else { return }
        if unreadable[d.bsdName] != nil {
            unreadable[d.bsdName] = d
            changed()
            return
        }
        guard let v = volumes[d.bsdName], !v.busy else { return }
        refreshNative(v, d)
    }

    private func diskDisappeared(_ disk: DADisk) {
        guard let bsd = DADiskGetBSDName(disk).map({ String(cString: $0) }) else { return }
        generation[bsd, default: 0] += 1
        releasedNative.remove(bsd)
        if unreadable.removeValue(forKey: bsd) != nil { changed() }
        guard let v = volumes.removeValue(forKey: bsd) else { return }
        v.gone = true
        let wasMounted = v.info.state == .mounted
        let wasWritable = wasMounted && !v.info.readOnly
        let worker = v.worker
        // Our NFS mount, also while it is being set up or taken down.
        let mp = [.mounted, .mounting, .unmounting].contains(v.info.state)
            ? (v.info.mountPoint ?? v.pendingMountPoint) : nil
        v.worker = nil
        if worker != nil || mp != nil {
            work.async { [self] in
                if let mp {
                    _ = unmountPath(mp, force: true)
                    removeMountPoint(mp)
                }
                if let worker {
                    worker.stop()
                    if !worker.waitForExit(timeout: 2) { worker.kill9() }
                }
                if let mp { q.async { self.reserved.remove(mp) } }
            }
            Log.warn("\(bsd) '\(v.info.displayName)' disappeared while in use")
        } else {
            Log.info("\(bsd) '\(v.info.displayName)' removed")
        }
        if wasWritable {
            onMessage("Disk Not Ejected Properly",
                      "Eject “\(v.info.displayName)” before disconnecting or turning it off.")
        }
        changed()
    }

    // MARK: - Policy

    /// Disk Arbitration asks before macOS mounts a volume. Hold back NTFS
    /// volumes we are going to mount read-write ourselves.
    private func shouldBlockMount(_ disk: DADisk) -> String? {
        guard !shuttingDown, let d = DiskDescription(disk) else { return nil }
        // Recognised through our filesystem bundle; macOS cannot mount these.
        if d.volumeKind == "fsmac_ext" { return "FSMac mounts this volume" }
        guard d.volumeKind == "ntfs", !releasedNative.contains(d.bsdName) else { return nil }
        if let v = volumes[d.bsdName] {
            if v.nativeAllowed { return nil }
            if v.busy { return "FSMac is using this volume" }
            return v.info.policy.mode == .readWrite && v.info.policy.autoMount && !v.userUnmounted
                ? "FSMac mounts this volume" : nil
        }
        // Not probed yet: decide from the global settings.
        return settings.ntfsMode == .readWrite && settings.autoMount ? "FSMac mounts this volume" : nil
    }

    private func allowNative(_ v: Volume) {
        v.nativeAllowed = true
        guard v.info.state == .unmounted else { return }
        let id = v.info.id
        work.async { [weak self] in
            if let err = self?.watcher.mountNatively(id) {
                Log.warn("macOS could not mount \(id): \(err)")
            }
        }
    }

    private func autoAction(_ v: Volume) {
        guard !shuttingDown, !v.busy, !v.userUnmounted else { return }
        let p = v.info.policy
        switch p.mode {
        case .ignore:
            return
        case .readOnly:
            guard p.autoMount, v.info.state == .unmounted else { return }
            if v.info.fsType == "ntfs" {
                // Apple's own NTFS driver handles read-only access well.
                allowNative(v)
            } else {
                mount(v.info.id, readOnly: true) { _ in }
            }
        case .readWrite:
            if p.autoMount, v.info.state == .unmounted || v.info.state == .mountedByMacOS {
                mount(v.info.id, readOnly: false) { _ in }
            }
        }
    }

    // MARK: - Mounting

    /// Mounts a volume through a worker. completion(nil) on success.
    func mount(_ id: String, readOnly: Bool, removeHibernation: Bool = false,
               completion: @escaping (String?) -> Void) {
        guard !shuttingDown else { return completion("FSMac is shutting down") }
        guard let v = volumes[id] else { return completion("no such volume: \(id)") }
        guard !v.busy else { return completion("an operation is already in progress") }
        if v.info.state == .mounted {
            let wantRW = !readOnly && v.info.isWritableMedia
            if v.info.readOnly != !wantRW || removeHibernation {
                // Switch modes: unmount, then mount again.
                return unmount(id, force: false, eject: false) { [weak self] err in
                    if let err { return completion(err) }
                    self?.mount(id, readOnly: readOnly, removeHibernation: removeHibernation,
                                completion: completion)
                }
            }
            return completion(nil)
        }
        v.busy = true
        v.userUnmounted = false
        v.nativeAllowed = false
        v.cancelMount = false
        v.requestedReadOnly = readOnly
        v.info.error = nil
        v.info.state = .mounting
        changed()

        let info = v.info
        let s = settings
        work.async { [weak self] in
            guard let self else { return }
            let result = self.doMount(v, info: info, settings: s, readOnly: readOnly,
                                      removeHibernation: removeHibernation)
            let native = result != nil && info.fsType == "ntfs" ? self.watcher.describe(id) : nil
            self.q.async {
                v.busy = false
                if let err = result {
                    Log.error("mounting \(id) failed: \(err)")
                    if self.volumes[id] === v {
                        v.info.state = .error
                        v.info.error = err
                        v.info.mountPoint = nil
                        if info.fsType == "ntfs" {
                            // Fall back to read-only access through macOS.
                            if let path = native?.volumePath {
                                v.info.state = .mountedByMacOS
                                v.info.mountPoint = path
                                v.info.readOnly = true
                            } else {
                                v.info.state = .unmounted
                                self.allowNative(v)
                            }
                        }
                    }
                }
                self.changed()
                completion(result)
            }
        }
    }

    /// Runs on `work`. Returns nil on success or an error message.
    private func doMount(_ v: Volume, info: VolumeInfo, settings s: Settings,
                         readOnly requestedRO: Bool, removeHibernation: Bool) -> String? {
        var readOnly = requestedRO || !info.isWritableMedia
        var notice: String? = nil

        // 1. Take the volume away from macOS's own driver.
        if let d = watcher.describe(info.id), let path = d.volumePath {
            Log.info("unmounting the macOS mount of \(info.id) at \(path)")
            if let err = watcher.unmount(info.id) {
                return "macOS could not release the volume (\(err)). Close any files on it and try again."
            }
        }

        // 2. ext: replay the journal / repair an unclean filesystem first.
        if info.fsType != "ntfs" && !readOnly && s.autoRepairExt {
            let pr = runProgram(ctx.helper("fsmac-probe"), ["/dev/r" + info.id], timeout: 60)
            let needsCheck = pr.output.contains("\"clean\":false") ||
                pr.output.contains("\"errors\":true") || pr.output.contains("\"needs_recovery\":true")
            if needsCheck {
                q.async { v.info.state = .checking; self.changed() }
                Log.info("checking \(info.id) with e2fsck -p")
                let r = runProgram(ctx.helper("e2fsck"), ["-p", "/dev/" + info.id], timeout: 3600) {
                    Log.info("e2fsck: \($0)")
                }
                switch r.status {
                case 0, 1, 2:
                    if r.status != 0 { notice = "The filesystem was checked and repaired." }
                    q.async { v.info.notice = nil }
                default:
                    readOnly = true
                    notice = "The filesystem has errors that need manual repair; it was mounted read-only. Use Check Disk to repair it."
                    Log.warn("e2fsck -p on \(info.id) exited with \(r.status)")
                }
                q.async { v.info.state = .mounting; self.changed() }
            }
        }

        // 3. Open the raw device.
        var fd = open("/dev/r" + info.id, readOnly ? O_RDONLY : O_RDWR)
        if fd < 0 && !readOnly && (errno == EROFS || errno == EACCES || errno == EPERM) {
            readOnly = true
            notice = "The disk is write-protected; it was mounted read-only."
            fd = open("/dev/r" + info.id, O_RDONLY)
        }
        guard fd >= 0 else {
            return "cannot open the disk: \(String(cString: strerror(errno)))"
        }
        defer { close(fd) }

        // 4. Socket and mount point.
        let listener: (fd: Int32, port: UInt16)
        do {
            listener = try makeLoopbackListener()
        } catch {
            return "cannot create a socket: \(error)"
        }
        defer { close(listener.fd) }
        let mountPoint: URL = q.sync {
            let u = chooseMountPoint(root: ctx.mountRoot, label: info.label, reserved: reserved)
            reserved.insert(u.path)
            return u
        }
        var keepReservation = false
        defer { if !keepReservation { q.sync { _ = self.reserved.remove(mountPoint.path) } } }

        // 5. Start the worker.
        let user = ctx.consoleUser()
        var o = WorkerProcess.Options()
        o.readOnly = readOnly
        o.uid = user.uid
        o.gid = user.gid
        o.mountPoint = mountPoint.path
        o.insecure = ctx.userMode
        o.windowsNames = s.windowsCompatibleNames
        o.hideDotFiles = s.hideDotFilesOnNTFS
        o.fileMode = s.ntfsFileMode
        o.keepOwnership = s.keepExtOwnership
        o.removeHibernation = removeHibernation
        // The worker parses untrusted on-disk structures: once it has the
        // device and socket open it gives up root.
        o.dropToUser = ctx.userMode ? nil : "nobody"
        let logPath = ctx.logDir.appendingPathComponent("\(info.id).log").path
        let worker: WorkerProcess
        do {
            worker = try WorkerProcess(executable: ctx.helper("fsmac-worker"), deviceFD: fd,
                                       listenFD: listener.fd, logPath: logPath, options: o, queue: q)
        } catch {
            return "cannot start the filesystem server: \(error)"
        }
        let ready = DispatchSemaphore(value: 0)
        q.sync {
            v.worker = worker
            v.readyEvent = nil
            v.readySignal = ready
            v.lastWorkerError = nil
            worker.onEvent = { [weak self, weak v] ev in
                guard let self, let v else { return }
                self.workerEvent(v, ev)
            }
            worker.onExit = { [weak self, weak v, weak worker] status in
                guard let self, let v, let worker else { return }
                self.workerExited(v, worker, status)
            }
            worker.start()
        }
        let discard = {
            worker.stop()
            if !worker.waitForExit(timeout: 60) {
                worker.kill9()
                _ = worker.waitForExit(timeout: 5)
            }
            self.q.sync { if v.worker === worker { v.worker = nil } }
        }

        // 6. Wait for it to mount the filesystem (bitmaps of large volumes
        // and journal replay can take a while).
        var readyEvent: [String: Any]?
        let giveUp = Date().addingTimeInterval(30 * 60)
        while true {
            if ready.wait(timeout: .now() + 0.5) == .success {
                readyEvent = q.sync { v.readyEvent }
                break
            }
            if worker.hasExited { break }
            if q.sync(execute: { v.gone || v.cancelMount || self.shuttingDown }) { break }
            if Date() > giveUp {
                q.sync { v.lastWorkerError = "the disk did not respond" }
                break
            }
        }
        guard let ev = readyEvent else {
            discard()
            if q.sync(execute: { v.gone }) { return "the disk was disconnected" }
            if q.sync(execute: { v.cancelMount }) { return "the mount was cancelled" }
            return q.sync { v.lastWorkerError } ?? "the filesystem server stopped (status \(worker.exitStatus))"
        }
        let actualRO = (ev["readonly"] as? Bool) ?? readOnly
        if let n = ev["notice"] as? String, !n.isEmpty { notice = n }
        let label = (ev["label"] as? String) ?? info.label

        // 7. Mount it. Recorded first, so that a worker that dies right now
        // gets its mount force-removed instead of leaving a dead hard mount.
        q.sync { v.pendingMountPoint = mountPoint.path }
        if let err = mountNFS(bsd: info.id, label: label, port: listener.port, at: mountPoint,
                              userMode: ctx.userMode, readOnly: actualRO) {
            q.sync { v.pendingMountPoint = nil }
            discard()
            return "mount failed: \(err)"
        }
        let committed: Bool = q.sync {
            v.pendingMountPoint = nil
            guard v.worker === worker, !worker.hasExited, !v.gone, !v.cancelMount, !shuttingDown else {
                return false
            }
            v.info.state = .mounted
            v.info.mountPoint = mountPoint.path
            v.info.readOnly = actualRO
            v.info.label = label
            if let notice { v.info.notice = notice }
            return true
        }
        guard committed else {
            // The worker died, the disk went away or the mount was cancelled.
            _ = unmountPath(mountPoint.path, force: true)
            removeMountPoint(mountPoint.path)
            discard()
            return q.sync { v.cancelMount } ? "the mount was cancelled" : "the filesystem server stopped while mounting"
        }
        keepReservation = true
        q.async { self.refreshSpace() }
        Log.info("mounted \(info.id) '\(label)' at \(mountPoint.path) (\(actualRO ? "read-only" : "read-write"))")
        if let notice, actualRO {
            q.async { self.onMessage("“\(label.isEmpty ? "Untitled" : label)” Mounted Read-Only", notice) }
        }
        return nil
    }

    private func workerEvent(_ v: Volume, _ ev: [String: Any]) {
        switch ev["event"] as? String {
        case "ready":
            v.readyEvent = ev
            v.readySignal?.signal()
        case "error":
            let msg = ev["message"] as? String ?? "unknown error"
            v.lastWorkerError = msg
            Log.error("\(v.info.id): \(msg)")
        case "log":
            let msg = ev["message"] as? String ?? ""
            let level = ev["level"] as? String ?? "info"
            Log.write(level, "\(v.info.id): \(msg)")
            onLog(level, "\(v.info.displayName): \(msg)")
        default:
            break
        }
    }

    private func workerExited(_ v: Volume, _ worker: WorkerProcess, _ status: Int32) {
        guard v.worker === worker else { return }
        v.worker = nil
        if v.info.state == .mounting, v.pendingMountPoint != nil {
            // Died right after mount_nfs: don't leave a dead hard mount
            // behind (doMount notices the exit and reports the error).
            forceCleanup(v)
            return
        }
        guard v.info.state == .mounted else { return }
        // The worker went away while the volume was mounted.
        let mp = v.info.mountPoint
        v.info.state = .error
        v.info.error = status == 4
            ? "The disk stopped responding or was disconnected."
            : "The filesystem server stopped unexpectedly (status \(status))." +
                (v.lastWorkerError.map { " \($0)" } ?? "")
        v.info.mountPoint = nil
        Log.error("\(v.info.id): worker exited with \(status) while mounted")
        if let mp {
            work.async { [self] in
                _ = unmountPath(mp, force: true)
                removeMountPoint(mp)
                q.async { self.reserved.remove(mp) }
            }
        }
        onMessage("Problem with “\(v.info.displayName)”", v.info.error ?? "")
        changed()
    }

    /// Updates free space in the background. statfs waits for the worker,
    /// so each volume is read separately: one stuck volume cannot hold up
    /// the others.
    func refreshSpace() {
        for v in volumes.values where v.info.state == .mounted && !v.busy {
            guard let mp = v.info.mountPoint, let worker = v.worker, !worker.hasExited,
                  !spaceInFlight.contains(v.info.id) else { continue }
            let id = v.info.id
            spaceInFlight.insert(id)
            spaceQueue.async { [weak self] in
                var st = statfs()
                let ok = statfs(mp, &st) == 0
                self?.q.async {
                    guard let self else { return }
                    self.spaceInFlight.remove(id)
                    guard ok, let v = self.volumes[id], v.info.state == .mounted, v.info.mountPoint == mp else { return }
                    let total = UInt64(st.f_blocks) * UInt64(st.f_bsize)
                    let free = UInt64(st.f_bavail) * UInt64(st.f_bsize)
                    if v.info.totalBytes != total || v.info.freeBytes != free {
                        v.info.totalBytes = total
                        v.info.freeBytes = free
                        self.changed()
                    }
                }
            }
        }
    }

    // MARK: - Unmounting

    func unmount(_ id: String, force: Bool, eject: Bool, completion: @escaping (String?) -> Void) {
        guard let v = volumes[id] else { return completion("no such volume: \(id)") }
        if v.info.state == .mounting && force {
            // Abandon a mount that does not finish (e.g. a stuck disk).
            v.cancelMount = true
            return completion(nil)
        }
        guard !v.busy else { return completion("an operation is already in progress") }
        if v.info.state == .mountedByMacOS {
            v.busy = true
            v.userUnmounted = true
            work.async { [weak self] in
                guard let self else { return }
                let err = self.watcher.unmount(id, force: force)
                let d = self.watcher.describe(id)
                self.q.async {
                    v.busy = false
                    if let d, d.volumePath == nil {
                        v.info.state = .unmounted
                        v.info.mountPoint = nil
                    }
                    self.changed()
                    if err == nil, eject { self.ejectDisk(v.info.wholeDisk, completion: completion) }
                    else { completion(err) }
                }
            }
            return
        }
        guard v.info.state == .mounted || v.worker != nil else {
            if eject { return ejectDisk(v.info.wholeDisk, completion: completion) }
            return completion(nil)
        }
        v.busy = true
        v.userUnmounted = true
        v.info.state = .unmounting
        changed()
        let mp = v.info.mountPoint
        work.async { [weak self] in
            guard let self else { return }
            let err = self.stopVolume(v, mountPoint: mp, force: force)
            self.q.async {
                v.busy = false
                if let err {
                    self.unmountFailed(v)
                    self.changed()
                    return completion(err)
                }
                v.info.state = .unmounted
                v.info.mountPoint = nil
                v.info.freeBytes = nil
                v.info.notice = nil
                self.changed()
                if eject {
                    self.ejectDisk(v.info.wholeDisk, completion: completion)
                } else {
                    completion(nil)
                }
            }
        }
    }

    /// An unmount was refused (files in use). Normally the volume simply
    /// stays mounted; if its worker died in the meantime the mount is dead
    /// and is removed by force. On `q`.
    private func unmountFailed(_ v: Volume) {
        guard volumes[v.info.id] === v else { return }
        if v.worker != nil {
            v.info.state = .mounted
            return
        }
        forceCleanup(v)
        v.info.state = .error
        v.info.error = "The filesystem server stopped unexpectedly."
    }

    /// Force-unmounts whatever NFS mount the volume has (on `q`).
    private func forceCleanup(_ v: Volume) {
        guard let mp = v.info.mountPoint ?? v.pendingMountPoint else { return }
        v.info.mountPoint = nil
        v.pendingMountPoint = nil
        v.info.freeBytes = nil
        work.async { [self] in
            _ = unmountPath(mp, force: true)
            removeMountPoint(mp)
            q.async { self.reserved.remove(mp) }
        }
    }

    /// Unmounts the NFS mount and stops the worker. Runs on `work` or the
    /// Disk Arbitration queue, never on `q`.
    private func stopVolume(_ v: Volume, mountPoint mp: String?, force: Bool) -> String? {
        let (id, name) = q.sync { (v.info.id, v.info.displayName) }
        if let mp {
            var err = unmountPath(mp, force: force)
            if err == "busy" && !force {
                // Finder and Spotlight sometimes hold the volume briefly.
                Thread.sleep(forTimeInterval: 1)
                err = unmountPath(mp, force: false)
            }
            if err == "busy" {
                let users = processesUsing(mp)
                return users.isEmpty
                    ? "The volume is in use."
                    : "The volume is in use by \(users.joined(separator: ", "))."
            }
            if let err { return err }
            removeMountPoint(mp)
            q.sync { _ = reserved.remove(mp) }
        }
        if let worker = q.sync(execute: { v.worker }) {
            worker.stop()
            if !worker.waitForExit(timeout: 120) {
                Log.error("\(id): worker did not exit; killing it")
                worker.kill9()
                _ = worker.waitForExit(timeout: 5)
            }
            q.sync { if v.worker === worker { v.worker = nil } }
        }
        Log.info("unmounted \(id) '\(name)'")
        return nil
    }

    /// Unmounts every volume of a whole disk and ejects it.
    func ejectDisk(_ whole: String, completion: @escaping (String?) -> Void) {
        let all = volumes.values.filter { $0.info.wholeDisk == whole }
        if all.contains(where: { $0.busy }) {
            return completion("FSMac is busy with this disk. Try again in a moment.")
        }
        let ours = all.filter { $0.info.state == .mounted || $0.worker != nil }
        let targets = ours.map { ($0, $0.info.mountPoint, $0.info.displayName) }
        for v in all { v.busy = true }
        for v in ours {
            v.userUnmounted = true
            v.info.state = .unmounting
        }
        ejecting.insert(whole)
        changed()
        work.async { [weak self] in
            guard let self else { return }
            var failure: String?
            var stopped: Set<ObjectIdentifier> = []
            for (v, mp, name) in targets {
                if let err = self.stopVolume(v, mountPoint: mp, force: false) {
                    failure = "“\(name)”: \(err)"
                    break
                }
                stopped.insert(ObjectIdentifier(v))
            }
            if failure == nil {
                if let err = self.watcher.unmount(whole, whole: true) {
                    failure = "macOS could not unmount the other volumes on the disk: \(err)"
                } else if let err = self.watcher.eject(whole) {
                    failure = "could not eject: \(err)"
                }
            }
            self.q.async {
                self.ejecting.remove(whole)
                for v in all {
                    v.busy = false
                    if stopped.contains(ObjectIdentifier(v)) {
                        v.info.state = .unmounted
                        v.info.mountPoint = nil
                        v.info.freeBytes = nil
                    } else if v.info.state == .unmounting {
                        self.unmountFailed(v)
                    }
                }
                self.changed()
                if failure == nil { Log.info("ejected \(whole)") }
                completion(failure)
            }
        }
    }

    /// Someone (Disk Utility, `diskutil eject`, another app) wants to eject
    /// a disk holding volumes we serve: unmount them first. Runs on the Disk
    /// Arbitration queue, which may block; returns a reason to refuse.
    private func approveEject(_ disk: DADisk) -> String? {
        guard let d = DiskDescription(disk) else { return nil }
        let whole = d.wholeBSDName
        enum Decision {
            case allow
            case refuse(String)
            case stop(all: [Volume], ours: [(Volume, String?, String)])
        }
        let decision: Decision = q.sync {
            if ejecting.contains(whole) || shuttingDown { return .allow }
            let all = volumes.values.filter { $0.info.wholeDisk == whole }
            if all.contains(where: { $0.busy }) {
                return .refuse("FSMac is busy with this disk. Try again in a moment.")
            }
            let ours = all.filter { $0.worker != nil }
            if ours.isEmpty { return .allow }
            for v in all { v.busy = true }
            for v in ours {
                v.userUnmounted = true
                v.info.state = .unmounting
            }
            changed()
            return .stop(all: all, ours: ours.map { ($0, $0.info.mountPoint, $0.info.displayName) })
        }
        guard case .stop(let all, let ours) = decision else {
            if case .refuse(let why) = decision { return why }
            return nil
        }
        var failure: String?
        var stopped: Set<ObjectIdentifier> = []
        for (v, mp, name) in ours where failure == nil {
            if let err = stopVolume(v, mountPoint: mp, force: false) {
                failure = "“\(name)” could not be unmounted: \(err)"
            } else {
                stopped.insert(ObjectIdentifier(v))
            }
        }
        q.sync {
            for v in all {
                v.busy = false
                if stopped.contains(ObjectIdentifier(v)) {
                    v.info.state = .unmounted
                    v.info.mountPoint = nil
                    v.info.freeBytes = nil
                } else if v.info.state == .unmounting {
                    unmountFailed(v)
                }
            }
            changed()
        }
        if failure == nil { Log.info("unmounted the volumes of \(whole) for an eject request") }
        return failure
    }

    /// Notices our volumes being unmounted by someone else (e.g. Finder's
    /// Eject button) and finishes the job: stop the worker, eject the disk.
    private func checkMountTable() {
        guard !shuttingDown else { return }
        let mounted = Set(currentMounts().map { $0.on })
        for v in volumes.values where v.info.state == .mounted && !v.busy {
            guard let mp = v.info.mountPoint, !mounted.contains(mp) else { continue }
            Log.info("\(v.info.id) was unmounted externally")
            v.busy = true
            v.userUnmounted = true
            v.info.state = .unmounting
            changed()
            let whole = v.info.wholeDisk
            let ejectable = v.info.isEjectable
            work.async { [weak self] in
                guard let self else { return }
                _ = self.stopVolume(v, mountPoint: nil, force: false)
                removeMountPoint(mp)
                // Finder ejects whole devices; do the same, but only if
                // nothing else lives on the disk.
                let onlyOurs = ejectable && self.diskHoldsOnlyOurVolumes(whole)
                self.q.async {
                    self.reserved.remove(mp)
                    v.busy = false
                    v.info.state = .unmounted
                    v.info.mountPoint = nil
                    v.info.freeBytes = nil
                    self.changed()
                    let others = self.volumes.values.contains {
                        $0.info.wholeDisk == whole &&
                            ($0.info.state == .mounted || $0.info.state == .mountedByMacOS || $0.busy)
                    }
                    if self.settings.ejectWhenUnmounted && onlyOurs && !others && !self.shuttingDown {
                        self.ejectDisk(whole) { err in
                            if let err { Log.warn("auto-eject of \(whole): \(err)") }
                        }
                    }
                }
            }
        }
    }

    /// True when every partition of the disk is an FSMac volume or one that
    /// macOS never mounts (EFI, swap, ...). Runs on `work`.
    private func diskHoldsOnlyOurVolumes(_ whole: String) -> Bool {
        let ours = q.sync { Set(volumes.values.filter { $0.info.wholeDisk == whole }.map { $0.info.id }) }
        let ignorable: Set<String> = [
            "EFI", "C12A7328-F81F-11D2-BA4B-00A0C93EC93B",            // EFI system
            "Microsoft Reserved", "E3C9E316-0B5C-4DB8-817D-F92DF00215AE",
            "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC",                  // Windows recovery
            "Linux_Swap", "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F",     // Linux swap
            "21686148-6449-6E6F-744E-656564454649",                  // BIOS boot
        ]
        for name in partitions(of: whole) where name != whole && !ours.contains(name) {
            guard let d = watcher.describe(name), d.wholeBSDName == whole else { continue }
            if ignorable.contains(d.content) { continue }
            if d.volumePath != nil { return false }
            // APFS containers mount through a separate synthesized disk.
            if d.content == "Apple_APFS" || d.content == "7C3457EF-0000-11AA-AA11-00306543ECAC" { return false }
        }
        return true
    }

    // MARK: - Maintenance

    /// Checks (or repairs) a volume, streaming tool output. A mounted volume
    /// is unmounted for the check and mounted again afterwards.
    /// completion(error, summary).
    func check(_ id: String, repair: Bool, output: @escaping (String) -> Void,
               completion: @escaping (String?, String?) -> Void) {
        guard !shuttingDown else { return completion("FSMac is shutting down", nil) }
        guard let v = volumes[id] else { return completion("no such volume: \(id)", nil) }
        guard !v.busy else { return completion("an operation is already in progress", nil) }
        let wasMounted = v.info.state == .mounted
        let wasNative = v.info.state == .mountedByMacOS
        let remountRO = v.requestedReadOnly || v.info.policy.mode == .readOnly
        let fsType = v.info.fsType
        let finish: (String?, String?) -> Void = { [weak self] err, summary in
            guard let self else { return }
            self.q.async {
                v.busy = false
                if self.volumes[id] === v {
                    v.info.state = .unmounted
                    if err == nil { v.info.notice = nil; v.info.error = nil }
                    self.changed()
                    if wasMounted {
                        self.mount(id, readOnly: remountRO) { _ in }
                    } else if wasNative {
                        v.userUnmounted = false
                        self.autoAction(v)
                    }
                }
                completion(err, summary)
            }
        }
        let run: () -> Void = { [weak self] in
            guard let self else { return }
            let dev = "/dev/" + id
            if fsType == "ntfs" {
                let r = runProgram(self.ctx.helper("ntfsfix"), repair ? ["-d", dev] : ["-n", dev],
                                   timeout: 3600, onLine: output)
                if r.status == 0 {
                    finish(nil, repair
                        ? "The volume was checked and its journal reset. For a full repair, run chkdsk on Windows."
                        : "No problems were found.")
                } else {
                    finish("ntfsfix reported problems (status \(r.status)). Run chkdsk /f on Windows to repair this volume.", nil)
                }
            } else {
                let r = runProgram(self.ctx.helper("e2fsck"), repair ? ["-fy", dev] : ["-fn", dev],
                                   timeout: 4 * 3600, onLine: output)
                switch r.status {
                case 0: finish(nil, "No problems were found.")
                case 1 where repair, 2 where repair: finish(nil, "Problems were found and repaired.")
                case 4 where !repair: finish("The volume has errors. Use Repair to fix them.", nil)
                default: finish("e2fsck could not repair all problems (status \(r.status)).", nil)
                }
            }
        }
        // Mark the volume busy on `q` before the tool runs, so that nothing
        // can mount it in between.
        let begin: () -> Void = { [weak self] in
            guard let self else { return }
            v.busy = true
            v.info.state = .checking
            self.changed()
            self.work.async(execute: run)
        }
        if wasMounted || wasNative {
            unmount(id, force: false, eject: false) { [weak self] err in
                if let err { return completion(err, nil) }
                guard let self, self.volumes[id] === v, !v.busy else {
                    return completion("the volume is no longer available", nil)
                }
                begin()
            }
        } else {
            begin()
        }
    }

    /// Erases a partition, or a whole disk, and creates a new NTFS or ext4
    /// filesystem. A whole disk is first given a GUID partition table with a
    /// single "Microsoft Basic Data" partition, which Windows, Linux and
    /// macOS all accept.
    func format(_ name: String, fsType: String, label: String, force: Bool,
                output: @escaping (String) -> Void, completion: @escaping (String?) -> Void) {
        guard !shuttingDown else { return completion("FSMac is shutting down") }
        guard ["ntfs", "ext4"].contains(fsType) else { return completion("unsupported type \(fsType)") }
        var bsd = name.hasPrefix("/dev/") ? String(name.dropFirst(5)) : name
        if bsd.hasPrefix("rdisk") { bsd.removeFirst() }
        guard bsd.range(of: #"^disk[0-9]+(s[0-9]+)?$"#, options: .regularExpression) != nil,
              let d = watcher.describe(bsd) else {
            return completion("no such disk: \(name)")
        }
        let target = d.bsdName
        let label = String(label.prefix(fsType == "ntfs" ? 32 : 16))
        if let err = formatRefusal(d, force: force) { return completion(err) }
        let partitioned = d.isWhole && !d.isLeaf
        let affected = volumes.values.filter {
            $0.info.id == target || (partitioned && $0.info.wholeDisk == target)
        }
        if affected.contains(where: { $0.busy }) {
            return completion("an operation is already in progress")
        }
        let ours = affected.filter { $0.info.state == .mounted || $0.worker != nil }
        let targets = ours.map { ($0, $0.info.mountPoint) }
        for v in affected { v.busy = true }
        for v in ours {
            v.userUnmounted = true
            v.info.state = .unmounting
        }
        changed()
        work.async { [weak self] in
            guard let self else { return }
            var err: String?
            var stopFailed = false
            for (v, mp) in targets where err == nil {
                err = self.stopVolume(v, mountPoint: mp, force: false)
                stopFailed = err != nil
            }
            if err == nil { err = self.doFormat(d, fsType: fsType, label: label, output: output) }
            let names = self.partitions(of: d.wholeBSDName)
            self.q.async {
                for v in affected {
                    v.busy = false
                    if stopFailed {
                        if v.info.state == .unmounting { self.unmountFailed(v) }
                    } else if self.volumes[v.info.id] === v && v.worker == nil {
                        // Forget what was there and look at the disk afresh.
                        self.volumes.removeValue(forKey: v.info.id)
                    }
                }
                self.changed()
                if !stopFailed {
                    for n in names where self.volumes[n] == nil {
                        if let disk = self.watcher.disk(bsdName: n) { self.diskAppeared(disk, stale: true) }
                    }
                }
                if err == nil { Log.info("formatted \(target) as \(fsType) '\(label)'") }
                completion(err)
            }
        }
    }

    private func formatRefusal(_ d: DiskDescription, force: Bool) -> String? {
        var root = statfs()
        if statfs("/", &root) == 0 {
            let dev = withUnsafeBytes(of: &root.f_mntfromname) {
                String(cString: $0.bindMemory(to: CChar.self).baseAddress!)
            }
            if dev.hasPrefix("/dev/\(d.wholeBSDName)") { return "refusing to erase the startup disk" }
        }
        if ["Apple_APFS", "Apple_APFS_ISC", "Apple_APFS_Recovery", "Apple_Boot", "EFI"].contains(d.content) ||
            d.volumeKind == "apfs" {
            return "refusing to erase a macOS system partition"
        }
        if d.isInternal && !force {
            return "this is an internal disk; erasing it requires --force"
        }
        if !d.isWritable { return "the disk is write-protected" }
        return nil
    }

    /// Runs on `work`.
    private func doFormat(_ d: DiskDescription, fsType: String, label: String,
                          output: @escaping (String) -> Void) -> String? {
        var bsd = d.bsdName
        if d.isWhole && !d.isLeaf {
            // New partition table. diskutil creates an EFI system partition
            // (on larger disks) followed by the data partition, typed for
            // Windows.
            output("Creating a GUID partition table on \(d.bsdName)…")
            if let e = watcher.unmount(d.bsdName, whole: true) { return "cannot unmount the disk: \(e)" }
            let r = runProgram("/usr/sbin/diskutil",
                               ["eraseDisk", "ExFAT", "FSMACFORMAT", "GPT", "/dev/" + d.wholeBSDName],
                               timeout: 600, onLine: output)
            guard r.status == 0 else { return "partitioning failed (status \(r.status))" }
            bsd = dataPartition(of: d.wholeBSDName) ?? (d.wholeBSDName + "s2")
            if let e = watcher.unmount(bsd, force: true), watcher.describe(bsd)?.volumePath != nil {
                return "cannot unmount the new partition: \(e)"
            }
        } else if d.volumePath != nil {
            if let e = watcher.unmount(d.bsdName) { return "cannot unmount \(d.bsdName): \(e)" }
        }

        let dev = "/dev/" + bsd
        output("Creating \(fsType == "ntfs" ? "NTFS" : "ext4") volume “\(label)” on \(bsd)…")
        let r: ProcessResult
        if fsType == "ntfs" {
            var args = ["-Q", "-F", "-L", label]
            // Windows uses the start sector to locate the partition; an
            // unpartitioned disk starts at 0.
            let start: UInt64? = bsd == d.wholeBSDName ? 0
                : partitionOffset(bsd).flatMap { $0.1 > 0 ? $0.0 / $0.1 : nil }
            if let start { args += ["-p", String(start), "-H", "255", "-S", "63"] }
            r = runProgram(ctx.helper("mkntfs"), args + [dev], timeout: 3600, onLine: output)
        } else {
            r = runProgram(ctx.helper("mke2fs"),
                           ["-F", "-t", "ext4", "-L", label, "-E", "lazy_itable_init=1,lazy_journal_init=1", dev],
                           timeout: 3600, onLine: output)
        }
        guard r.status == 0 else { return "creating the filesystem failed (status \(r.status))" }
        output("Done.")
        return nil
    }

    private func diskutilInfo(_ bsd: String) -> [String: Any]? {
        let r = runProgram("/usr/sbin/diskutil", ["info", "-plist", bsd], timeout: 30)
        guard r.status == 0, let data = r.output.data(using: .utf8) else { return nil }
        return try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any]
    }

    private func partitionOffset(_ bsd: String) -> (UInt64, UInt64)? {
        guard let info = diskutilInfo(bsd),
              let off = (info["PartitionMapPartitionOffset"] as? NSNumber)?.uint64Value,
              let bs = (info["DeviceBlockSize"] as? NSNumber)?.uint64Value else { return nil }
        return (off, bs)
    }

    private func dataPartition(of whole: String) -> String? {
        let r = runProgram("/usr/sbin/diskutil", ["list", "-plist", whole], timeout: 30)
        guard let data = r.output.data(using: .utf8),
              let pl = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
              let disks = pl["AllDisksAndPartitions"] as? [[String: Any]],
              let parts = disks.first?["Partitions"] as? [[String: Any]] else { return nil }
        return parts.first { ($0["Content"] as? String) != "EFI" }?["DeviceIdentifier"] as? String
    }

    /// The disk and its partitions (runs on `work`).
    private func partitions(of whole: String) -> [String] {
        let r = runProgram("/usr/sbin/diskutil", ["list", "-plist", whole], timeout: 30)
        if let data = r.output.data(using: .utf8),
           let pl = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
           let all = pl["AllDisks"] as? [String] {
            return all
        }
        return [whole]
    }

    // MARK: - Settings

    func setPolicy(uuid: String, policy: VolumePolicy?) {
        config.setPolicy(uuid: uuid, policy: policy)
        applySettings()
    }

    func setSettings(_ s: Settings) {
        config.update(s)
        applySettings()
    }

    private func applySettings() {
        for v in volumes.values {
            let p = settings.policy(fsType: v.info.fsType, uuid: v.info.uuid)
            let old = v.info.policy
            v.info.policy = p
            guard p != old, !v.busy else { continue }
            let mismatched = v.info.state == .mounted &&
                (p.mode == .ignore || (p.mode == .readOnly) != v.info.readOnly)
            if mismatched {
                unmount(v.info.id, force: false, eject: false) { [weak self] err in
                    guard err == nil else { return }
                    v.userUnmounted = false
                    self?.autoAction(v)
                }
            } else {
                v.userUnmounted = false
                autoAction(v)
            }
        }
        changed()
    }

    func rescan() {
        for v in volumes.values where !v.busy { autoAction(v) }
    }

    // MARK: - Shutdown

    /// Unmounts everything cleanly (on SIGTERM and before uninstalling).
    func shutdown(completion: @escaping () -> Void) {
        q.async { [self] in
            shuttingDown = true
            let mounted = volumes.values.filter { $0.worker != nil }
            for v in mounted {
                v.busy = true
                v.info.state = .unmounting
            }
            changed()
            let group = DispatchGroup()
            for v in mounted {
                group.enter()
                let mp = v.info.mountPoint
                work.async {
                    var err = self.stopVolume(v, mountPoint: mp, force: false)
                    if err != nil { err = self.stopVolume(v, mountPoint: mp, force: true) }
                    if let err { Log.error("shutdown: \(v.info.id): \(err)") }
                    self.q.async {
                        v.busy = false
                        v.info.state = .unmounted
                        v.info.mountPoint = nil
                        group.leave()
                    }
                }
            }
            group.notify(queue: work) {
                // Give a running check or format a chance to finish.
                let deadline = Date().addingTimeInterval(100)
                while Date() < deadline, self.q.sync(execute: { self.volumes.values.contains { $0.busy } }) {
                    Thread.sleep(forTimeInterval: 0.5)
                }
                self.q.async { self.changed() }
                completion()
            }
        }
    }
}
