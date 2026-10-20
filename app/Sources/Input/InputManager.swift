import Combine
import CoreHaptics
import Foundation
import GameController

/// One PS3 pad's worth of input, in the units ips3_pad_set takes.
struct PadState: Equatable {
    var digital1: UInt32 = 0
    var digital2: UInt32 = 0
    var lx: Int32 = 128
    var ly: Int32 = 128
    var rx: Int32 = 128
    var ry: Int32 = 128

    static let neutral = PadState()
}

/// CELL_PAD_CTRL_* bits, see ios/core/include/ips3_core.h.
enum PadButton: CaseIterable, Hashable {
    case select, l3, r3, start, up, right, down, left, ps
    case l2, r2, l1, r1, triangle, circle, cross, square

    var isDigital1: Bool {
        switch self {
        case .select, .l3, .r3, .start, .up, .right, .down, .left, .ps: return true
        default: return false
        }
    }

    var bit: UInt32 {
        switch self {
        case .select: return 0x01
        case .l3: return 0x02
        case .r3: return 0x04
        case .start: return 0x08
        case .up: return 0x10
        case .right: return 0x20
        case .down: return 0x40
        case .left: return 0x80
        case .ps: return 0x100
        case .l2: return 0x01
        case .r2: return 0x02
        case .l1: return 0x04
        case .r1: return 0x08
        case .triangle: return 0x10
        case .circle: return 0x20
        case .cross: return 0x40
        case .square: return 0x80
        }
    }
}

extension PadState {
    mutating func set(_ button: PadButton, _ pressed: Bool) {
        if button.isDigital1 {
            digital1 = pressed ? digital1 | button.bit : digital1 & ~button.bit
        } else {
            digital2 = pressed ? digital2 | button.bit : digital2 & ~button.bit
        }
    }

    /// -1...1 axes, y up, to the PS3's 0...255 with y down.
    static func axis(_ value: Float, invert: Bool = false) -> Int32 {
        let v = invert ? -value : value
        return Int32((max(-1, min(1, v)) * 127.5 + 127.5).rounded())
    }
}

/// Merges the touch overlay and GameController pads and pushes the result to the core at
/// display rate. Port 0 is shared by touch and the first controller; further controllers get
/// ports 1, 2, ...
final class InputManager: ObservableObject {
    @Published private(set) var controllers: [GCController] = []
    @Published var showTouchControls = true

    private var core: CoreLibrary?
    private var touch = PadState()
    private var pads: [ObjectIdentifier: PadState] = [:]
    private var lastSent: [Int: PadState] = [:]
    private var link: CADisplayLink?
    private var observers: [NSObjectProtocol] = []
    private var hapticEngines: [ObjectIdentifier: CHHapticEngine] = [:]
    private var lastRumble: [Int: Int32] = [:]
    private var hapticPlayers: [ObjectIdentifier: CHHapticAdvancedPatternPlayer] = [:]

