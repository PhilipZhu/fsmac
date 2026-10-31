// FSMac app: renders the main views to PNG files for visual review.
//   FSMAC_SNAPSHOTS=/tmp/shots FSMac.app/Contents/MacOS/FSMac

import AppKit
import SwiftUI

extension AppModel {
    /// Sample data covering the interesting states.
    func loadSampleData() {
        if ProcessInfo.processInfo.environment["FSMAC_SNAPSHOTS_EJECT"] == "only" {
            // A drive whose media went away after the eject: only its card.
            setPreview(volumes: [], ejected: [
                EjectedDisk(id: "disk5", name: "Samsung T7", volumes: ["Windows Backup", "linux-home"],
                            date: Date(), mediaPresent: false, verified: true, warning: nil),
            ])
            return
        }
        func vol(_ id: String, _ whole: String, _ type: String, _ label: String, _ dev: String,
                 _ state: VolumeState, ro: Bool = false, total: UInt64 = 0, free: UInt64 = 0,
                 notice: String? = nil, error: String? = nil) -> VolumeInfo {
            VolumeInfo(id: id, wholeDisk: whole, fsType: type, label: label, uuid: UUID().uuidString,
                       size: total == 0 ? 500_107_862_016 : total, deviceName: dev, isInternal: false,
                       isEjectable: true, isWritableMedia: true, state: state,
                       mountPoint: state == .mounted ? "/Volumes/\(label)" : nil, readOnly: ro,
                       notice: notice, error: error, totalBytes: total == 0 ? nil : total,
                       freeBytes: total == 0 ? nil : free,
                       policy: VolumePolicy(mode: .readWrite, autoMount: true))
        }
        setPreview(volumes: [
            vol("disk4s2", "disk4", "ntfs", "Windows Backup", "Samsung T7", .mounted,
                total: 1_000_202_043_392, free: 412_000_000_000),
            vol("disk4s3", "disk4", "ext4", "linux-home", "Samsung T7", .mounted,
                total: 512_110_190_592, free: 31_000_000_000),
            vol("disk6s1", "disk6", "ntfs", "GAMES", "WD Elements", .mounted, ro: true,
                total: 2_000_398_934_016, free: 800_000_000_000,
                notice: "Windows is hibernated (Fast Startup); mounted read-only."),
            vol("disk7s1", "disk7", "ext4", "rootfs", "SanDisk Ultra", .unmounted),
        ] + (ProcessInfo.processInfo.environment["FSMAC_SNAPSHOTS_ACCESS"] != nil ? [
            {
                var v = vol("disk12s2", "disk12", "ext4", "", "Samsung T5", .error,
                            error: "macOS doesn't allow FSMac to read this disk yet. Give FSMac Full Disk Access.")
                v.needsPermission = true
                return v
            }(),
        ] : []),
        ejected: ProcessInfo.processInfo.environment["FSMAC_SNAPSHOTS_EJECT"] != nil ? [
            EjectedDisk(id: "disk12", name: "Elements 25A2", volumes: ["NTFS(625GB)", "Ext4(275GB)"],
                        date: Date(), mediaPresent: true, verified: true, warning: nil),
        ] : [],
        progress: ProcessInfo.processInfo.environment["FSMAC_SNAPSHOTS_EJECT"] != nil
            ? ["disk4": "“Windows Backup”: written, marked clean"] : [:],
        errors: ProcessInfo.processInfo.environment["FSMAC_SNAPSHOTS_EJECT"] != nil
            ? ["disk6": "“GAMES”: The volume is in use by Preview, Terminal."] : [:])
    }
}

@MainActor
enum Snapshots {
    static func runIfRequested(model: AppModel) {
        guard let dir = ProcessInfo.processInfo.environment["FSMAC_SNAPSHOTS"] else { return }
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        let live = ProcessInfo.processInfo.environment["FSMAC_SNAPSHOTS_LIVE"] != nil
        DispatchQueue.main.asyncAfter(deadline: .now() + (live ? 1.5 : 0.2)) {
            if !live { model.loadSampleData() }
            for scheme in [NSAppearance.Name.aqua, .darkAqua] {
                let suffix = scheme == .aqua ? "light" : "dark"
                render(MenuView(), model, scheme, "\(dir)/menu-\(suffix).png")
            }
            render(SetupView().frame(width: 380), model, .aqua, "\(dir)/setup.png")
            render(WelcomeView(), model, .aqua, "\(dir)/welcome.png")
            render(SettingsView(), model, .aqua, "\(dir)/settings.png")
            render(NTFSSettings().frame(width: 560, height: 420), model, .aqua, "\(dir)/settings-ntfs.png")
            render(ExtSettings().frame(width: 560, height: 420), model, .aqua, "\(dir)/settings-ext.png")
            render(HelperSettings().frame(width: 560, height: 420), model, .aqua, "\(dir)/settings-helper.png")
            render(AboutView().frame(width: 560, height: 420), model, .aqua, "\(dir)/about.png")
            if let id = model.volumes.first?.id {
                render(CheckView(diskID: id), model, .aqua, "\(dir)/check.png")
                render(FormatView(diskID: id), model, .aqua, "\(dir)/format.png")
            }
            NSApp.terminate(nil)
        }
    }

    private static func render<V: View>(_ view: V, _ model: AppModel, _ appearance: NSAppearance.Name,
                                        _ path: String) {
        let dark = appearance == .darkAqua
        let root = view
            .environmentObject(model)
            .background(Color(nsColor: dark ? NSColor(white: 0.17, alpha: 1) : .windowBackgroundColor))
            .environment(\.colorScheme, dark ? .dark : .light)
        let host = NSHostingView(rootView: root)
        host.appearance = NSAppearance(named: appearance)
        let size = host.fittingSize
        let window = NSWindow(contentRect: NSRect(origin: CGPoint(x: -10000, y: -10000), size: size),
                              styleMask: [.borderless], backing: .buffered, defer: false)
        window.appearance = NSAppearance(named: appearance)
        window.contentView = host
        host.frame = NSRect(origin: .zero, size: size)
        host.layoutSubtreeIfNeeded()
        window.displayIfNeeded()
        guard let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) else { return }
        NSAppearance(named: appearance)!.performAsCurrentDrawingAppearance {
            host.cacheDisplay(in: host.bounds, to: rep)
        }
        if let png = rep.representation(using: .png, properties: [:]) {
            try? png.write(to: URL(fileURLWithPath: path))
        }
        window.orderOut(nil)
    }
}
