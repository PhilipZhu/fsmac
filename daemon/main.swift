// fsmacd: the FSMac background service.
//
// Normally started by launchd as root (com.fsmac.daemon). With --user-mode
// it runs unprivileged for development and testing: it then only handles
// disks the current user can open (attached disk images) and mounts them
// below ~/FSMacVolumes.

import Foundation

setvbuf(stderr, nil, _IOLBF, 0)
signal(SIGPIPE, SIG_IGN)

var userMode = getuid() != 0
for arg in CommandLine.arguments.dropFirst() {
    switch arg {
    case "--user-mode": userMode = true
    case "--version":
        print("fsmacd \(FSMac.version)")
        exit(0)
    case "-h", "--help":
        print("usage: fsmacd [--user-mode]")
        exit(0)
    default:
        FileHandle.standardError.write("fsmacd: unknown option \(arg)\n".data(using: .utf8)!)
        exit(2)
    }
}

umask(0o022)
let ctx = Context(userMode: userMode)
Log.open(ctx.logDir.appendingPathComponent("fsmacd.log"))
Log.info("fsmacd \(FSMac.version) starting (\(userMode ? "user" : "system") mode, pid \(getpid()))")

for tool in ["fsmac-worker", "fsmac-probe", "e2fsck", "mke2fs", "mkntfs", "ntfsfix"]
where !FileManager.default.isExecutableFile(atPath: ctx.helper(tool)) {
    Log.error("missing helper \(ctx.helper(tool))")
}

if !userMode { FilesystemBundle.install(ctx) }

let config = Config(url: ctx.configURL)
let manager = VolumeManager(ctx: ctx, config: config)
let ipc = IPCServer(path: ctx.socketPath, manager: manager, ctx: ctx)

manager.onChange = { ipc.broadcastVolumes() }
manager.onMessage = { title, body in ipc.notify(title: title, message: body) }
manager.onLog = { level, msg in ipc.log(level: level, message: msg) }

manager.start()
do {
    try ipc.start()
} catch {
    Log.error("cannot listen on \(ctx.socketPath): \(error)")
    exit(1)
}

// Keep free-space figures current for clients.
let spaceTimer = DispatchSource.makeTimerSource(queue: manager.q)
var lastSnapshot: [VolumeInfo] = []
spaceTimer.schedule(deadline: .now() + 30, repeating: 30)
spaceTimer.setEventHandler {
    manager.refreshSpace()
    let snap = manager.snapshot()
    if snap != lastSnapshot {
        lastSnapshot = snap
        ipc.broadcastVolumes()
    }
}
spaceTimer.resume()

// Unmount everything cleanly when asked to stop (launchd sends SIGTERM at
// shutdown and when the service is unloaded).
var signalSources: [DispatchSourceSignal] = []
var stopping = false
for sig in [SIGTERM, SIGINT, SIGHUP] {
    signal(sig, SIG_IGN)
    let src = DispatchSource.makeSignalSource(signal: sig, queue: .main)
    src.setEventHandler {
        if stopping {
            if sig == SIGINT { exit(1) }
            return
        }
        stopping = true
        Log.info("signal \(sig): unmounting volumes and exiting")
        manager.shutdown {
            ipc.stop()
            Log.info("fsmacd stopped")
            exit(0)
        }
    }
    src.resume()
    signalSources.append(src)
}

dispatchMain()
