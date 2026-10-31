// FSMac: definitions shared by the daemon (fsmacd), the app and the CLI.
//
// The daemon listens on a Unix domain socket and speaks newline-delimited
// JSON: every request carries an `id` that is echoed in its response;
// clients that subscribed additionally receive unsolicited events.

import Foundation

public enum FSMac {
    public static let version = "1.0.3"
    public static let daemonLabel = "com.fsmac.daemon"
    public static let appBundleID = "com.fsmac.FSMac"
    public static let systemSocketPath = "/var/run/fsmac.sock"
    public static let supportDirectory = "/Library/Application Support/FSMac"
    public static let logDirectory = "/Library/Logs/FSMac"

    /// Socket of a daemon running in development (user) mode.
    public static var userSocketPath: String {
        NSHomeDirectory() + "/Library/Application Support/FSMac/fsmac.sock"
    }

    /// Absolute path of the running executable. argv[0] is not reliable:
    /// launchd starts bundle helpers as "Contents/Helpers/fsmacd" in "/".
    public static var executableURL: URL {
        var buf = [CChar](repeating: 0, count: 4096)
        if proc_pidpath(getpid(), &buf, UInt32(buf.count)) > 0 {
            return URL(fileURLWithPath: String(cString: buf)).resolvingSymlinksInPath()
        }
        if let p = Bundle.main.executablePath { return URL(fileURLWithPath: p).resolvingSymlinksInPath() }
        return URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath()
    }

    /// The socket clients should use: FSMAC_SOCKET overrides, then the
    /// system daemon, then a user-mode daemon.
    public static var socketPath: String {
        if let env = ProcessInfo.processInfo.environment["FSMAC_SOCKET"], !env.isEmpty {
            return env
        }
        if FileManager.default.fileExists(atPath: systemSocketPath) { return systemSocketPath }
        if FileManager.default.fileExists(atPath: userSocketPath) { return userSocketPath }
        return systemSocketPath
    }
}

/// How a volume (or a filesystem type) is handled.
public enum MountMode: String, Codable, CaseIterable, Sendable {
    case readWrite
    case readOnly
    case ignore

    public var title: String {
        switch self {
        case .readWrite: return "Read & Write"
        case .readOnly: return "Read Only"
        case .ignore: return "Don't Mount"
        }
    }
}

public struct VolumePolicy: Codable, Hashable, Sendable {
    public var mode: MountMode
    public var autoMount: Bool
    public init(mode: MountMode, autoMount: Bool) {
        self.mode = mode
        self.autoMount = autoMount
    }
}

public struct Settings: Codable, Equatable, Sendable {
    public var ntfsMode: MountMode = .readWrite
    public var extMode: MountMode = .readWrite
    /// Mount supported volumes automatically when they appear.
    public var autoMount = true
    /// Run e2fsck in preen mode before mounting an ext volume that was not
    /// cleanly unmounted (as Linux does at boot).
    public var autoRepairExt = true
    /// Map characters Windows does not allow in file names (? : * etc.).
    public var windowsCompatibleNames = true
    /// Give files whose names start with "." the Windows hidden attribute.
    public var hideDotFilesOnNTFS = true
    /// Permissions shown for files on NTFS (which has no Unix permissions).
    public var ntfsFileMode = "644"
    /// Show the owners stored on ext volumes instead of the current user.
    public var keepExtOwnership = false
    /// Eject the disk once all its volumes have been unmounted.
    public var ejectWhenUnmounted = true
    /// Per-volume overrides keyed by volume UUID.
    public var volumes: [String: VolumePolicy] = [:]

    public init() {}

    public init(from decoder: Decoder) throws {
        // Tolerate missing keys so older/newer config files keep working.
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = Settings()
        ntfsMode = try c.decodeIfPresent(MountMode.self, forKey: .ntfsMode) ?? d.ntfsMode
        extMode = try c.decodeIfPresent(MountMode.self, forKey: .extMode) ?? d.extMode
        autoMount = try c.decodeIfPresent(Bool.self, forKey: .autoMount) ?? d.autoMount
        autoRepairExt = try c.decodeIfPresent(Bool.self, forKey: .autoRepairExt) ?? d.autoRepairExt
        windowsCompatibleNames = try c.decodeIfPresent(Bool.self, forKey: .windowsCompatibleNames) ?? d.windowsCompatibleNames
        hideDotFilesOnNTFS = try c.decodeIfPresent(Bool.self, forKey: .hideDotFilesOnNTFS) ?? d.hideDotFilesOnNTFS
        ntfsFileMode = try c.decodeIfPresent(String.self, forKey: .ntfsFileMode) ?? d.ntfsFileMode
        keepExtOwnership = try c.decodeIfPresent(Bool.self, forKey: .keepExtOwnership) ?? d.keepExtOwnership
        ejectWhenUnmounted = try c.decodeIfPresent(Bool.self, forKey: .ejectWhenUnmounted) ?? d.ejectWhenUnmounted
        volumes = try c.decodeIfPresent([String: VolumePolicy].self, forKey: .volumes) ?? [:]
    }

    /// Effective policy for a volume.
    public func policy(fsType: String, uuid: String) -> VolumePolicy {
        if let p = volumes[uuid] { return p }
        return VolumePolicy(mode: fsType == "ntfs" ? ntfsMode : extMode, autoMount: autoMount)
    }
}

