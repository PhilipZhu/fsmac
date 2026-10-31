// FSMac app: first-run window that walks through installing the helper.

import AppKit
import ServiceManagement
import SwiftUI

struct WelcomeView: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.dismiss) private var dismiss
    @State private var openAtLogin = LoginItem.isEnabled

    private var installed: Bool {
        model.connection == .connected || model.helper == .enabled || model.helper == .installedManually
    }
    private var running: Bool { model.connection == .connected }

    var body: some View {
        VStack(spacing: 18) {
            Image(nsImage: NSApp.applicationIconImage)
                .resizable()
                .frame(width: 88, height: 88)
            VStack(spacing: 4) {
                Text("Welcome to FSMac").font(.title.bold())
                Text("Read and write NTFS and ext4 disks.")
                    .foregroundStyle(.secondary)
            }
            VStack(alignment: .leading, spacing: 14) {
                Step(number: 1, done: installed || model.helper == .requiresApproval,
                     title: "Install the FSMac helper",
                     detail: "") {
                    if !installed && model.helper != .requiresApproval {
                        HStack {
                            Button("Install Helper") { model.installHelper() }
                                .buttonStyle(.borderedProminent)
                            Button("Install with Password…") { model.installHelperWithPassword() }
                        }
                    }
                }
                Step(number: 2, done: running,
                     title: "Allow it in System Settings",
                     detail: "Login Items & Extensions › Allow in the Background") {
                    if model.helper == .requiresApproval && !running {
                        Button("Open System Settings") { SMAppService.openSystemSettingsLoginItems() }
                            .buttonStyle(.borderedProminent)
                    } else if installed && !running {
                        HStack(spacing: 6) {
                            ProgressView().controlSize(.small)
                            Text("Waiting for the helper…").font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
                Step(number: 3, done: running && !model.volumes.isEmpty,
                     title: "Connect a disk",
                     detail: "") {
                    EmptyView()
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(16)
            .background(RoundedRectangle(cornerRadius: 10).fill(Color.primary.opacity(0.04)))

            if let err = model.lastError {
                Label(err, systemImage: "exclamationmark.triangle.fill")
                    .font(.caption)
                    .foregroundStyle(.orange)
                    .fixedSize(horizontal: false, vertical: true)
            }
            HStack {
                Toggle("Open FSMac at login", isOn: $openAtLogin)
                    .onChange(of: openAtLogin) { _, on in LoginItem.set(on) }
                Spacer()
                Button(running ? "Done" : "Later") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(24)
        .frame(width: 500)
        .onAppear { model.refreshHelperStatus() }
    }
}

private struct Step<Accessory: View>: View {
    var number: Int
    var done: Bool
    var title: String
    var detail: String
    @ViewBuilder var accessory: () -> Accessory

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            ZStack {
                Circle().fill(done ? Color.green : Color.accentColor.opacity(0.15))
                if done {
                    Image(systemName: "checkmark").font(.system(size: 12, weight: .bold)).foregroundStyle(.white)
                } else {
                    Text("\(number)").font(.system(size: 12, weight: .bold)).foregroundStyle(Color.accentColor)
                }
            }
            .frame(width: 24, height: 24)
            VStack(alignment: .leading, spacing: 4) {
                Text(title).font(.headline)
                if !detail.isEmpty {
                    Text(detail).font(.callout).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                accessory()
            }
        }
    }
}

/// Opens the welcome window once at launch while the helper is not running.
struct LaunchCoordinator: ViewModifier {
    @EnvironmentObject var model: AppModel
    @Environment(\.openWindow) private var openWindow
    @State private var checked = false

    func body(content: Content) -> some View {
        content.task {
            guard !checked else { return }
            checked = true
            if ProcessInfo.processInfo.environment["FSMAC_SNAPSHOTS"] != nil { return }
            // Give the connection a moment to come up.
            try? await Task.sleep(nanoseconds: 1_500_000_000)
            if model.connection != .connected {
                openWindow(id: "welcome")
                NSApp.activate(ignoringOtherApps: true)
            }
        }
    }
}
