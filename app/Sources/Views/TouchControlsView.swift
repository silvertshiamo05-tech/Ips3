import SwiftUI

/// The on-screen DualShock 3. Every control has its own gesture so several can be held at
/// once (SwiftUI delivers simultaneous touches to separate views independently).
struct TouchControlsView: View {
    let input: InputManager
    var opacity: Double = 0.45

    var body: some View {
        GeometryReader { geo in
            let unit = min(geo.size.width, geo.size.height) / 9

            ZStack {
                // Shoulders
                VStack {
                    HStack {
                        VStack(spacing: unit * 0.15) {
                            PadKey(label: "L2", button: .l2, input: input, size: CGSize(width: unit * 1.6, height: unit * 0.7))
                            PadKey(label: "L1", button: .l1, input: input, size: CGSize(width: unit * 1.6, height: unit * 0.7))
                        }
                        Spacer()
                        VStack(spacing: unit * 0.15) {
                            PadKey(label: "R2", button: .r2, input: input, size: CGSize(width: unit * 1.6, height: unit * 0.7))
                            PadKey(label: "R1", button: .r1, input: input, size: CGSize(width: unit * 1.6, height: unit * 0.7))
                        }
                    }
                    Spacer()
                }
                .padding(unit * 0.3)

                // Left: d-pad above the left stick
                VStack(spacing: unit * 0.3) {
                    Spacer()
                    DPad(input: input, unit: unit)
                    HStack {
                        AnalogStick(left: true, input: input, radius: unit * 1.1)
                        Spacer()
                    }
                }
                .padding(.leading, unit * 0.4)
                .padding(.bottom, unit * 0.3)
                .frame(maxWidth: .infinity, alignment: .leading)

                // Right: face buttons above the right stick
                VStack(spacing: unit * 0.3) {
                    Spacer()
                    FaceButtons(input: input, unit: unit)
                    HStack {
                        Spacer()
                        AnalogStick(left: false, input: input, radius: unit * 1.1)
                    }
                }
                .padding(.trailing, unit * 0.4)
                .padding(.bottom, unit * 0.3)
                .frame(maxWidth: .infinity, alignment: .trailing)

                // Centre: Select, PS, Start, L3/R3
                VStack {
                    Spacer()
                    HStack(spacing: unit * 0.5) {
                        PadKey(label: "L3", button: .l3, input: input, size: CGSize(width: unit * 0.8, height: unit * 0.55))
                        PadKey(label: "SELECT", button: .select, input: input, size: CGSize(width: unit * 1.2, height: unit * 0.55))
                        PadKey(label: "PS", button: .ps, input: input, size: CGSize(width: unit * 0.8, height: unit * 0.55))
                        PadKey(label: "START", button: .start, input: input, size: CGSize(width: unit * 1.2, height: unit * 0.55))
                        PadKey(label: "R3", button: .r3, input: input, size: CGSize(width: unit * 0.8, height: unit * 0.55))
                    }
                    .padding(.bottom, unit * 0.3)
                }
            }
            .opacity(opacity)
        }
        .ignoresSafeArea()
    }
}

private struct PadKey: View {
    let label: String
    let button: PadButton
    let input: InputManager
    let size: CGSize
    var circle = false

    @State private var pressed = false

    var body: some View {
        let shape = RoundedRectangle(cornerRadius: circle ? size.width / 2 : 10, style: .continuous)

        shape
            .fill(pressed ? Color.white.opacity(0.6) : Color.black.opacity(0.35))
            .overlay(shape.stroke(Color.white.opacity(0.8), lineWidth: 2))
            .overlay(
                Text(label)
                    .font(.system(size: max(10, size.height * 0.35), weight: .bold, design: .rounded))
                    .foregroundStyle(.white)
            )
            .frame(width: size.width, height: size.height)
            .contentShape(shape)
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { _ in
                        if !pressed {
                            pressed = true
                            input.setTouch(button, true)
                            UIImpactFeedbackGenerator(style: .light).impactOccurred()
                        }
                    }
                    .onEnded { _ in
                        pressed = false
                        input.setTouch(button, false)
                    }
            )
    }
}

private struct FaceButtons: View {
    let input: InputManager
    let unit: CGFloat

    var body: some View {
        let size = CGSize(width: unit * 0.95, height: unit * 0.95)
        VStack(spacing: unit * 0.05) {
            PadKey(label: "△", button: .triangle, input: input, size: size, circle: true)
            HStack(spacing: unit * 0.9) {
                PadKey(label: "□", button: .square, input: input, size: size, circle: true)
                PadKey(label: "○", button: .circle, input: input, size: size, circle: true)
            }
            PadKey(label: "✕", button: .cross, input: input, size: size, circle: true)
        }
    }
}

/// One view for all four directions so sliding a thumb between them (and onto diagonals)
/// works the way a real d-pad does.
private struct DPad: View {
    let input: InputManager
    let unit: CGFloat

    @State private var active: Set<PadButton> = []

    var body: some View {
        let side = unit * 2.6

        ZStack {
            Image(systemName: "dpad")
                .resizable()
                .scaledToFit()
                .foregroundStyle(.white.opacity(0.85))
            ForEach(Array(active), id: \.self) { button in
                Image(systemName: Self.symbol(for: button))
                    .resizable()
                    .scaledToFit()
                    .foregroundStyle(.white)
            }
        }
        .frame(width: side, height: side)
        .contentShape(Rectangle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { value in
                    let dx = value.location.x - side / 2
                    let dy = value.location.y - side / 2
                    let dead = side * 0.12
                    var now: Set<PadButton> = []
                    if dx < -dead { now.insert(.left) }
                    if dx > dead { now.insert(.right) }
                    if dy < -dead { now.insert(.up) }
                    if dy > dead { now.insert(.down) }
                    update(now)
                }
                .onEnded { _ in update([]) }
        )
    }

    private func update(_ now: Set<PadButton>) {
        guard now != active else { return }
        for button in active.subtracting(now) { input.setTouch(button, false) }
        for button in now.subtracting(active) { input.setTouch(button, true) }
        if !now.subtracting(active).isEmpty {
            UIImpactFeedbackGenerator(style: .light).impactOccurred()
        }
        active = now
    }

    private static func symbol(for button: PadButton) -> String {
        switch button {
        case .up: return "dpad.up.filled"
        case .down: return "dpad.down.filled"
        case .left: return "dpad.left.filled"
        default: return "dpad.right.filled"
        }
    }
}

private struct AnalogStick: View {
    let left: Bool
    let input: InputManager
    let radius: CGFloat

    @State private var offset: CGSize = .zero

    var body: some View {
        ZStack {
            Circle()
                .fill(Color.black.opacity(0.3))
                .overlay(Circle().stroke(Color.white.opacity(0.8), lineWidth: 2))
                .frame(width: radius * 2, height: radius * 2)
            Circle()
                .fill(Color.white.opacity(0.7))
                .frame(width: radius * 0.9, height: radius * 0.9)
                .offset(offset)
        }
        .contentShape(Circle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { value in
                    var dx = value.location.x - radius
                    var dy = value.location.y - radius
                    let length = sqrt(dx * dx + dy * dy)
                    if length > radius {
                        dx *= radius / length
                        dy *= radius / length
                    }
                    offset = CGSize(width: dx, height: dy)
                    input.setTouchStick(left: left, x: Float(dx / radius), y: Float(-dy / radius))
                }
                .onEnded { _ in
                    offset = .zero
                    input.setTouchStick(left: left, x: 0, y: 0)
                }
        )
    }
}
