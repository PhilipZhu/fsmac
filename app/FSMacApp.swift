// FSMac: menu bar app.

import AppKit
import SwiftUI

@main
struct FSMacApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var model = AppModel()
    @Environment(\.openWindow) private var openWindow
    @State private var checkedSetup = false

    var body: some Scene {
        MenuBarExtra(isInserted: .constant(
            model.connection == .connected && (!model.volumes.isEmpty || !model.ejected.isEmpty)
        )) {
            MenuView()
                .environmentObject(model)
        } label: {
            Image(systemName: menuBarSymbol)
        }
        .menuBarExtraStyle(.window)
        .onChange(of: model.connection, initial: true) {
            guard !checkedSetup else { return }
            checkedSetup = true
            if ProcessInfo.processInfo.environment["FSMAC_SNAPSHOTS"] != nil { return }
            Task { @MainActor in
                // Give the connection a moment to come up, even with no menu bar icon.
                try? await Task.sleep(nanoseconds: 1_500_000_000)
                guard model.connection != .connected else { return }
                model.refreshHelperStatus()
                switch model.helper {
                case .enabled, .installedManually:
                    break
                case .notInstalled, .requiresApproval, .unavailable:
                    openWindow(id: "welcome")
                    NSApp.activate(ignoringOtherApps: true)
                }
            }
        }

        Window("Welcome to FSMac", id: "welcome") {
            WelcomeView()
                .environmentObject(model)
        }
        .windowResizability(.contentSize)
        .defaultPosition(.center)

        Window("FSMac Settings", id: "settings") {
            SettingsView()
                .environmentObject(model)
        }
        .windowResizability(.contentSize)
        .defaultPosition(.center)

        WindowGroup("Check Disk", id: "check", for: String.self) { $disk in
            if let disk {
                CheckView(diskID: disk).environmentObject(model)
            }
        }
        .windowResizability(.contentMinSize)
        .defaultPosition(.center)

        WindowGroup("Erase", id: "format", for: String.self) { $disk in
            if let disk {
                FormatView(diskID: disk).environmentObject(model)
            }
        }
        .windowResizability(.contentSize)
        .defaultPosition(.center)
    }

    private var menuBarSymbol: String {
        if model.connection == .unavailable { return "externaldrive.badge.exclamationmark" }
        if model.volumes.contains(where: { $0.state == .error }) { return "externaldrive.badge.exclamationmark" }
        if model.volumes.contains(where: { $0.state == .mounted }) { return "externaldrive.fill" }
        if !model.ejected.isEmpty {
            return model.ejected.allSatisfy(\.verified) ? "externaldrive.badge.checkmark" : "externaldrive.badge.exclamationmark"
        }
        return "externaldrive"
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        Notifier.requestAuthorization()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false
    }
}
