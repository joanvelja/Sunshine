/**
 * @file tests/unit/platform/macos/test_nv12_zero_device.mm
 * @brief Unit tests for the BGRA to NV12 conversion in src/platform/macos/nv12_zero_device.*.
 */

// Only compile these tests on macOS
#ifdef __APPLE__

  #include "../../../tests_common.h"

  #include <cmath>
  #include <functional>

  #import <CoreVideo/CoreVideo.h>
  #include <src/platform/macos/nv12_zero_device.h>

namespace {
  /**
   * @brief An 8-bit color in byte order R, G, B.
   */
  struct rgb_t {
    uint8_t r;  ///< Red.
    uint8_t g;  ///< Green.
    uint8_t b;  ///< Blue.
  };

  constexpr rgb_t black {0, 0, 0};
  constexpr rgb_t white {255, 255, 255};
  constexpr rgb_t lavender {0xb1, 0xb9, 0xf9};  ///< A pale terminal-text color that loses its tint easily.
  constexpr rgb_t red {255, 0, 0};
  constexpr rgb_t blue {0, 0, 255};

  /**
   * @brief Chroma and luma samples read from an NV12 buffer.
   */
  struct ycbcr_t {
    int y;  ///< Luma of the top-left pixel.
    int cb;  ///< Cb of the top-left 2x2 block.
    int cr;  ///< Cr of the top-left 2x2 block.
  };

  /**
   * @brief Owns a CVPixelBuffer for the duration of a test.
   */
  struct pixel_buffer_t {
    CVPixelBufferRef buf {};  ///< Owned buffer.

    pixel_buffer_t(int width, int height, OSType format) {
      CVPixelBufferCreate(kCFAllocatorDefault, width, height, format, nullptr, &buf);
    }

    ~pixel_buffer_t() {
      CVPixelBufferRelease(buf);
    }
  };

  /**
   * @brief Fill a BGRA buffer with a color chosen per pixel.
   *
   * @param buffer BGRA buffer to fill.
   * @param color Returns the color at (x, y).
   */
  void fill_bgra(CVPixelBufferRef buffer, const std::function<rgb_t(int, int)> &color) {
    CVPixelBufferLockBaseAddress(buffer, 0);
    auto *base = static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(buffer));
    const auto stride {CVPixelBufferGetBytesPerRow(buffer)};
    for (int y = 0; y < (int) CVPixelBufferGetHeight(buffer); ++y) {
      for (int x = 0; x < (int) CVPixelBufferGetWidth(buffer); ++x) {
        const auto c {color(x, y)};
        auto *px = base + y * stride + x * 4;
        px[0] = c.b;
        px[1] = c.g;
        px[2] = c.r;
        px[3] = 255;
      }
    }
    CVPixelBufferUnlockBaseAddress(buffer, 0);
  }

  /**
   * @brief Convert a 2x2 BGRA pattern and read back the top-left block.
   *
   * @param colorspace Colorspace to convert with.
   * @param color Returns the color at (x, y).
   * @return The converted samples.
   */
  ycbcr_t convert_block(const video::sunshine_colorspace_t &colorspace, const std::function<rgb_t(int, int)> &color) {
    pixel_buffer_t bgra {2, 2, kCVPixelFormatType_32BGRA};
    pixel_buffer_t nv12 {2, 2, colorspace.full_range ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange};
    fill_bgra(bgra.buf, color);

    vImage_ARGBToYpCbCr info;
    EXPECT_EQ(platf::make_argb_to_ypcbcr_info(colorspace, info), kvImageNoError);

    CVPixelBufferLockBaseAddress(bgra.buf, kCVPixelBufferLock_ReadOnly);
    EXPECT_EQ(platf::convert_bgra_to_nv12(bgra.buf, nv12.buf, info), 0);
    CVPixelBufferUnlockBaseAddress(bgra.buf, kCVPixelBufferLock_ReadOnly);

    CVPixelBufferLockBaseAddress(nv12.buf, kCVPixelBufferLock_ReadOnly);
    const auto *yp = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(nv12.buf, 0));
    const auto *cbcr = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(nv12.buf, 1));
    const ycbcr_t out {yp[0], cbcr[0], cbcr[1]};
    CVPixelBufferUnlockBaseAddress(nv12.buf, kCVPixelBufferLock_ReadOnly);
    return out;
  }

  /**
   * @brief Convert a solid 2x2 block of one color.
   *
   * @param colorspace Colorspace to convert with.
   * @param c Color of every pixel.
   * @return The converted samples.
   */
  ycbcr_t convert_solid(const video::sunshine_colorspace_t &colorspace, rgb_t c) {
    return convert_block(colorspace, [c](int, int) {
      return c;
    });
  }

  constexpr video::sunshine_colorspace_t rec709_limited {video::colorspace_e::rec709, false, 8};
  constexpr video::sunshine_colorspace_t rec709_full {video::colorspace_e::rec709, true, 8};
  constexpr video::sunshine_colorspace_t rec601_limited {video::colorspace_e::rec601, false, 8};

  /**
   * @brief Assert that a mixed block's chroma is the average of its two colors.
   *
   * Decimating converters keep one pixel's chroma, so they land on one of the two
   * solid values instead of between them.
   *
   * @param mixed Samples of the block that contains both colors.
   * @param a Samples of a solid block of the first color.
   * @param b Samples of a solid block of the second color.
   */
  void expect_average_chroma(const ycbcr_t &mixed, const ycbcr_t &a, const ycbcr_t &b) {
    EXPECT_NEAR(mixed.cb, (a.cb + b.cb) / 2.0, 2.0);
    EXPECT_NEAR(mixed.cr, (a.cr + b.cr) / 2.0, 2.0);
  }
}  // namespace

