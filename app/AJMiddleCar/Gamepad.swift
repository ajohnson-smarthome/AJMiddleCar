import GameController

@MainActor
final class Gamepad: ObservableObject {
    @Published var connected = false
    // Stick axes, up = +1 (GameController convention).
    @Published var leftX = 0.0
    @Published var leftY = 0.0
    @Published var rightY = 0.0

    init() {
        NotificationCenter.default.addObserver(self, selector: #selector(changed),
            name: .GCControllerDidConnect, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(changed),
            name: .GCControllerDidDisconnect, object: nil)
        attach(GCController.controllers().first)
    }

    @objc private func changed() {
        // NotificationCenter may post off the main queue; hop to the actor.
        Task { @MainActor in self.attach(GCController.controllers().first) }
    }

    private func attach(_ c: GCController?) {
        connected = (c?.extendedGamepad != nil)
        guard let gp = c?.extendedGamepad else { return }
        gp.leftThumbstick.valueChangedHandler = { [weak self] _, x, y in
            Task { @MainActor in self?.leftX = Double(x); self?.leftY = Double(y) }
        }
        gp.rightThumbstick.valueChangedHandler = { [weak self] _, _, y in
            Task { @MainActor in self?.rightY = Double(y) }
        }
    }
}
