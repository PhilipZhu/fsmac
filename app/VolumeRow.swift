// FSMac app: one volume in the menu bar window.

import AppKit
import SwiftUI

struct VolumeRow: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.openWindow) private var openWindow
    var volume: VolumeInfo
    @State private var hovering = false

    private var busy: Bool { volume.isBusy || model.pending.contains(volume.id) }

    var body: some View {
        HStack(alignment: .center, spacing: 10) {
            icon
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(volume.displayName)
                        .font(.system(.body, weight: .medium))
                        .lineLimit(1)
                        .truncationMode(.middle)
                    Badge(text: volume.fsDisplayName)
                    if volume.state == .mounted && volume.readOnly {
                        Badge(text: "Read-only", tint: .orange)
                    }
                }
                if let frac = volume.usedFraction {
                    ProgressView(value: frac)
                        .progressViewStyle(.linear)
                        .tint(frac > 0.9 ? .red : .accentColor)
                        .controlSize(.small)
                }
                Text(detailLine)
                    .font(.caption)
                    .foregroundStyle(volume.state == .error && volume.needsPermission != true
                                     ? AnyShapeStyle(.red) : AnyShapeStyle(.secondary))
                    .lineLimit(1)
                if volume.needsPermission != true, let msg = volume.error ?? volume.notice {
                    Text(msg)
                        .font(.caption)
                        .foregroundStyle(volume.error != nil ? .red : .orange)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer(minLength: 4)
            actions
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 7)
        .background(RoundedRectangle(cornerRadius: 7).fill(hovering ? Color.primary.opacity(0.07) : .clear))
        .padding(.horizontal, 4)
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
        .onTapGesture(count: 2) { open() }
        .contextMenu { if volume.needsPermission != true { moreMenu } }
    }

    private var icon: some View {
        Image(systemName: volume.isInternal ? "internaldrive.fill" : "externaldrive.fill")
            .font(.system(size: 22))
            .foregroundStyle(volume.state == .mounted ? Color.accentColor : Color.secondary)
            .frame(width: 30)
    }

    private var detailLine: String {
        if volume.needsPermission == true {
            return volume.state == .mountedByMacOS ? "Read-only (macOS driver) · \(volume.size.formattedBytes)"
                                                    : "Waiting for access · \(volume.size.formattedBytes)"
        }
        if busy && !volume.isBusy { return "Working…" }
        if volume.state == .mounted, let t = volume.totalBytes, let f = volume.freeBytes {
            return "\(f.formattedBytes) free of \(t.formattedBytes)"
        }
        return "\(volume.stateDescription) · \(volume.size.formattedBytes)"
    }

    @ViewBuilder
    private var actions: some View {
        if volume.needsPermission == true {
            IconButton(symbol: "lock.open", help: "Allow access to this disk…") {
                model.openDiskAccessSettings()
            }
        } else if busy {
            ProgressView().controlSize(.small).frame(width: 22)
        } else {
            HStack(spacing: 2) {
                if volume.canOpen {
                    IconButton(symbol: "folder", help: "Open in Finder") { open() }
                } else if volume.policy.mode != .ignore || volume.state == .error {
                    IconButton(symbol: "play.circle", help: "Mount") { model.mount(volume) }
                }
                IconButton(symbol: "eject", help: "Eject “\(volume.displayName)”") { model.eject(volume) }
                Menu {
                    moreMenu
                } label: {
                    Image(systemName: "ellipsis.circle")
                }
                .menuStyle(.borderlessButton)
                .menuIndicator(.hidden)
                .fixedSize()
                .help("More")
            }
        }
    }

    @ViewBuilder
    private var moreMenu: some View {
        if volume.canOpen {
            Button("Open in Finder") { open() }
        }
        if volume.state == .mounted {
            Button("Unmount") { model.unmount(volume) }
            if !volume.readOnly {
                Button("Remount Read-Only") { remount(readOnly: true) }
            } else if volume.isWritableMedia {
                Button("Remount Read & Write") { remount(readOnly: false) }
            }
        } else if volume.state == .mountedByMacOS {
            Button("Mount Read & Write") { model.mount(volume) }
            Button("Unmount") { model.unmount(volume) }
        } else {
            Button("Mount") { model.mount(volume) }
            Button("Mount Read-Only") { model.mount(volume, readOnly: true) }
        }
        if volume.fsType == "ntfs", volume.notice?.localizedCaseInsensitiveContains("hibernat") == true {
            Button("Mount Read & Write (discard Windows hibernation)…") { confirmHibernation() }
        }
        Button("Eject Disk") { model.eject(volume) }
        Divider()
        Menu("When Connected") {
            policyButton(nil, "Use Default (\(defaultPolicy.mode.title))")
            Divider()
            policyButton(VolumePolicy(mode: .readWrite, autoMount: true), "Mount Read & Write")
            policyButton(VolumePolicy(mode: .readOnly, autoMount: true), "Mount Read-Only")
            policyButton(VolumePolicy(mode: .readWrite, autoMount: false), "Don't Mount Automatically")
            policyButton(VolumePolicy(mode: .ignore, autoMount: false), "Ignore This Volume")
        }
        Divider()
        Button("Check Disk…") { openTool("check") }
        Button("Erase…") { openTool("format") }
        Button("Show Info in Disk Utility") {
            NSWorkspace.shared.open(URL(fileURLWithPath: "/System/Applications/Utilities/Disk Utility.app"))
        }
        if volume.state == .mounted {
            Divider()
            Button("Force Unmount") { model.unmount(volume, force: true) }
        }
    }

    private var defaultPolicy: VolumePolicy {
        var s = model.settings
        s.volumes = [:]
        return s.policy(fsType: volume.fsType, uuid: volume.uuid)
    }

    private func policyButton(_ p: VolumePolicy?, _ title: String) -> some View {
        let current = model.settings.volumes[volume.uuid]
        return Button {
            model.setPolicy(volume, p)
        } label: {
            if current == p {
                Label(title, systemImage: "checkmark")
            } else {
                Text(title)
            }
        }
    }

    private func open() {
        guard let mp = volume.mountPoint else { return }
        NSWorkspace.shared.open(URL(fileURLWithPath: mp))
    }

    private func remount(readOnly: Bool) {
        let v = volume
        model.unmount(v)
        // Mount again once the unmount has gone through.
        Task { @MainActor in
            for _ in 0..<100 {
                try? await Task.sleep(nanoseconds: 100_000_000)
                if let cur = model.volume(v.id), cur.state == .unmounted, !model.pending.contains(v.id) {
                    model.mount(cur, readOnly: readOnly)
                    return
                }
            }
        }
    }

    private func confirmHibernation() {
        let alert = NSAlert()
        alert.messageText = "Discard Windows' hibernation data on “\(volume.displayName)”?"
        alert.informativeText = """
            Windows was hibernated or shut down with Fast Startup. Mounting the volume read & write \
            deletes the saved Windows session; anything unsaved in it is lost. To avoid this, start \
            Windows and shut it down fully (hold Shift while choosing Shut Down).
            """
        alert.alertStyle = .warning
        alert.addButton(withTitle: "Discard and Mount")
        alert.addButton(withTitle: "Cancel")
        NSApp.activate(ignoringOtherApps: true)
        if alert.runModal() == .alertFirstButtonReturn {
            let v = volume
            if v.state == .mounted {
                model.unmount(v)
                Task { @MainActor in
                    for _ in 0..<100 {
                        try? await Task.sleep(nanoseconds: 100_000_000)
                        if let cur = model.volume(v.id), cur.state == .unmounted, !model.pending.contains(v.id) {
                            model.mount(cur, removeHibernation: true)
                            return
                        }
                    }
                }
            } else {
                model.mount(v, removeHibernation: true)
            }
        }
    }

    private func openTool(_ id: String) {
        openWindow(id: id, value: volume.id)
        NSApp.activate(ignoringOtherApps: true)
    }
}

struct Badge: View {
    var text: String
    var tint: Color = .secondary

    var body: some View {
        Text(text)
            .font(.system(size: 9.5, weight: .semibold))
            .padding(.horizontal, 5)
            .padding(.vertical, 1.5)
            .foregroundStyle(tint)
            .background(Capsule().strokeBorder(tint.opacity(0.6), lineWidth: 0.8))
    }
}

struct IconButton: View {
    var symbol: String
    var help: String
    var action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 14))
                .frame(width: 24, height: 24)
                .background(Circle().fill(hovering ? Color.primary.opacity(0.1) : .clear))
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .help(help)
    }
}
