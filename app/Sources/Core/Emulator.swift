import Foundation
import SwiftUI
import UIKit

struct Game: Identifiable, Decodable, Hashable {
    let title: String
    let titleID: String
    let version: String
    let category: String
    let path: String
    let icon: String
    let kind: String

    var id: String { path }

    enum CodingKeys: String, CodingKey {
        case title
        case titleID = "title_id"
        case version, category, path, icon, kind
    }
}

struct LogLine: Identifiable {
    let id = UUID()
    let level: Int32
    let text: String
}

/// Directories the core uses. Data lives in Documents so it shows up in the Files app
/// (LiveContainer exposes each guest app's Documents); caches can be rebuilt.
enum Paths {
    static var documents: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0] }
    static var data: URL { documents.appendingPathComponent("RPCS3", isDirectory: true) }
    static var games: URL { data.appendingPathComponent("games", isDirectory: true) }
    static var imports: URL { documents.appendingPathComponent("Import", isDirectory: true) }
    static var cache: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("RPCS3", isDirectory: true)
    }
    static var resources: URL { Bundle.main.resourceURL ?? Bundle.main.bundleURL }

    static func create() {
        for url in [data, games, imports, cache] {
            try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        }
    }
}

@MainActor
final class Emulator: ObservableObject {
    enum Phase: Equatable {
        case checking
        case blocked
        case failed(String)
        case ready
    }

    @Published private(set) var phase: Phase = .checking
    @Published private(set) var preflight: Preflight?
    @Published private(set) var games: [Game] = []
    @Published private(set) var firmwareVersion = ""
    @Published private(set) var busyMessage: String?
    @Published private(set) var progress: Double?
    @Published private(set) var logs: [LogLine] = []
    @Published var running: Game?
    @Published var lastError: String?
    @Published private(set) var fps: Double = 0

    private(set) var core: CoreLibrary?
    let input = InputManager()
    private var fpsTimer: Timer?

    static let shared = Emulator()

    private init() {}

    // MARK: - Startup

    func start(force: Bool = false) {
        Paths.create()

        let checks = Preflight.run()
        preflight = checks

        guard checks.canRun || force else {
            phase = .blocked
            return
        }

        // The core reads its directories from the environment while it is being loaded.
        setenv("IPS3_DATA_DIR", Paths.data.path + "/", 1)
        setenv("IPS3_CACHE_DIR", Paths.cache.path + "/", 1)
        setenv("IPS3_RESOURCE_DIR", Paths.resources.path + "/", 1)
        // MoltenVK: Metal argument buffers cost memory and gain little on A14.
        setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0", 0)
        setenv("MVK_CONFIG_RESUME_LOST_DEVICE", "1", 0)

        do {
            let core = try CoreLibrary.load()
            self.core = core

            core.setLogCallback(Emulator.logCallback, nil)

            let ok = Paths.data.path.withCString { data in
                Paths.cache.path.withCString { cache in
                    Paths.resources.path.withCString { resources in
                        core.initialize(data, cache, resources, checks.physicalMemory, checks.memoryLimit)
                    }
                }
            }

            guard ok else {
                phase = .failed("The PS3 core failed to initialise. See the log for details.")
                return
            }

            input.attach(core: core)
            phase = .ready
            refresh()
        } catch {
            phase = .failed(error.localizedDescription)
        }
    }

    private static let logCallback: CoreLibrary.LogFn = { level, _, text, _ in
        guard let text else { return }
        let line = LogLine(level: level, text: String(cString: text))
        DispatchQueue.main.async {
            let emulator = Emulator.shared
            emulator.logs.append(line)
            if emulator.logs.count > 500 {
                emulator.logs.removeFirst(emulator.logs.count - 500)
            }
        }
    }

    func refresh() {
        guard let core else { return }
        firmwareVersion = core.takeString(core.firmwareVersion()) ?? ""

        let json = Paths.imports.path.withCString { core.takeString(core.listGames($0)) } ?? "[]"
        let decoded = (try? JSONDecoder().decode([Game].self, from: Data(json.utf8))) ?? []
        games = decoded.sorted { $0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending }
    }

    // MARK: - Installing

    private final class ProgressBox: @unchecked Sendable {
        let report: (String, UInt64, UInt64) -> Void
        init(_ report: @escaping (String, UInt64, UInt64) -> Void) { self.report = report }
    }

    nonisolated private static let progressCallback: CoreLibrary.ProgressFn = { message, value, max, user in
        guard let user else { return true }
        let box = Unmanaged<ProgressBox>.fromOpaque(user).takeUnretainedValue()
        box.report(message.map { String(cString: $0) } ?? "", value, max)
        return true
    }

