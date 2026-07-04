import {
  assembleFrame,
  chunkOffsets,
  frameToRgba,
  isValidPixelByte,
  pixelByteToGray,
} from "../src/frame";

describe("chunkOffsets", () => {
  it("returns evenly spaced offsets covering the full length", () => {
    expect(chunkOffsets(484, 128)).toEqual([0, 128, 256, 384]);
  });

  it("handles a length that is an exact multiple of chunk size", () => {
    expect(chunkOffsets(256, 128)).toEqual([0, 128]);
  });

  it("returns a single zero offset when length is smaller than chunk size", () => {
    expect(chunkOffsets(50, 128)).toEqual([0]);
  });

  it("returns an empty array for zero length", () => {
    expect(chunkOffsets(0, 128)).toEqual([]);
  });

  it("throws for a non-positive chunk size", () => {
    expect(() => chunkOffsets(100, 0)).toThrow();
  });
});

describe("isValidPixelByte / pixelByteToGray", () => {
  it("treats bit7 as the validity flag", () => {
    expect(isValidPixelByte(0x80)).toBe(true);
    expect(isValidPixelByte(0xff)).toBe(true);
    expect(isValidPixelByte(0x7f)).toBe(false);
    expect(isValidPixelByte(0x00)).toBe(false);
  });

  it("masks bit7 and shifts left by 1 for grayscale", () => {
    expect(pixelByteToGray(0x00)).toBe(0);
    expect(pixelByteToGray(0x7f)).toBe(0xfe);
    expect(pixelByteToGray(0xff)).toBe(0xfe); // bit7 masked off first
    expect(pixelByteToGray(0x80)).toBe(0);
    expect(pixelByteToGray(0x01)).toBe(2);
  });
});

describe("assembleFrame", () => {
  it("reassembles chunks in order into a contiguous buffer", () => {
    const chunks = [
      { offset: 0, data: Uint8Array.from([0x81, 0x82]) },
      { offset: 2, data: Uint8Array.from([0x83, 0x84]) },
    ];
    const result = assembleFrame(chunks, 4);
    expect(Array.from(result.bytes)).toEqual([0x81, 0x82, 0x83, 0x84]);
    expect(result.invalidCount).toBe(0);
  });

  it("reassembles out-of-order chunks correctly", () => {
    const chunks = [
      { offset: 2, data: Uint8Array.from([0x83, 0x84]) },
      { offset: 0, data: Uint8Array.from([0x81, 0x82]) },
    ];
    const result = assembleFrame(chunks, 4);
    expect(Array.from(result.bytes)).toEqual([0x81, 0x82, 0x83, 0x84]);
  });

  it("counts invalid (bit7 clear) bytes", () => {
    const chunks = [
      { offset: 0, data: Uint8Array.from([0x81, 0x02, 0x83, 0x00]) },
    ];
    const result = assembleFrame(chunks, 4);
    expect(result.invalidCount).toBe(2);
  });

  it("ignores chunk bytes that fall outside totalLength", () => {
    const chunks = [{ offset: 2, data: Uint8Array.from([0x81, 0x82, 0x83]) }];
    const result = assembleFrame(chunks, 4);
    // Only offsets 2 and 3 are in range; offset 4 (0x83) is dropped.
    expect(Array.from(result.bytes)).toEqual([0, 0, 0x81, 0x82]);
  });

  it("leaves unwritten bytes as zero (counted invalid)", () => {
    const chunks = [{ offset: 0, data: Uint8Array.from([0x81]) }];
    const result = assembleFrame(chunks, 3);
    expect(Array.from(result.bytes)).toEqual([0x81, 0, 0]);
    expect(result.invalidCount).toBe(2);
  });
});

describe("frameToRgba", () => {
  it("produces 4 bytes per pixel with equal R/G/B and full alpha", () => {
    const bytes = Uint8Array.from([0x80, 0xff]);
    const rgba = frameToRgba(bytes);
    expect(rgba.length).toBe(8);
    // pixel 0: masked value 0 -> gray 0
    expect(Array.from(rgba.slice(0, 4))).toEqual([0, 0, 0, 255]);
    // pixel 1: masked value 0x7f -> gray 0xfe
    expect(Array.from(rgba.slice(4, 8))).toEqual([0xfe, 0xfe, 0xfe, 255]);
  });
});