public enum VolumeState: String, Codable, Sendable {
    /// Found, not mounted (auto-mount off, ignored, or after unmount).
    case unmounted
    /// Checking/repairing the filesystem before mounting.
    case checking
    case mounting
    case mounted
    case unmounting
    /// Mounted by macOS itself (Apple's read-only NTFS driver).
    case mountedByMacOS
    case error
}

public struct VolumeInfo: Codable, Identifiable, Hashable, Sendable {
    public var id: String               // BSD name of the partition, e.g. disk4s1
    public var wholeDisk: String        // e.g. disk4
    public var fsType: String           // ntfs, ext4, ext3, ext2
    public var label: String
    public var uuid: String
    public var size: UInt64
    public var deviceName: String       // model / media name
    public var isInternal: Bool
    public var isEjectable: Bool
    public var isWritableMedia: Bool
    public var state: VolumeState
    public var mountPoint: String?
    public var readOnly: Bool
    public var notice: String?          // e.g. Windows is hibernated
    public var error: String?
    public var totalBytes: UInt64?
    public var freeBytes: UInt64?
    public var policy: VolumePolicy
    /// macOS refused FSMac access to the disk (Full Disk Access).
    public var needsPermission: Bool? = nil

    public var displayName: String {
        label.isEmpty ? "Untitled" : label
    }

    public var fsDisplayName: String {
        switch fsType {
        case "ntfs": return "NTFS"
        case "ext4": return "ext4"
        case "ext3": return "ext3"
        case "ext2": return "ext2"
        default: return fsType
        }
    }
}

/// A disk FSMac has ejected and that may still be plugged in. It is listed
/// until it is physically disconnected.
public struct EjectedDisk: Codable, Identifiable, Hashable, Sendable {
    public var id: String               // whole disk, e.g. disk4
    public var name: String             // device name, e.g. "Elements 25A2"
    public var volumes: [String]        // names of the volumes that were on it
    public var date: Date
    /// The disk is still attached (many USB drives stay attached after an
    /// eject until they are unplugged); it can be mounted again.
    public var mediaPresent: Bool
    /// Every FSMac volume on the disk confirmed that all its data was
    /// written and that it was marked clean, and any requested drive cache
    /// flush succeeded.
    public var verified: Bool
    public var warning: String?

    public init(id: String, name: String, volumes: [String], date: Date, mediaPresent: Bool,
                verified: Bool, warning: String?) {
        self.id = id
        self.name = name
        self.volumes = volumes
        self.date = date
        self.mediaPresent = mediaPresent
        self.verified = verified
        self.warning = warning
    }
}

/// A request to the daemon.
public struct Request: Codable, Sendable {
    public var id: Int
    public var cmd: String
    public var disk: String?
    public var readOnly: Bool?
    public var force: Bool?
    public var eject: Bool?
    public var repair: Bool?
    public var fsType: String?
    public var label: String?
    public var settings: Settings?
    public var uuid: String?
    public var policy: VolumePolicy?
    public var removeHibernation: Bool?

    public init(id: Int, cmd: String) {
        self.id = id
        self.cmd = cmd
    }
}

/// A response or event from the daemon.
public struct Response: Codable, Sendable {
    public var id: Int?
    public var ok: Bool
    public var error: String?
    public var event: String?
    public var volumes: [VolumeInfo]?
    /// Disks ejected by FSMac that are waiting to be unplugged.
    public var ejected: [EjectedDisk]?
    public var settings: Settings?
    public var version: String?
    public var output: String?
    public var title: String?
    public var message: String?
    public var disk: String?
    /// hello: the daemon runs unprivileged (development mode).
    public var userMode: Bool?
    /// hello: the caller may change settings, check and format.
    public var admin: Bool?

    public init(id: Int?, ok: Bool) {
        self.id = id
        self.ok = ok
    }
}

/// Commands understood by the daemon.
public enum Command {
    public static let hello = "hello"            // version handshake
    public static let list = "list"
    public static let subscribe = "subscribe"    // volume and log events
    public static let mount = "mount"            // disk, readOnly?, removeHibernation?
    public static let unmount = "unmount"        // disk, force?, eject?
    public static let eject = "eject"            // disk (whole or partition), force? (streams steps)
    public static let mountDisk = "mountDisk"    // disk (whole): mount an ejected disk again
    public static let check = "check"            // disk, repair? (streams output)
    public static let format = "format"          // disk, fsType, label
    public static let getSettings = "getSettings"
    public static let setSettings = "setSettings"
    public static let setPolicy = "setPolicy"    // uuid, policy
    public static let rescan = "rescan"
    /// Unmounts everything and removes system components before the helper
    /// is unregistered.
    public static let prepareUninstall = "prepareUninstall"
    /// Unmounts everything and restarts the helper (e.g. after an update).
    public static let restart = "restart"
}

/// Unsolicited events sent to subscribed clients.
public enum Event {
    public static let volumes = "volumes"        // volumes
    public static let notify = "notify"          // title, message, disk?
    public static let log = "log"                // message (worker warnings)
    public static let output = "output"          // id, output (check/format progress)
}
