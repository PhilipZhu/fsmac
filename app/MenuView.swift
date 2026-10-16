// FSMac app: the menu bar window.

import AppKit
import ServiceManagement
import SwiftUI

struct MenuView: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            Divider()
            if model.needsDiskAccess {
                DiskAccessBanner()
                Divider()
            }
            if model.helperOutdated {
                HStack(spacing: 8) {
                    Image(systemName: "arrow.triangle.2.circlepath").foregroundStyle(.tint)
                    Text("The helper is from another version of FSMac.")
                        .font(.caption)
                    Spacer()
                    Button("Update") { model.updateHelper() }
                        .controlSize(.small)
                }
                .padding(.horizontal, 14)
                .padding(.vertical, 8)
                Divider()
            }
            content
            if let err = model.lastError {
                Divider()
                ErrorBanner(message: err) { model.lastError = nil }
            }
            Divider()
            footer
        }
        .frame(width: 380)
    }

    private var header: some View {
        HStack(spacing: 8) {
            Image(systemName: "externaldrive.fill.badge.checkmark")
                .font(.title2)
                .foregroundStyle(.tint)
            VStack(alignment: .leading, spacing: 1) {
                Text("FSMac").font(.headline)
                Text(statusLine).font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
    }

    private var statusLine: String {
        switch model.connection {
        case .connecting: return "Connecting…"
        case .unavailable: return "Helper not running"
        case .connected:
            let n = model.volumes.filter { $0.state == .mounted }.count
            if model.volumes.isEmpty { return "NTFS and ext4 read & write" }
            return n == 0 ? "No volumes mounted" : "\(n) volume\(n == 1 ? "" : "s") mounted"
        }
    }

    @ViewBuilder
    private var content: some View {
        switch model.connection {
        case .connecting:
            HStack {
                Spacer()
                ProgressView().controlSize(.small)
                Spacer()
            }
            .padding(24)
        case .unavailable:
            SetupView()
        case .connected:
            if model.volumes.isEmpty {
                VStack(spacing: 6) {
                    Image(systemName: "externaldrive.badge.questionmark")
                        .font(.system(size: 30))
                        .foregroundStyle(.secondary)
                    Text("No NTFS or ext disks connected")
                        .font(.callout)
                    Text("Connect a disk and it will appear in Finder, ready to use.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 22)
                .padding(.horizontal, 20)
            } else {
                ScrollView {
                    VStack(alignment: .leading, spacing: 2) {
                        ForEach(groups, id: \.disk) { group in
                            Text(group.title)
                                .font(.caption.weight(.semibold))
                                .foregroundStyle(.secondary)
                                .padding(.horizontal, 14)
                                .padding(.top, 8)
                            ForEach(group.volumes) { v in
                                VolumeRow(volume: v)
                            }
                        }
                    }
                    .padding(.bottom, 6)
                }
                .frame(maxHeight: 420)
                .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    private struct Group {
        var disk: String
        var title: String
        var volumes: [VolumeInfo]
    }

    private var groups: [Group] {
        var order: [String] = []
        var map: [String: [VolumeInfo]] = [:]
        for v in model.volumes {
            if map[v.wholeDisk] == nil { order.append(v.wholeDisk) }
            map[v.wholeDisk, default: []].append(v)
        }
        return order.map { disk in
            let vs = map[disk]!
            let name = vs.first?.deviceName.isEmpty == false ? vs.first!.deviceName : disk
            return Group(disk: disk, title: name.uppercased(), volumes: vs)
        }
    }

    private var footer: some View {
        HStack {
            Button {
                openWindow(id: "settings")
                NSApp.activate(ignoringOtherApps: true)
            } label: {
                Label("Settings…", systemImage: "gearshape")
            }
            .keyboardShortcut(",")
            Spacer()
            Button("Quit FSMac") {
                NSApp.terminate(nil)
            }
            .keyboardShortcut("q")
        }
        .buttonStyle(.borderless)
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
    }
}

struct ErrorBanner: View {
    var message: String
    var dismiss: () -> Void

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
            Text(message).font(.caption).fixedSize(horizontal: false, vertical: true)
            Spacer()
            Button(action: dismiss) {
                Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary)
            }
            .buttonStyle(.borderless)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
    }
}

/// Shown while the helper is not running: explains and installs it.
struct SetupView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            switch model.helper {
            case .requiresApproval:
                Label("Allow the FSMac helper", systemImage: "hand.raised.fill")
                    .font(.headline)
                Text("Turn on **FSMac** under *Allow in the Background* in System Settings › General › Login Items & Extensions. The helper mounts NTFS and ext disks for all users.")
                    .font(.callout)
                    .fixedSize(horizontal: false, vertical: true)
                Button("Open Login Items Settings") {
                    SMAppService.openSystemSettingsLoginItems()
                }
                .buttonStyle(.borderedProminent)
            case .enabled, .installedManually:
                Label("Starting the helper…", systemImage: "hourglass")
                    .font(.headline)
                Text("The FSMac helper is installed but not responding yet. If this persists, reinstall it in Settings.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            case .notInstalled, .unavailable:
                Label("One more step", systemImage: "wrench.and.screwdriver.fill")
                    .font(.headline)
                Text("FSMac needs a small helper to read and write NTFS and ext4 disks. macOS will ask you to allow it in System Settings.")
                    .font(.callout)
                    .fixedSize(horizontal: false, vertical: true)
                HStack {
                    Button("Install Helper") { model.installHelper() }
                        .buttonStyle(.borderedProminent)
                    Button("Install with Password…") { model.installHelperWithPassword() }
                }
                if case .unavailable(let why) = model.helper {
                    Text(why).font(.caption).foregroundStyle(.secondary)
                }
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// macOS refuses raw disk access to the helper until it has Full Disk Access.
struct DiskAccessBanner: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label("Allow FSMac to read disks", systemImage: "lock.shield.fill")
                .font(.headline)
                .foregroundStyle(.orange)
            Text("macOS blocks access to connected disks until the FSMac helper has Full Disk Access.")
                .font(.callout)
                .fixedSize(horizontal: false, vertical: true)
            VStack(alignment: .leading, spacing: 3) {
                Text("1. Click **Open Settings**. Full Disk Access and a Finder window showing **fsmacd** open.")
                Text("2. Drag **fsmacd** into the Full Disk Access list and make sure it is turned on.")
                Text("The disk is mounted a few seconds later.")
                    .foregroundStyle(.secondary)
            }
            .font(.caption)
            .fixedSize(horizontal: false, vertical: true)
            HStack {
                Button("Open Settings") { model.openDiskAccessSettings() }
                    .buttonStyle(.borderedProminent)
                Button("Show fsmacd in Finder") { model.revealHelper() }
            }
            .controlSize(.small)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}