    init() {
        let center = NotificationCenter.default
        observers.append(center.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] note in
            if let controller = note.object as? GCController { self?.connected(controller) }
        })
        observers.append(center.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] note in
            if let controller = note.object as? GCController { self?.disconnected(controller) }
        })

        GCController.shouldMonitorBackgroundEvents = false
        GCController.startWirelessControllerDiscovery {}
        for controller in GCController.controllers() {
            connected(controller)
        }
    }

    func attach(core: CoreLibrary) {
        self.core = core
        guard link == nil else { return }
        let link = CADisplayLink(target: self, selector: #selector(tick))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 120, preferred: 120)
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    // MARK: - Touch

    func setTouch(_ button: PadButton, _ pressed: Bool) {
        touch.set(button, pressed)
    }

    func setTouchStick(left: Bool, x: Float, y: Float) {
        if left {
            touch.lx = PadState.axis(x)
            touch.ly = PadState.axis(y, invert: true)
        } else {
            touch.rx = PadState.axis(x)
            touch.ry = PadState.axis(y, invert: true)
        }
    }

    // MARK: - Controllers

    private func port(of controller: GCController) -> Int {
        controllers.firstIndex(of: controller) ?? 0
    }

    private func connected(_ controller: GCController) {
        guard !controllers.contains(controller), let gamepad = controller.extendedGamepad else { return }
        controllers.append(controller)
        controller.playerIndex = GCControllerPlayerIndex(rawValue: controllers.count - 1) ?? .indexUnset
        showTouchControls = false

        let id = ObjectIdentifier(controller)
        pads[id] = PadState()
        gamepad.valueChangedHandler = { [weak self] gamepad, _ in
            self?.pads[id] = InputManager.read(gamepad)
        }

        if let engine = controller.haptics?.createEngine(withLocality: .default) {
            try? engine.start()
            hapticEngines[id] = engine
        }
    }

    private func disconnected(_ controller: GCController) {
        let id = ObjectIdentifier(controller)
        controllers.removeAll { $0 == controller }
        pads[id] = nil
        hapticEngines[id]?.stop()
        hapticEngines[id] = nil
        hapticPlayers[id] = nil
        if controllers.isEmpty {
            showTouchControls = true
        }
    }

    private static func read(_ g: GCExtendedGamepad) -> PadState {
        var s = PadState()
        // Face buttons by position: A (bottom) is Cross, as on a DualShock.
        s.set(.cross, g.buttonA.isPressed)
        s.set(.circle, g.buttonB.isPressed)
        s.set(.square, g.buttonX.isPressed)
        s.set(.triangle, g.buttonY.isPressed)
        s.set(.l1, g.leftShoulder.isPressed)
        s.set(.r1, g.rightShoulder.isPressed)
        s.set(.l2, g.leftTrigger.value > 0.3)
        s.set(.r2, g.rightTrigger.value > 0.3)
        s.set(.l3, g.leftThumbstickButton?.isPressed ?? false)
        s.set(.r3, g.rightThumbstickButton?.isPressed ?? false)
        s.set(.up, g.dpad.up.isPressed)
        s.set(.down, g.dpad.down.isPressed)
        s.set(.left, g.dpad.left.isPressed)
        s.set(.right, g.dpad.right.isPressed)
        s.set(.start, g.buttonMenu.isPressed)
        s.set(.select, g.buttonOptions?.isPressed ?? false)
        s.set(.ps, g.buttonHome?.isPressed ?? false)
        s.lx = PadState.axis(g.leftThumbstick.xAxis.value)
        s.ly = PadState.axis(g.leftThumbstick.yAxis.value, invert: true)
        s.rx = PadState.axis(g.rightThumbstick.xAxis.value)
        s.ry = PadState.axis(g.rightThumbstick.yAxis.value, invert: true)
        return s
    }

    // MARK: - Pump

    @objc private func tick() {
        guard let core else { return }

        var ports: [Int: PadState] = [0: touch]

        for controller in controllers {
            let port = self.port(of: controller)
            guard let pad = pads[ObjectIdentifier(controller)] else { continue }

            if port == 0 {
                // Touch and the first controller share port 0.
                var merged = touch
                merged.digital1 |= pad.digital1
                merged.digital2 |= pad.digital2
                if pad.lx != 128 || pad.ly != 128 { merged.lx = pad.lx; merged.ly = pad.ly }
                if pad.rx != 128 || pad.ry != 128 { merged.rx = pad.rx; merged.ry = pad.ry }
                ports[0] = merged
            } else {
                ports[port] = pad
            }
        }

        for (port, state) in ports where lastSent[port] != state {
            lastSent[port] = state
            core.padSet(Int32(port), state.digital1, state.digital2, state.lx, state.ly, state.rx, state.ry)
        }

        updateRumble(core)
    }

    private func updateRumble(_ core: CoreLibrary) {
        for controller in controllers {
            let id = ObjectIdentifier(controller)
            let port = self.port(of: controller)
            let value = core.padGetRumble(Int32(port))
            guard lastRumble[port] != value else { continue }
            lastRumble[port] = value

            guard let engine = hapticEngines[id] else { continue }
            let large = Float((value >> 8) & 0xff) / 255
            let small = Float(value & 0xff) / 255
            let intensity = max(large, small)

            if intensity <= 0.01 {
                try? hapticPlayers[id]?.stop(atTime: CHHapticTimeImmediate)
                hapticPlayers[id] = nil
                continue
            }

            let event = CHHapticEvent(
                eventType: .hapticContinuous,
                parameters: [
                    CHHapticEventParameter(parameterID: .hapticIntensity, value: intensity),
                    CHHapticEventParameter(parameterID: .hapticSharpness, value: large > small ? 0.2 : 0.8),
                ],
                relativeTime: 0,
                duration: 30
            )

            try? hapticPlayers[id]?.stop(atTime: CHHapticTimeImmediate)
            if let pattern = try? CHHapticPattern(events: [event], parameters: []),
               let player = try? engine.makeAdvancedPlayer(with: pattern) {
                try? player.start(atTime: CHHapticTimeImmediate)
                hapticPlayers[id] = player
            }
        }
    }
}
