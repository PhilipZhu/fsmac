// FSMac app: the Settings window.

import AppKit
import ServiceManagement
import SwiftUI

struct SettingsView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        TabView {
            GeneralSettings()
                .tabItem { Label("General", systemImage: "gearshape") }
            NTFSSettings()
                .tabItem { Label("NTFS", systemImage: "w.square") }
            ExtSettings()
                .tabItem { Label("ext4", systemImage: "l.square") }
            VolumeSettings()
                .tabItem { Label("Volumes", systemImage: "externaldrive") }
            HelperSettings()
                .tabItem { Label("Helper", systemImage: "wrench.and.screwdriver") }
            AboutView()
                .tabItem { Label("About", systemImage: "info.circle") }
        }
        .frame(width: 560, height: 420)
    }
}

/// Binding into the daemon's settings.
@MainActor
private func setting<T>(_ model: AppModel, _ kp: WritableKeyPath<Settings, T>) -> Binding<T> {
    Binding(get: { model.settings[keyPath: kp] },
            set: { v in model.update { $0[keyPath: kp] = v } })
}

private struct NotConnectedNote: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        if model.connection != .connected {
            Label("The FSMac helper is not running; settings cannot be changed. See the Helper tab.",
                  systemImage: "exclamationmark.triangle")
                .font(.caption)
                .foregroundStyle(.orange)
        } else if !model.isAdmin {
            Label("Only administrators can change these settings.", systemImage: "lock")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }
}

struct GeneralSettings: View {
    @EnvironmentObject var model: AppModel
    @State private var openAtLogin = LoginItem.isEnabled

    var body: some View {
        Form {
            Section {
                Toggle("Open FSMac at login", isOn: $openAtLogin)
                    .onChange(of: openAtLogin) { _, on in LoginItem.set(on) }
                Text("The menu bar app is optional: disks are mounted by the helper even when it is not running.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("When a disk is connected") {
                Toggle("Mount NTFS and ext volumes automatically", isOn: setting(model, \.autoMount))
                Toggle("Eject the disk when its last volume is unmounted in Finder",
                       isOn: setting(model, \.ejectWhenUnmounted))
            }
            .disabled(!editable)
            Section {
                NotConnectedNote()
            }
        }
        .formStyle(.grouped)
    }

    private var editable: Bool { model.connection == .connected && model.isAdmin }
}

struct NTFSSettings: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        Form {
            Section {
                Picker("NTFS volumes:", selection: setting(model, \.ntfsMode)) {
                    Text("Read & Write").tag(MountMode.readWrite)
                    Text("Read Only (macOS driver)").tag(MountMode.readOnly)
                    Text("Don't Mount").tag(MountMode.ignore)
                }
            }
            Section("Compatibility") {
                Toggle("Store characters Windows doesn't allow (? : * \" < > |) as look-alikes",
                       isOn: setting(model, \.windowsCompatibleNames))
                Text("Uses the same mapping as macOS and Windows' Services for Mac, so Windows can open these files.")
                    .font(.caption).foregroundStyle(.secondary)
                Toggle("Hide files whose names start with a dot from Windows",
                       isOn: setting(model, \.hideDotFilesOnNTFS))
                Picker("File permissions shown:", selection: setting(model, \.ntfsFileMode)) {
                    Text("rw-r--r-- (644)").tag("644")
                    Text("rw-rw-r-- (664)").tag("664")
                    Text("rw-rw-rw- (666)").tag("666")
                    Text("rw------- (600)").tag("600")
                }
            }
            Section {
                Text("Changes apply the next time a volume is mounted.")
                    .font(.caption).foregroundStyle(.secondary)
                NotConnectedNote()
            }
        }
        .formStyle(.grouped)
        .disabled(model.connection != .connected || !model.isAdmin)
    }
}

struct ExtSettings: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        Form {
            Section {
                Picker("ext2/3/4 volumes:", selection: setting(model, \.extMode)) {
                    Text("Read & Write").tag(MountMode.readWrite)
                    Text("Read Only").tag(MountMode.readOnly)
                    Text("Don't Mount").tag(MountMode.ignore)
                }
            }
            Section("Integrity") {
                Toggle("Check and repair volumes that were not unmounted cleanly",
                       isOn: setting(model, \.autoRepairExt))
                Text("Runs e2fsck before mounting, as Linux does at boot. Volumes with errors that need attention are mounted read-only.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Ownership") {
                Toggle("Show the Linux owners and permissions stored on the volume",
                       isOn: setting(model, \.keepExtOwnership))
                Text("When off, all files appear to belong to you, and new files get the owner of their folder, so they stay accessible on Linux.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section {
                NotConnectedNote()
            }
        }
        .formStyle(.grouped)
        .disabled(model.connection != .connected || !model.isAdmin)
    }
}

struct VolumeSettings: View {
    @EnvironmentObject var model: AppModel

