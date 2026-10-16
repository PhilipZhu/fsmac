// FSMac daemon: notices when the user logged in at the console changes.
//
// Files on volumes that ignore ownership are shown as belonging to the
// console user. A disk mounted at boot, before anyone logged in, or across
// fast user switching would otherwise keep showing the previous owner.

import Foundation
import SystemConfiguration

final class ConsoleUserWatcher {
    private var store: SCDynamicStore?
    private let onChange: () -> Void

    init(queue: DispatchQueue, onChange: @escaping () -> Void) {
        self.onChange = onChange
        var ctx = SCDynamicStoreContext(version: 0, info: Unmanaged.passUnretained(self).toOpaque(),
                                        retain: nil, release: nil, copyDescription: nil)
        guard let store = SCDynamicStoreCreate(nil, "com.fsmac.daemon" as CFString, { _, _, info in
            guard let info else { return }
            Unmanaged<ConsoleUserWatcher>.fromOpaque(info).takeUnretainedValue().onChange()
        }, &ctx) else {
            Log.warn("cannot watch the console user")
            return
        }
        let key = SCDynamicStoreKeyCreateConsoleUser(nil)
        SCDynamicStoreSetNotificationKeys(store, [key] as CFArray, nil)
        SCDynamicStoreSetDispatchQueue(store, queue)
        self.store = store
    }

    deinit {
        if let store { SCDynamicStoreSetDispatchQueue(store, nil) }
    }
}