TEST(Nv12ConversionTest, AveragesChromaOfOnePixelVerticalLine) {
  const auto mixed {convert_block(rec709_limited, [](int x, int) {
    return x == 1 ? lavender : black;
  })};
  expect_average_chroma(mixed, convert_solid(rec709_limited, lavender), convert_solid(rec709_limited, black));
}

TEST(Nv12ConversionTest, AveragesChromaOfOnePixelHorizontalLine) {
  const auto mixed {convert_block(rec709_limited, [](int, int y) {
    return y == 0 ? lavender : black;
  })};
  expect_average_chroma(mixed, convert_solid(rec709_limited, lavender), convert_solid(rec709_limited, black));
}

TEST(Nv12ConversionTest, AveragesChromaOfAlternatingRedBlueColumns) {
  const auto mixed {convert_block(rec709_limited, [](int x, int) {
    return x % 2 ? blue : red;
  })};
  expect_average_chroma(mixed, convert_solid(rec709_limited, red), convert_solid(rec709_limited, blue));
}

TEST(Nv12ConversionTest, LimitedRangeLevels) {
  const auto w {convert_solid(rec709_limited, white)};
  const auto k {convert_solid(rec709_limited, black)};
  EXPECT_NEAR(w.y, 235, 1);
  EXPECT_NEAR(k.y, 16, 1);
  EXPECT_NEAR(w.cb, 128, 1);
  EXPECT_NEAR(w.cr, 128, 1);
  EXPECT_NEAR(k.cb, 128, 1);
  EXPECT_NEAR(k.cr, 128, 1);
}

TEST(Nv12ConversionTest, FullRangeLevels) {
  const auto w {convert_solid(rec709_full, white)};
  const auto k {convert_solid(rec709_full, black)};
  EXPECT_NEAR(w.y, 255, 1);
  EXPECT_NEAR(k.y, 0, 1);
  EXPECT_NEAR(w.cb, 128, 1);
  EXPECT_NEAR(k.cr, 128, 1);
}

TEST(Nv12ConversionTest, MatrixFollowsColorspace) {
  // Luma of pure red is 16 + 219 * Kr: Kr = 0.2126 for Rec. 709 and 0.299 for Rec. 601.
  EXPECT_NEAR(convert_solid(rec709_limited, red).y, 16 + 219 * 0.2126, 1.0);
  EXPECT_NEAR(convert_solid(rec601_limited, red).y, 16 + 219 * 0.299, 1.0);
  // Pure red maximizes Cr in every matrix: 128 + 112 in limited range.
  EXPECT_NEAR(convert_solid(rec709_limited, red).cr, 240, 1);
  EXPECT_NEAR(convert_solid(rec601_limited, red).cr, 240, 1);
}

TEST(Nv12ConversionTest, RejectsMismatchedDimensions) {
  pixel_buffer_t bgra {4, 4, kCVPixelFormatType_32BGRA};
  pixel_buffer_t nv12 {2, 2, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange};
  vImage_ARGBToYpCbCr info;
  ASSERT_EQ(platf::make_argb_to_ypcbcr_info(rec709_limited, info), kvImageNoError);
  EXPECT_NE(platf::convert_bgra_to_nv12(bgra.buf, nv12.buf, info), 0);
}

TEST(Nv12ConversionTest, RejectsNonBgraSource) {
  pixel_buffer_t src {2, 2, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange};
  pixel_buffer_t nv12 {2, 2, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange};
  vImage_ARGBToYpCbCr info;
  ASSERT_EQ(platf::make_argb_to_ypcbcr_info(rec709_limited, info), kvImageNoError);
  EXPECT_NE(platf::convert_bgra_to_nv12(src.buf, nv12.buf, info), 0);
}

#endif
