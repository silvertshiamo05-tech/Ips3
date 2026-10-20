import Darwin
import Foundation

/// The C API of Ips3Core.framework (ios/core/include/ips3_core.h), resolved with dlsym.
///
/// The framework is not linked: RPCS3 reserves ~26 GB of address space and sets up its
/// signal handlers from static initialisers, which run inside dlopen(). Loading it only after
/// `Preflight` has passed is what turns "crashes on launch" into an explanation.
final class CoreLibrary {
    typealias LogFn = @convention(c) (Int32, UnsafePointer<CChar>?, UnsafePointer<CChar>?, UnsafeMutableRawPointer?) -> Void
    typealias ProgressFn = @convention(c) (UnsafePointer<CChar>?, UInt64, UInt64, UnsafeMutableRawPointer?) -> Bool

    let apiVersion: @convention(c) () -> Int32
    let initialize: @convention(c) (UnsafePointer<CChar>, UnsafePointer<CChar>, UnsafePointer<CChar>, UInt64, UInt64) -> Bool
    let setLogCallback: @convention(c) (LogFn?, UnsafeMutableRawPointer?) -> Void
    let applyMemoryProfile: @convention(c) (UInt64, UInt64) -> Void
    let firmwareVersion: @convention(c) () -> UnsafeMutablePointer<CChar>?
    let installFirmware: @convention(c) (UnsafePointer<CChar>, ProgressFn?, UnsafeMutableRawPointer?) -> Bool
    let installPkg: @convention(c) (UnsafePointer<CChar>, ProgressFn?, UnsafeMutableRawPointer?) -> Bool
    let listGames: @convention(c) (UnsafePointer<CChar>?) -> UnsafeMutablePointer<CChar>?
    let setSurface: @convention(c) (UnsafeMutableRawPointer?, Int32, Int32) -> Void
    let surfaceResized: @convention(c) (Int32, Int32) -> Void
    let boot: @convention(c) (UnsafePointer<CChar>) -> Int32
    let state: @convention(c) () -> Int32
    let pause: @convention(c) () -> Void
    let resume: @convention(c) () -> Void
    let stop: @convention(c) () -> Void
    let openHomeMenu: @convention(c) () -> Void
    let setBackgrounded: @convention(c) (Bool) -> Void
    let padSet: @convention(c) (Int32, UInt32, UInt32, Int32, Int32, Int32, Int32) -> Void
    let padSetMotion: @convention(c) (Int32, Int32, Int32, Int32, Int32) -> Void
    let padGetRumble: @convention(c) (Int32) -> Int32
    let configGet: @convention(c) (UnsafePointer<CChar>) -> UnsafeMutablePointer<CChar>?
    let configSet: @convention(c) (UnsafePointer<CChar>, UnsafePointer<CChar>) -> Bool
    let configSave: @convention(c) () -> Void
    let getFPS: @convention(c) () -> Double
    let getTitle: @convention(c) () -> UnsafeMutablePointer<CChar>?
    let logPath: @convention(c) () -> UnsafeMutablePointer<CChar>?
    let free: @convention(c) (UnsafeMutableRawPointer?) -> Void

    static let expectedAPIVersion: Int32 = 1

    enum LoadError: LocalizedError {
        case notFound([String])
        case dlopen(String)
        case missingSymbol(String)
        case apiMismatch(Int32)

        var errorDescription: String? {
            switch self {
            case .notFound(let paths):
                return "Ips3Core.framework was not found. Looked in:\n" + paths.joined(separator: "\n")
            case .dlopen(let message):
                return "Loading the PS3 core failed: \(message)"
            case .missingSymbol(let name):
                return "The PS3 core is missing \(name). The app and core are from different builds."
            case .apiMismatch(let version):
                return "The PS3 core speaks API v\(version), the app expects v\(CoreLibrary.expectedAPIVersion)."
            }
        }
    }

    private init(handle: UnsafeMutableRawPointer) throws {
        func sym<T>(_ name: String, _: T.Type = T.self) throws -> T {
            guard let pointer = dlsym(handle, name) else { throw LoadError.missingSymbol(name) }
            return unsafeBitCast(pointer, to: T.self)
        }

        apiVersion = try sym("ips3_api_version")
        initialize = try sym("ips3_initialize")
        setLogCallback = try sym("ips3_set_log_callback")
        applyMemoryProfile = try sym("ips3_apply_memory_profile")
        firmwareVersion = try sym("ips3_firmware_version")
        installFirmware = try sym("ips3_install_firmware")
        installPkg = try sym("ips3_install_pkg")
        listGames = try sym("ips3_list_games")
        setSurface = try sym("ips3_set_surface")
        surfaceResized = try sym("ips3_surface_resized")
        boot = try sym("ips3_boot")
        state = try sym("ips3_state")
        pause = try sym("ips3_pause")
        resume = try sym("ips3_resume")
        stop = try sym("ips3_stop")
        openHomeMenu = try sym("ips3_open_home_menu")
        setBackgrounded = try sym("ips3_set_backgrounded")
        padSet = try sym("ips3_pad_set")
        padSetMotion = try sym("ips3_pad_set_motion")
        padGetRumble = try sym("ips3_pad_get_rumble")
        configGet = try sym("ips3_config_get")
        configSet = try sym("ips3_config_set")
        configSave = try sym("ips3_config_save")
        getFPS = try sym("ips3_get_fps")
        getTitle = try sym("ips3_get_title")
        logPath = try sym("ips3_log_path")
        free = try sym("ips3_free")

        let version = apiVersion()
        if version != CoreLibrary.expectedAPIVersion {
            throw LoadError.apiMismatch(version)
        }
    }

    /// Where the framework can be. Under LiveContainer the process belongs to the host app, so
    /// besides Bundle.main (which LiveContainer redirects to the guest bundle) the directory of
    /// this very binary is tried too.
    static func candidatePaths() -> [String] {
        var paths: [String] = []

        if let frameworks = Bundle.main.privateFrameworksPath {
            paths.append(frameworks + "/Ips3Core.framework/Ips3Core")
        }

        var info = Dl_info()
        if dladdr(#dsohandle, &info) != 0, let fname = info.dli_fname {
            let binaryDir = (String(cString: fname) as NSString).deletingLastPathComponent
            paths.append(binaryDir + "/Frameworks/Ips3Core.framework/Ips3Core")
        }

        paths.append(Bundle.main.bundlePath + "/Frameworks/Ips3Core.framework/Ips3Core")

        var seen = Set<String>()
        return paths.filter { seen.insert($0).inserted }
    }

    static func load() throws -> CoreLibrary {
        let paths = candidatePaths()
        guard let path = paths.first(where: { FileManager.default.fileExists(atPath: $0) }) else {
            throw LoadError.notFound(paths)
        }

        guard let handle = dlopen(path, RTLD_NOW | RTLD_LOCAL) else {
            throw LoadError.dlopen(String(cString: dlerror()))
        }

        return try CoreLibrary(handle: handle)
    }

    // MARK: - Swift conveniences

    func takeString(_ pointer: UnsafeMutablePointer<CChar>?) -> String? {
        guard let pointer else { return nil }
        defer { free(pointer) }
        return String(cString: pointer)
    }

    func config(_ path: String) -> String? {
        path.withCString { takeString(configGet($0)) }
    }

    @discardableResult
    func setConfig(_ path: String, _ value: String) -> Bool {
        path.withCString { p in value.withCString { v in configSet(p, v) } }
    }
}
