// FSMac app: state shared by the menu bar extra and the windows.

import AppKit
import Combine
import ServiceManagement
import SwiftUI

@MainActor
final class AppModel: ObservableObject {
    enum Connection: Equatable {
        case connecting
        case connected
        /// The helper is not running (not installed, not approved, ...).
        case unavailable
    }

    @Published private(set) var connection: Connection = .connecting
    @Published private(set) var volumes: [VolumeInfo] = []
    /// Disks FSMac ejected that have not been unplugged yet.
    @Published private(set) var ejected: [EjectedDisk] = []
    /// Whole disk → the step an eject in progress is at.
    @Published private(set) var ejectProgress: [String: String] = [:]
    /// Whole disk → why the last eject failed (the disk is still in use).
    @Published private(set) var ejectErrors: [String: String] = [:]
    @Published private(set) var settings = Settings()
    @Published private(set) var daemonVersion: String?
    @Published private(set) var userMode = false
    @Published private(set) var isAdmin = true
    /// Volumes with a request in flight (for spinners).
    @Published private(set) var pending: Set<String> = []
    @Published var lastError: String?
    @Published private(set) var helper = HelperStatus.current()

    private var client: DaemonClient?
    private var retryTimer: Timer?
    private var announcedMounts: Set<String> = []

    init() {
        let env = ProcessInfo.processInfo.environment
        if env["FSMAC_SNAPSHOTS"] == nil || env["FSMAC_SNAPSHOTS_LIVE"] != nil { connect() }
        Snapshots.runIfRequested(model: self)
        NotificationCenter.default.addObserver(
            forName: NSApplication.didBecomeActiveNotification, object: nil, queue: .main
        ) { [weak self] _ in
            Task { @MainActor in self?.refreshHelperStatus() }
        }
    }

    // MARK: - Connection

