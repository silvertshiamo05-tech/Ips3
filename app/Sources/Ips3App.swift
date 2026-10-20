import AVFoundation
import SwiftUI

@main
struct Ips3App: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var emulator = Emulator.shared
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(emulator)
                .preferredColorScheme(.dark)
                .onAppear {
                    if emulator.phase == .checking {
                        emulator.start()
                    }
                }
        }
        .onChange(of: scenePhase) { _, phase in
            emulator.scenePhaseChanged(phase)
        }
    }
}

struct RootView: View {
    @EnvironmentObject var emulator: Emulator

    var body: some View {
        switch emulator.phase {
        case .checking:
            ProgressView("Checking device…")
        case .blocked:
            SetupView()
        case .failed(let message):
            SetupView(failure: message)
        case .ready:
            if let game = emulator.running {
                EmulationView(input: emulator.input, game: game)
            } else {
                LibraryView()
            }
        }
    }
}

final class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
        try? session.setPreferredIOBufferDuration(0.01)
        try? session.setActive(true)
        return true
    }

    func applicationDidReceiveMemoryWarning(_ application: UIApplication) {
        // Nothing the core can drop on demand yet; record it, since it explains a later kill.
        NSLog("Ips3: memory warning, footprint %llu MB", Preflight.residentFootprint() >> 20)
    }
}
