// FSMac daemon: Disk Arbitration integration.

import DiskArbitration
import Foundation

/// A snapshot of the Disk Arbitration description of a disk.
struct DiskDescription {
    var bsdName: String
    var wholeBSDName: String
    var size: UInt64
    var isWhole: Bool
    var isLeaf: Bool
    var isWritable: Bool
    var isEjectable: Bool
    var isRemovable: Bool
    var isInternal: Bool
    var isNetwork: Bool
    var content: String
    var volumeKind: String?
    var volumeName: String?
    var volumeUUID: String?
    var volumePath: String?
    var deviceModel: String
    var mediaName: String
    var deviceProtocol: String

    /// A disk image or other virtual device (nothing to unplug).
    var isVirtual: Bool { deviceProtocol == "Disk Image" || deviceProtocol == "Virtual Interface" }

    init?(_ disk: DADisk) {
        guard let cf = DADiskCopyDescription(disk) as? [CFString: Any],
              let bsd = DADiskGetBSDName(disk).map({ String(cString: $0) }) else {
            return nil
        }
        bsdName = bsd
        size = (cf[kDADiskDescriptionMediaSizeKey] as? NSNumber)?.uint64Value ?? 0
        isWhole = (cf[kDADiskDescriptionMediaWholeKey] as? Bool) ?? false
        isLeaf = (cf[kDADiskDescriptionMediaLeafKey] as? Bool) ?? false
        isWritable = (cf[kDADiskDescriptionMediaWritableKey] as? Bool) ?? false
        isEjectable = (cf[kDADiskDescriptionMediaEjectableKey] as? Bool) ?? false
        isRemovable = (cf[kDADiskDescriptionMediaRemovableKey] as? Bool) ?? false
        isInternal = (cf[kDADiskDescriptionDeviceInternalKey] as? Bool) ?? false
        isNetwork = (cf[kDADiskDescriptionVolumeNetworkKey] as? Bool) ?? false
        content = (cf[kDADiskDescriptionMediaContentKey] as? String) ?? ""
        volumeKind = cf[kDADiskDescriptionVolumeKindKey] as? String
        volumeName = cf[kDADiskDescriptionVolumeNameKey] as? String
        if let u = cf[kDADiskDescriptionVolumeUUIDKey] {
            volumeUUID = CFUUIDCreateString(nil, (u as! CFUUID)) as String?
        }
        volumePath = (cf[kDADiskDescriptionVolumePathKey] as? URL)?.path
        deviceModel = ((cf[kDADiskDescriptionDeviceModelKey] as? String) ?? "")
            .trimmingCharacters(in: .whitespaces)
        mediaName = (cf[kDADiskDescriptionMediaNameKey] as? String) ?? ""
        deviceProtocol = (cf[kDADiskDescriptionDeviceProtocolKey] as? String) ?? ""
        if let whole = DADiskCopyWholeDisk(disk), let wb = DADiskGetBSDName(whole) {
            wholeBSDName = String(cString: wb)
        } else {
            wholeBSDName = bsd
        }
    }

    /// Partition types (or whole-disk contents) that may hold NTFS or ext.
    var mayHoldSupportedFS: Bool {
        if volumeKind == "ntfs" || volumeKind == "fsmac_ext" { return true }
        let candidates: Set<String> = [
            "", "Windows_NTFS", "Linux", "Linux_Ext2FS", "Microsoft Basic Data",
            "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7",   // Microsoft basic data
            "0FC63DAF-8483-4772-8E79-3D69D8477DE4",   // Linux filesystem data
            "933AC7E1-2EB4-4F13-B844-0E14E2AEF915",   // Linux /home
            "3B8F8425-20E0-4F3B-907F-1A25A76F98E8",   // Linux /srv
            "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709",   // Linux root (x86-64)
            "B921B045-1DF0-41C3-AF44-4C6F280D3FAE",   // Linux root (arm64)
            "Apple_UNIX_SVR2", "Windows_FAT_32", "DOS_FAT_32",
        ]
        return candidates.contains(content)
    }
}

/// Disk Arbitration events. Appear/disappear/change notifications are
/// delivered on the queue given to init; approval callbacks run on the
/// watcher's private queue and may block (e.g. to unmount before an eject),
/// so they must not wait for anything that waits for that private queue.
final class DiskWatcher {
    private let session: DASession
    private let daQueue = DispatchQueue(label: "com.fsmac.diskarbitration")
    private let target: DispatchQueue
    var onAppeared: (DADisk) -> Void = { _ in }
    var onDisappeared: (DADisk) -> Void = { _ in }
    var onChanged: (DADisk) -> Void = { _ in }
    /// Return a non-nil message to stop macOS from mounting the disk.
    var shouldBlockMount: (DADisk) -> String? = { _ in nil }
    /// Return a non-nil message to refuse ejecting the disk.
    var shouldBlockEject: (DADisk) -> String? = { _ in nil }

    init(queue: DispatchQueue) {
        target = queue
        session = DASessionCreate(kCFAllocatorDefault)!
        DASessionSetDispatchQueue(session, daQueue)
        DASessionSetDispatchQueue(opSession, opQueue)
    }

