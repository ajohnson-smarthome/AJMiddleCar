import SwiftUI

struct Palette {
    let bg: Color, panel: Color, line: Color, text: Color, muted: Color, accent: Color, idleWheel: Color, warn: Color
    // Car artwork: wheel/edge "metal" and the unlit segment of the power bars.
    let metal: Color, segOff: Color
}

enum Theme {
    static let dark = Palette(
        bg:        Color(red: 0.067, green: 0.059, blue: 0.047),
        panel:     Color(red: 0.106, green: 0.090, blue: 0.067),
        line:      Color(red: 0.169, green: 0.145, blue: 0.110),
        text:      Color(red: 0.702, green: 0.675, blue: 0.624),
        muted:     Color(red: 0.420, green: 0.388, blue: 0.337),
        accent:    Color(red: 0.290, green: 0.871, blue: 0.502),
        idleWheel: Color(red: 0.227, green: 0.353, blue: 0.267),
        warn:      Color(red: 0.878, green: 0.643, blue: 0.188),
        metal:     Color(red: 0.227, green: 0.188, blue: 0.141),   // #3a3024
        segOff:    Color(red: 0.141, green: 0.122, blue: 0.090))   // #241f17
    static let light = Palette(
        bg:        Color(red: 0.957, green: 0.941, blue: 0.910),
        panel:     Color(red: 1.000, green: 0.992, blue: 0.972),
        line:      Color(red: 0.910, green: 0.875, blue: 0.812),
        text:      Color(red: 0.416, green: 0.388, blue: 0.353),
        muted:     Color(red: 0.647, green: 0.612, blue: 0.557),
        accent:    Color(red: 0.082, green: 0.502, blue: 0.239),
        idleWheel: Color(red: 0.812, green: 0.890, blue: 0.824),
        warn:      Color(red: 0.722, green: 0.475, blue: 0.122),
        metal:     Color(red: 0.557, green: 0.522, blue: 0.471),   // warm grey-brown rubber
        segOff:    Color(red: 0.867, green: 0.835, blue: 0.784))   // soft sand
    static func current(_ scheme: ColorScheme) -> Palette { scheme == .dark ? dark : light }
}
