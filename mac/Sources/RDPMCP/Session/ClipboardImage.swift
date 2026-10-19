import AppKit

/// Omzetting tussen Windows-klembordafbeeldingen (CF_DIB) en macOS-afbeeldingen.
enum ClipboardImage {
    private static let maxPixels = 8192 * 8192

    // MARK: - CF_DIB naar NSImage

    static func image(fromDIB dib: Data) -> NSImage? {
        guard dib.count > 40 else { return nil }
        let headerSize = Int(dib.u32(0))
        let width = Int(Int32(bitPattern: dib.u32(4)))
        let rawHeight = Int(Int32(bitPattern: dib.u32(8)))
        let bitCount = Int(dib.u16(14))
        let compression = dib.u32(16)
        let colorsUsed = Int(dib.u32(32))
        let height = abs(rawHeight)
        guard headerSize >= 40, width > 0, height > 0, width * height <= maxPixels else { return nil }

        var pixelOffset = headerSize
        if headerSize == 40 && compression == 3 { pixelOffset += 12 } // BI_BITFIELDS-maskers
        if headerSize == 40 && compression == 6 { pixelOffset += 16 } // BI_ALPHABITFIELDS
        if bitCount <= 8 {
            pixelOffset += (colorsUsed != 0 ? colorsUsed : 1 << bitCount) * 4
        } else {
            pixelOffset += colorsUsed * 4
        }

        // Gangbare formaten zelf lezen en de alfa negeren: Windows zet die vaak op 0
        let standardMasks = compression == 0
            || (compression == 3 && (headerSize > 40 || dib.count >= 52)
                && dib.u32(40) == 0x00FF_0000 && dib.u32(44) == 0x0000_FF00 && dib.u32(48) == 0x0000_00FF)
        if (bitCount == 32 || bitCount == 24) && standardMasks {
            return decodeRGB(dib, width: width, height: height, topDown: rawHeight < 0,
                             bytesPerPixel: bitCount / 8, offset: pixelOffset)
        }

        // Andere varianten (palet, RLE, ...) via de BMP-lezer van macOS
        var file = Data([0x42, 0x4D]) // "BM"
        file.appendLE(UInt32(14 + dib.count))
        file.appendLE(UInt32(0))
        file.appendLE(UInt32(14 + pixelOffset))
        file.append(dib)
        return NSImage(data: file)
    }

    private static func decodeRGB(_ dib: Data, width: Int, height: Int, topDown: Bool,
                                  bytesPerPixel: Int, offset: Int) -> NSImage? {
        let srcStride = (width * bytesPerPixel + 3) & ~3
        guard offset + srcStride * height <= dib.count else { return nil }
        var pixels = Data(count: width * height * 4)
        dib.withUnsafeBytes { (src: UnsafeRawBufferPointer) in
            pixels.withUnsafeMutableBytes { (dst: UnsafeMutableRawBufferPointer) in
                for y in 0 ..< height {
                    let srcRow = topDown ? y : height - 1 - y
                    let s = src.baseAddress! + offset + srcRow * srcStride
                    let d = dst.baseAddress! + y * width * 4
                    for x in 0 ..< width {
                        let sp = s + x * bytesPerPixel
                        let dp = d + x * 4
                        dp.storeBytes(of: sp.load(as: UInt8.self), as: UInt8.self)                      // B
                        (dp + 1).storeBytes(of: (sp + 1).load(as: UInt8.self), as: UInt8.self)          // G
                        (dp + 2).storeBytes(of: (sp + 2).load(as: UInt8.self), as: UInt8.self)          // R
                        (dp + 3).storeBytes(of: 0xFF, as: UInt8.self)
                    }
                }
            }
        }
        guard let provider = CGDataProvider(data: pixels as CFData),
              let cg = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
                               bytesPerRow: width * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                               bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue
                                   | CGBitmapInfo.byteOrder32Little.rawValue),
                               provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
        else { return nil }
        return NSImage(cgImage: cg, size: NSSize(width: width, height: height))
    }

    // MARK: - macOS-klembord naar CF_DIB

    /// Afbeelding op het Mac-klembord als CF_DIB (32 bit, BI_RGB, onderaan beginnend).
    /// Gekopieerde bestanden in de Finder worden overgeslagen: dan staat er enkel een icoon.
    static func dib(from pasteboard: NSPasteboard) -> Data? {
        let types = pasteboard.types ?? []
        guard !types.contains(.fileURL),
              types.contains(.png) || types.contains(.tiff),
              let image = NSImage(pasteboard: pasteboard),
              let cg = image.cgImage(forProposedRect: nil, context: nil, hints: nil)
        else { return nil }
        return dib(from: cg)
    }

    static func dib(from image: CGImage) -> Data? {
        let width = image.width
        let height = image.height
        guard width > 0, height > 0, width * height <= maxPixels else { return nil }
        var data = Data(count: 40 + width * height * 4)
        data.withUnsafeMutableBytes { (buf: UnsafeMutableRawBufferPointer) in
            let base = buf.baseAddress!
            func put32(_ offset: Int, _ value: UInt32) { (base + offset).storeBytes(of: value.littleEndian, as: UInt32.self) }
            func put16(_ offset: Int, _ value: UInt16) { (base + offset).storeBytes(of: value.littleEndian, as: UInt16.self) }
            put32(0, 40)                                   // biSize
            put32(4, UInt32(width))                        // biWidth
            put32(8, UInt32(height))                       // biHeight > 0: onderaan beginnend
            put16(12, 1)                                   // biPlanes
            put16(14, 32)                                  // biBitCount
            put32(16, 0)                                   // BI_RGB
            put32(20, UInt32(width * height * 4))          // biSizeImage
            put32(24, 2835)                                // 72 dpi
            put32(28, 2835)
            put32(32, 0)
            put32(36, 0)
            guard let ctx = CGContext(data: base + 40, width: width, height: height, bitsPerComponent: 8,
                                      bytesPerRow: width * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                      bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue
                                          | CGBitmapInfo.byteOrder32Little.rawValue)
            else { return }
            // transparante delen wit maken, en verticaal spiegelen voor een onderaan beginnende DIB
            ctx.setFillColor(CGColor(gray: 1, alpha: 1))
            ctx.fill(CGRect(x: 0, y: 0, width: width, height: height))
            ctx.translateBy(x: 0, y: CGFloat(height))
            ctx.scaleBy(x: 1, y: -1)
            ctx.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
        }
        return data
    }
}

private extension Data {
    func u32(_ offset: Int) -> UInt32 {
        guard offset + 4 <= count else { return 0 }
        return withUnsafeBytes { $0.loadUnaligned(fromByteOffset: offset, as: UInt32.self) }.littleEndian
    }

    func u16(_ offset: Int) -> UInt16 {
        guard offset + 2 <= count else { return 0 }
        return withUnsafeBytes { $0.loadUnaligned(fromByteOffset: offset, as: UInt16.self) }.littleEndian
    }

    mutating func appendLE(_ value: UInt32) {
        Swift.withUnsafeBytes(of: value.littleEndian) { append(contentsOf: $0) }
    }
}