    func start() {
        let ctx = Unmanaged.passUnretained(self).toOpaque()
        DARegisterDiskAppearedCallback(session, nil, { disk, ctx in
            let me = Unmanaged<DiskWatcher>.fromOpaque(ctx!).takeUnretainedValue()
            me.target.async { me.onAppeared(disk) }
        }, ctx)
        DARegisterDiskDisappearedCallback(session, nil, { disk, ctx in
            let me = Unmanaged<DiskWatcher>.fromOpaque(ctx!).takeUnretainedValue()
            me.target.async { me.onDisappeared(disk) }
        }, ctx)
        let keys = [kDADiskDescriptionVolumePathKey, kDADiskDescriptionVolumeNameKey] as CFArray
        DARegisterDiskDescriptionChangedCallback(session, nil, keys, { disk, _, ctx in
            let me = Unmanaged<DiskWatcher>.fromOpaque(ctx!).takeUnretainedValue()
            me.target.async { me.onChanged(disk) }
        }, ctx)
        DARegisterDiskMountApprovalCallback(session, nil, { disk, ctx in
            let me = Unmanaged<DiskWatcher>.fromOpaque(ctx!).takeUnretainedValue()
            guard let msg = me.shouldBlockMount(disk) else { return nil }
            return Unmanaged.passRetained(DADissenterCreate(kCFAllocatorDefault,
                                                            DAReturn(kDAReturnBusy), msg as CFString))
        }, ctx)
        DARegisterDiskEjectApprovalCallback(session, nil, { disk, ctx in
            let me = Unmanaged<DiskWatcher>.fromOpaque(ctx!).takeUnretainedValue()
            guard let msg = me.shouldBlockEject(disk) else { return nil }
            return Unmanaged.passRetained(DADissenterCreate(kCFAllocatorDefault,
                                                            DAReturn(kDAReturnBusy), msg as CFString))
        }, ctx)
    }

    func disk(bsdName: String) -> DADisk? {
        DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsdName)
    }

    // MARK: Synchronous operations
    //
    // These use a second session with its own queue, so that their
    // completions are delivered even while an approval callback blocks the
    // main session's queue. Call them from a background queue, never from an
    // approval callback.

    private let opQueue = DispatchQueue(label: "com.fsmac.diskarbitration.ops")
    private let opSession: DASession = DASessionCreate(kCFAllocatorDefault)!

    private final class Waiter {
        let sem = DispatchSemaphore(value: 0)
        var error: String?
    }

    private static func message(_ d: DADissenter?) -> String? {
        guard let d else { return nil }
        let status = DADissenterGetStatus(d)
        if let s = DADissenterGetStatusString(d) { return s as String }
        return String(format: "error 0x%08x", status)
    }

    private func opDisk(_ bsd: String) -> DADisk? {
        DADiskCreateFromBSDName(kCFAllocatorDefault, opSession, bsd)
    }

    /// The current description of a disk.
    func describe(_ bsd: String) -> DiskDescription? {
        opDisk(bsd).flatMap { DiskDescription($0) }
    }

    private func wait(_ w: Waiter, _ timeout: TimeInterval) -> String? {
        if w.sem.wait(timeout: .now() + timeout) == .timedOut { return "timed out" }
        return w.error
    }

    /// Unmounts a volume mounted by macOS (whole = all volumes of the disk).
    func unmount(_ bsd: String, whole: Bool = false, force: Bool = false) -> String? {
        guard let disk = opDisk(bsd) else { return "no such disk" }
        let w = Waiter()
        var opts = DADiskUnmountOptions(kDADiskUnmountOptionDefault)
        if whole { opts |= DADiskUnmountOptions(kDADiskUnmountOptionWhole) }
        if force { opts |= DADiskUnmountOptions(kDADiskUnmountOptionForce) }
        DADiskUnmount(disk, opts, { _, dissenter, ctx in
            let w = Unmanaged<Waiter>.fromOpaque(ctx!).takeRetainedValue()
            w.error = DiskWatcher.message(dissenter)
            w.sem.signal()
        }, Unmanaged.passRetained(w).toOpaque())
        return wait(w, 120)
    }

    /// Lets macOS mount a volume with its own driver.
    func mountNatively(_ bsd: String) -> String? {
        guard let disk = opDisk(bsd) else { return "no such disk" }
        let w = Waiter()
        DADiskMount(disk, nil, DADiskMountOptions(kDADiskMountOptionDefault), { _, dissenter, ctx in
            let w = Unmanaged<Waiter>.fromOpaque(ctx!).takeRetainedValue()
            w.error = DiskWatcher.message(dissenter)
            w.sem.signal()
        }, Unmanaged.passRetained(w).toOpaque())
        return wait(w, 120)
    }

    func eject(_ bsd: String) -> String? {
        guard let disk = opDisk(bsd) else { return "no such disk" }
        let w = Waiter()
        DADiskEject(disk, DADiskEjectOptions(kDADiskEjectOptionDefault), { _, dissenter, ctx in
            let w = Unmanaged<Waiter>.fromOpaque(ctx!).takeRetainedValue()
            w.error = DiskWatcher.message(dissenter)
            w.sem.signal()
        }, Unmanaged.passRetained(w).toOpaque())
        return wait(w, 300)
    }
}