    /// Imports a picked file: firmware and packages are installed, everything else (ISOs,
    /// disc folders) is copied into the games directory.
    func importItem(at url: URL) {
        guard let core else { return }
        let ext = url.pathExtension.lowercased()

        busyMessage = "Importing \(url.lastPathComponent)…"
        progress = nil

        Task.detached(priority: .userInitiated) {
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }

            let error: String?
            switch ext {
            case "pup":
                error = Emulator.install(core: core, url: url, firmware: true)
            case "pkg":
                error = Emulator.install(core: core, url: url, firmware: false)
            case "rap":
                error = Emulator.copyLicense(url)
            default:
                error = Emulator.copyIntoGames(url)
            }

            await MainActor.run {
                let emulator = Emulator.shared
                emulator.busyMessage = nil
                emulator.progress = nil
                emulator.lastError = error
                emulator.refresh()
            }
        }
    }

    /// Runs off the main thread: firmware and package installs take minutes.
    nonisolated private static func install(core: CoreLibrary, url: URL, firmware: Bool) -> String? {
        // Install from a local copy: PKG/PUP readers seek all over the file, which is slow
        // and fragile through a security-scoped provider URL.
        let local = Paths.imports.appendingPathComponent(".installing-" + url.lastPathComponent)
        try? FileManager.default.removeItem(at: local)
        do {
            try FileManager.default.copyItem(at: url, to: local)
        } catch {
            return "Could not read \(url.lastPathComponent): \(error.localizedDescription)"
        }
        defer { try? FileManager.default.removeItem(at: local) }

        let box = ProgressBox { message, value, max in
            DispatchQueue.main.async {
                let emulator = Emulator.shared
                emulator.busyMessage = message
                emulator.progress = max > 0 ? Double(value) / Double(max) : nil
            }
        }
        let user = Unmanaged.passRetained(box).toOpaque()
        defer { Unmanaged<ProgressBox>.fromOpaque(user).release() }

        let ok = local.path.withCString { path in
            firmware
                ? core.installFirmware(path, Emulator.progressCallback, user)
                : core.installPkg(path, Emulator.progressCallback, user)
        }

        return ok ? nil : (firmware ? "Firmware installation failed. Use the official PS3UPDAT.PUP." : "Package installation failed. Check the log.")
    }

    nonisolated private static func copyIntoGames(_ url: URL) -> String? {
        let destination = Paths.games.appendingPathComponent(url.lastPathComponent)
        do {
            if FileManager.default.fileExists(atPath: destination.path) {
                try FileManager.default.removeItem(at: destination)
            }
            try FileManager.default.copyItem(at: url, to: destination)
            return nil
        } catch {
            return "Could not import \(url.lastPathComponent): \(error.localizedDescription)"
        }
    }

    /// RAP licences go where RPCS3 looks for them: dev_hdd0/home/00000001/exdata.
    nonisolated private static func copyLicense(_ url: URL) -> String? {
        let exdata = Paths.data.appendingPathComponent("dev_hdd0/home/00000001/exdata", isDirectory: true)
        try? FileManager.default.createDirectory(at: exdata, withIntermediateDirectories: true)
        let destination = exdata.appendingPathComponent(url.lastPathComponent)
        do {
            try? FileManager.default.removeItem(at: destination)
            try FileManager.default.copyItem(at: url, to: destination)
            return nil
        } catch {
            return "Could not import the licence: \(error.localizedDescription)"
        }
    }

    // MARK: - Running

    func boot(_ game: Game) {
        guard let core else { return }

        if firmwareVersion.isEmpty {
            lastError = "Install the PS3 firmware (PS3UPDAT.PUP) first."
            return
        }

        running = game
        UIApplication.shared.isIdleTimerDisabled = true

        Task.detached(priority: .userInitiated) {
            let result = game.path.withCString { core.boot($0) }
            if result != 0 {
                await MainActor.run {
                    let emulator = Emulator.shared
                    emulator.lastError = "Boot failed (\(Emulator.describeBootResult(result))). See the log."
                    emulator.running = nil
                    UIApplication.shared.isIdleTimerDisabled = false
                }
            }
        }

        fpsTimer?.invalidate()
        fpsTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { _ in
            Task { @MainActor in
                let emulator = Emulator.shared
                emulator.fps = emulator.core?.getFPS() ?? 0
            }
        }
    }

    func stop() {
        fpsTimer?.invalidate()
        fpsTimer = nil
        if let core {
            Task.detached { core.stop() }
        }
        running = nil
        fps = 0
        UIApplication.shared.isIdleTimerDisabled = false
    }

    func pause() { core?.pause() }
    func resume() { core?.resume() }
    func homeMenu() { core?.openHomeMenu() }

    var isPaused: Bool { core?.state() == 4 }

    /// Rendering from the background gets the app killed by iOS, so emulation is paused
    /// whenever the app leaves the foreground.
    func scenePhaseChanged(_ phase: ScenePhase) {
        guard let core else { return }
        switch phase {
        case .background, .inactive:
            core.setBackgrounded(true)
            if running != nil, core.state() == 3 {
                core.pause()
                pausedForBackground = true
            }
        case .active:
            core.setBackgrounded(false)
            if pausedForBackground {
                pausedForBackground = false
                core.resume()
            }
        @unknown default:
            break
        }
    }

    private var pausedForBackground = false

    nonisolated static func describeBootResult(_ code: Int32) -> String {
        // game_boot_result in Emu/System.h
        let names = [
            "no errors", "generic error", "nothing to boot", "wrong disc location", "invalid file or folder",
            "invalid BD-ROM folder", "install failed", "decryption error", "file creation error",
            "firmware missing", "firmware version too old", "unsupported disc type", "savestate corrupted",
            "savestate version unsupported", "still running", "already added",
        ]
        let index = Int(code)
        return index >= 0 && index < names.count ? names[index] : "error \(code)"
    }

    // MARK: - Settings

    func config(_ path: String) -> String { core?.config(path) ?? "" }

    func setConfig(_ path: String, _ value: String) {
        guard let core else { return }
        if core.setConfig(path, value) {
            core.configSave()
        }
        objectWillChange.send()
    }

    func resetMemoryProfile() {
        guard let core, let preflight else { return }
        core.applyMemoryProfile(preflight.physicalMemory, preflight.memoryLimit)
        objectWillChange.send()
    }

    var logFileURL: URL? {
        guard let core, let path = core.takeString(core.logPath()) else { return nil }
        return URL(fileURLWithPath: path)
    }
}