    private var rows: [(uuid: String, name: String, policy: VolumePolicy)] {
        model.settings.volumes.map { uuid, p in
            let name = model.volumes.first { $0.uuid == uuid }?.displayName ?? "Disconnected volume"
            return (uuid, name, p)
        }
        .sorted { $0.name < $1.name }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Volumes with their own setting")
                .font(.headline)
            Text("Choose “When Connected” in a volume’s menu to remember how it should be mounted.")
                .font(.callout).foregroundStyle(.secondary)
            List {
                if rows.isEmpty {
                    Text("None").foregroundStyle(.secondary)
                }
                ForEach(rows, id: \.uuid) { row in
                    HStack {
                        Image(systemName: "externaldrive")
                        VStack(alignment: .leading) {
                            Text(row.name)
                            Text(row.uuid).font(.caption2).foregroundStyle(.secondary)
                        }
                        Spacer()
                        Text(describe(row.policy)).foregroundStyle(.secondary)
                        Button("Forget") {
                            model.update { $0.volumes[row.uuid] = nil }
                        }
                    }
                }
            }
            NotConnectedNote()
        }
        .padding(20)
        .disabled(model.connection != .connected || !model.isAdmin)
    }

    private func describe(_ p: VolumePolicy) -> String {
        switch (p.mode, p.autoMount) {
        case (.ignore, _): return "Ignored"
        case (let m, true): return m.title
        case (let m, false): return "\(m.title), manual"
        }
    }
}

struct HelperSettings: View {
    @EnvironmentObject var model: AppModel
    @State private var cliInstalled = HelperStatus.commandLineToolInstalled
    @State private var message: String?

    var body: some View {
        Form {
            Section("Helper") {
                LabeledContent("Status:") {
                    HStack(spacing: 6) {
                        Circle().fill(statusColor).frame(width: 8, height: 8)
                        Text(statusText)
                    }
                }
                if let v = model.daemonVersion {
                    LabeledContent("Version:") { Text(v) }
                }
                HStack {
                    switch model.helper {
                    case .requiresApproval:
                        Button("Open Login Items Settings") { SMAppService.openSystemSettingsLoginItems() }
                    case .notInstalled, .unavailable:
                        Button("Install Helper") { model.installHelper() }
                        Button("Install with Password…") { model.installHelperWithPassword() }
                    case .enabled, .installedManually:
                        Button("Reinstall") {
                            if model.helper == .installedManually { model.installHelperWithPassword() }
                            else { model.installHelper() }
                        }
                        Button("Uninstall…") { confirmUninstall() }
                    }
                }
            }
            Section("Command-line tool") {
                LabeledContent("fsmac:") {
                    Text(cliInstalled ? "Installed in /usr/local/bin" : "Not installed")
                }
                if !cliInstalled {
                    Button("Install Command-Line Tool…") {
                        message = HelperStatus.installCommandLineTool()
                        cliInstalled = HelperStatus.commandLineToolInstalled
                    }
                }
                Text("Run `fsmac help` in Terminal for mounting, checking and formatting from scripts.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Troubleshooting") {
                Button("Show Logs in Finder") {
                    let dirs = [FSMac.logDirectory, NSHomeDirectory() + "/Library/Logs/FSMac"]
                    if let d = dirs.first(where: { FileManager.default.fileExists(atPath: $0) }) {
                        NSWorkspace.shared.open(URL(fileURLWithPath: d))
                    }
                }
                if let message {
                    Text(message).font(.caption).foregroundStyle(.red)
                }
            }
        }
        .formStyle(.grouped)
        .onAppear { model.refreshHelperStatus() }
    }

    private var statusText: String {
        if model.connection == .connected { return model.userMode ? "Running (development mode)" : "Running" }
        switch model.helper {
        case .notInstalled: return "Not installed"
        case .requiresApproval: return "Waiting for approval in System Settings"
        case .enabled, .installedManually: return "Installed, not responding"
        case .unavailable(let why): return why
        }
    }

    private var statusColor: Color {
        if model.connection == .connected { return .green }
        return model.helper == .requiresApproval ? .orange : .red
    }

    private func confirmUninstall() {
        let alert = NSAlert()
        alert.messageText = "Uninstall the FSMac helper?"
        alert.informativeText = "Mounted NTFS and ext volumes are unmounted first. NTFS disks will be read-only (macOS driver) and ext disks unavailable until you install it again."
        alert.addButton(withTitle: "Uninstall")
        alert.addButton(withTitle: "Cancel")
        if alert.runModal() == .alertFirstButtonReturn { model.uninstallHelper() }
    }
}

struct AboutView: View {
    var body: some View {
        VStack(spacing: 10) {
            Image(nsImage: NSApp.applicationIconImage)
                .resizable()
                .frame(width: 96, height: 96)
            Text("FSMac").font(.title.bold())
            Text("Version \(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? FSMac.version)")
                .foregroundStyle(.secondary)
            Text("Read and write NTFS and ext2/3/4 disks on your Mac.")
            Divider().frame(width: 300)
            Text("""
                Built on ntfs-3g (Tuxera / ntfs-3g project) and e2fsprogs (Theodore Ts'o and contributors), \
                licensed under the GNU GPL v2. FSMac is free software under the same license.
                """)
                .font(.caption)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 420)
        }
        .padding(24)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
