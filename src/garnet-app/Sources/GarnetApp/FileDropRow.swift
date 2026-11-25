import SwiftUI
import UniformTypeIdentifiers

struct FileDropRow: View {
    let systemImage: String
    let message: String
    let buttonTitle: String
    let disabled: Bool
    let onPick: () -> Void
    let onDropURL: (URL) -> Void

    @State private var isTargeted = false

    init(
        systemImage: String,
        message: String,
        buttonTitle: String,
        disabled: Bool = false,
        onPick: @escaping () -> Void,
        onDropURL: @escaping (URL) -> Void
    ) {
        self.systemImage = systemImage
        self.message = message
        self.buttonTitle = buttonTitle
        self.disabled = disabled
        self.onPick = onPick
        self.onDropURL = onDropURL
    }

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: systemImage)
                .foregroundColor(isTargeted ? .accentColor : .secondary)
                .frame(width: 20)
            Text(message)
                .font(.caption)
                .foregroundColor(.secondary)
            Spacer()
            Button(buttonTitle, action: onPick)
                .disabled(disabled)
        }
        .padding(.vertical, 4)
        .padding(.horizontal, 6)
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(isTargeted ? Color.accentColor.opacity(0.12) : Color.clear)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 6)
                .stroke(isTargeted ? Color.accentColor : Color.clear, lineWidth: 1.5)
        )
        .onDrop(of: [.fileURL], isTargeted: $isTargeted) { providers in
            guard !disabled,
                  let provider = providers.first(where: { $0.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier) }) else {
                return false
            }
            provider.loadItem(forTypeIdentifier: UTType.fileURL.identifier, options: nil) { item, _ in
                let url = (item as? URL) ?? (item as? Data).flatMap { URL(dataRepresentation: $0, relativeTo: nil) }
                guard let url else { return }
                Task { @MainActor in
                    onDropURL(url)
                }
            }
            return true
        }
    }
}