    func connect() {
        guard client == nil else { return }
        do {
            let c = try DaemonClient(queue: .main)
            client = c
            c.onEvent = { [weak self] ev in
                MainActor.assumeIsolated { self?.handle(ev) }
            }
            c.onDisconnect = { [weak self] in
                MainActor.assumeIsolated { self?.disconnected() }
            }
            c.send(.make(Command.hello)) { [weak self] r in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    self.daemonVersion = r.version
                    self.userMode = r.userMode ?? false
                    self.isAdmin = r.admin ?? true
                }
            }
            c.send(.make(Command.subscribe)) { [weak self] r in
                MainActor.assumeIsolated {
                    guard let self, r.ok else { return }
                    self.connection = .connected
                    self.volumes = r.volumes ?? []
                    self.ejected = r.ejected ?? []
                    self.announcedMounts = Set(self.volumes.filter { $0.state == .mounted }.map(\.id))
                    if let s = r.settings { self.settings = s }
                    self.retryTimer?.invalidate()
                    self.retryTimer = nil
                }
            }
        } catch {
            connection = .unavailable
            scheduleRetry()
        }
    }

    private func disconnected() {
        client = nil
        connection = .unavailable
        volumes = []
        pending = []
        scheduleRetry()
    }

    private func scheduleRetry() {
        guard retryTimer == nil else { return }
        retryTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.refreshHelperStatus()
                if self.client == nil { self.connect() }
            }
        }
    }

    private func handle(_ ev: Response) {
        switch ev.event {
        case Event.volumes:
            let vols = ev.volumes ?? []
            for v in vols where v.state == .mounted && !announcedMounts.contains(v.id) {
                Notifier.post(title: "“\(v.displayName)” mounted",
                              body: (v.mountPoint ?? "") + (v.readOnly ? " (read-only)" : ""),
                              id: "mounted-\(v.id)")
            }
            announcedMounts = Set(vols.filter { $0.state == .mounted }.map(\.id))
            volumes = vols
            ejected = ev.ejected ?? []
            // Forget errors about disks that are gone.
            let present = Set(vols.map(\.wholeDisk))
            ejectErrors = ejectErrors.filter { present.contains($0.key) }
        case Event.notify:
            Notifier.post(title: ev.title ?? "FSMac", body: ev.message ?? "", id: nil)
        default:
            break
        }
    }

    // MARK: - Requests

    private func request(_ req: Request, disk: String? = nil, output: ((String) -> Void)? = nil,
                         then: ((Response) -> Void)? = nil) {
        guard let client else {
            lastError = ClientError.notRunning.errorDescription
            return
        }
        if let disk { pending.insert(disk) }
        client.send(req, output: output) { [weak self] r in
            MainActor.assumeIsolated {
                guard let self else { return }
                if let disk { self.pending.remove(disk) }
                if !r.ok { self.lastError = r.error }
                if let s = r.settings { self.settings = s }
                then?(r)
            }
        }
    }

    func mount(_ v: VolumeInfo, readOnly: Bool = false, removeHibernation: Bool = false) {
        request(.make(Command.mount) {
            $0.disk = v.id
            $0.readOnly = readOnly
            $0.removeHibernation = removeHibernation
        }, disk: v.id)
    }

    func unmount(_ v: VolumeInfo, force: Bool = false) {
        request(.make(Command.unmount) {
            $0.disk = v.id
            $0.force = force
        }, disk: v.id)
    }

    func eject(_ v: VolumeInfo, force: Bool = false) {
        ejectDisk(v.wholeDisk, force: force)
    }

    /// Ejects a whole disk, following its steps; the outcome stays visible
    /// in the menu (ejected, or the reason it is not safe to unplug).
    func ejectDisk(_ whole: String, force: Bool = false) {
        guard let client, ejectProgress[whole] == nil else { return }
        ejectErrors[whole] = nil
        ejectProgress[whole] = "Preparing to eject…"
        client.send(.make(Command.eject) {
            $0.disk = whole
            $0.force = force
        }, output: { [weak self] line in
            MainActor.assumeIsolated { self?.ejectProgress[whole] = line }
        }) { [weak self] r in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.ejectProgress[whole] = nil
                guard r.ok else {
                    self.ejectErrors[whole] = r.error ?? "The disk could not be ejected."
                    return
                }
                if let e = r.ejected?.first {
                    if !self.ejected.contains(where: { $0.id == e.id }) { self.ejected.append(e) }
                    Notifier.post(title: e.verified ? "“\(e.name)” can be unplugged" : "“\(e.name)” was ejected",
                                  body: e.verified ? "All data written." : (e.warning ?? "Not all data confirmed written."),
                                  id: "ejected-\(whole)")
                }
            }
        }
    }

    /// Mounts an ejected disk that is still connected.
    func mountDisk(_ whole: String) {
        ejectErrors[whole] = nil
        request(.make(Command.mountDisk) { $0.disk = whole }, disk: whole)
    }

    func dismissEjectError(_ whole: String) {
        ejectErrors[whole] = nil
    }

    func check(_ diskID: String, repair: Bool, output: @escaping (String) -> Void,
               done: @escaping (Bool, String) -> Void) {
        request(.make(Command.check) {
            $0.disk = diskID
            $0.repair = repair
        }, disk: diskID, output: output) { r in
            done(r.ok, r.ok ? (r.message ?? "Done.") : (r.error ?? "The check failed."))
        }
    }

    func format(_ diskID: String, type: String, label: String, force: Bool,
                output: @escaping (String) -> Void, done: @escaping (Bool, String) -> Void) {
        request(.make(Command.format) {
            $0.disk = diskID
            $0.fsType = type
            $0.label = label
            $0.force = force
        }, disk: diskID, output: output) { r in
            done(r.ok, r.ok ? "The disk was formatted." : (r.error ?? "Formatting failed."))
        }
    }

    func setPolicy(_ v: VolumeInfo, _ policy: VolumePolicy?) {
        request(.make(Command.setPolicy) {
            $0.uuid = v.uuid
            $0.policy = policy
        })
    }

    func update(_ change: (inout Settings) -> Void) {
        var s = settings
        change(&s)
        guard s != settings else { return }
        settings = s
        request(.make(Command.setSettings) { $0.settings = s })
    }

    func volume(_ id: String) -> VolumeInfo? {
        volumes.first { $0.id == id }
    }

    /// Shows fixed data instead of the daemon's (snapshots).
    func setPreview(volumes: [VolumeInfo], ejected: [EjectedDisk] = [],
                    progress: [String: String] = [:], errors: [String: String] = [:]) {
        connection = .connected
        self.volumes = volumes
        self.ejected = ejected
        ejectProgress = progress
        ejectErrors = errors
        announcedMounts = Set(volumes.map(\.id))
        daemonVersion = FSMac.version
    }

    // MARK: - Helper

    /// The running helper is not the one in this app (e.g. after an update).
    var helperOutdated: Bool {
        guard connection == .connected, !userMode, let v = daemonVersion else { return false }
        return v != FSMac.version
    }

    /// Restarts the helper from this app's bundle: it unmounts its volumes
    /// cleanly and launchd starts the new one. Helpers too old to restart
    /// themselves are registered again.
    func updateHelper() {
        let fallback: () -> Void = { [weak self] in
            guard let self else { return }
            if self.helper == .installedManually { self.installHelperWithPassword() } else { self.installHelper() }
        }
        guard let client else { return fallback() }
        client.send(.make(Command.restart)) { r in
            MainActor.assumeIsolated { if !r.ok { fallback() } }
        }
    }

    /// Some connected disk can't be read until FSMac has Full Disk Access.
    var needsDiskAccess: Bool {
        volumes.contains { $0.needsPermission == true }
    }

    /// Opens Privacy & Security › Full Disk Access and shows the helper in
    /// Finder, ready to be dragged into the list.
    func openDiskAccessSettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles") {
            NSWorkspace.shared.open(url)
        }
        revealHelper()
    }

    func revealHelper() {
        let helper = Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/fsmacd")
        NSWorkspace.shared.activateFileViewerSelecting([helper])
    }

    func refreshHelperStatus() {
        helper = HelperStatus.current()
    }

    func installHelper() {
        lastError = nil
        do {
            try HelperStatus.register()
        } catch {
            lastError = error.localizedDescription
        }
        refreshHelperStatus()
        if helper == .requiresApproval {
            SMAppService.openSystemSettingsLoginItems()
        }
        scheduleRetry()
    }

    func installHelperWithPassword() {
        lastError = nil
        if let err = HelperStatus.installWithAdministratorPrivileges() {
            lastError = err
        }
        scheduleRetry()
    }

    func uninstallHelper() {
        // Let the helper unmount everything and remove what it installed.
        let finish: () -> Void = { [weak self] in
            HelperStatus.unregister()
            self?.refreshHelperStatus()
        }
        guard let client else { return finish() }
        client.send(.make(Command.prepareUninstall)) { r in
            MainActor.assumeIsolated {
                if !r.ok, let err = r.error { self.lastError = err }
                finish()
            }
        }
    }
}

extension VolumeInfo {
    var stateDescription: String {
        switch state {
        case .mounted: return readOnly ? "Mounted read-only" : "Mounted"
        case .mountedByMacOS: return "Read-only (macOS driver)"
        case .unmounted: return policy.mode == .ignore ? "Ignored" : "Not mounted"
        case .checking: return "Checking…"
        case .mounting: return "Mounting…"
        case .unmounting: return "Unmounting…"
        case .error: return "Error"
        }
    }

    var isBusy: Bool {
        [.checking, .mounting, .unmounting].contains(state)
    }

    var canOpen: Bool {
        (state == .mounted || state == .mountedByMacOS) && mountPoint != nil
    }

    var usedFraction: Double? {
        guard let t = totalBytes, let f = freeBytes, t > 0, state == .mounted else { return nil }
        return Double(t - min(f, t)) / Double(t)
    }
}
