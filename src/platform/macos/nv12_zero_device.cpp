/**
 * @file src/platform/macos/nv12_zero_device.cpp
 * @brief Definitions for NV12 zero copy device on macOS.
 */
// standard includes
#include <utility>

// local includes
#include "src/logging.h"
#include "src/platform/macos/av_img_t.h"
#include "src/platform/macos/nv12_zero_device.h"
#include "src/video.h"

extern "C" {
#include "libavutil/imgutils.h"
}

namespace platf {
  using namespace std::literals;

  /**
   * @brief Release an FFmpeg frame allocated by the capture or conversion backend.
   */
  void free_frame(AVFrame *frame) {
    av_frame_free(&frame);
  }

  /**
   * @brief Release a backend buffer allocated for capture or conversion.
   *
   * @param opaque Opaque user pointer provided to the callback.
   * @param data Payload or state data to serialize, deserialize, or forward.
   */
  void free_buffer(void *opaque, uint8_t *data) {
    CVPixelBufferRelease((CVPixelBufferRef) data);
  }

  util::safe_ptr<AVFrame, free_frame> av_frame;  ///< AV frame.

  vImage_Error make_argb_to_ypcbcr_info(const video::sunshine_colorspace_t &colorspace, vImage_ARGBToYpCbCr &info) {
    // Luma coefficients (Kr, Kb) per ITU-R BT.601, BT.709 and BT.2020.
    float kr;
    float kb;
    switch (colorspace.colorspace) {
      case video::colorspace_e::rec601:
        kr = 0.299f;
        kb = 0.114f;
        break;
      case video::colorspace_e::rec709:
        kr = 0.2126f;
        kb = 0.0722f;
        break;
      case video::colorspace_e::bt2020sdr:
      case video::colorspace_e::bt2020:
        kr = 0.2627f;
        kb = 0.0593f;
        break;
      default:
        return kvImageInvalidParameter;
    }
    const float kg {1.0f - kr - kb};

    const vImage_ARGBToYpCbCrMatrix matrix {
      .R_Yp = kr,
      .G_Yp = kg,
      .B_Yp = kb,
      .R_Cb = -kr / (2.0f * (1.0f - kb)),
      .G_Cb = -kg / (2.0f * (1.0f - kb)),
      .B_Cb_R_Cr = 0.5f,
      .G_Cr = -kg / (2.0f * (1.0f - kr)),
      .B_Cr = -kb / (2.0f * (1.0f - kr)),
    };

    // Field order: Yp_bias, CbCr_bias, YpRangeMax, CbCrRangeMax, YpMax, YpMin, CbCrMax, CbCrMin.
    const vImage_YpCbCrPixelRange range = colorspace.full_range ?
                                            vImage_YpCbCrPixelRange {0, 128, 255, 255, 255, 0, 255, 0} :
                                            vImage_YpCbCrPixelRange {16, 128, 235, 240, 235, 16, 240, 16};

    return vImageConvert_ARGBToYpCbCr_GenerateConversion(&matrix, &range, &info, kvImageARGB8888, kvImage420Yp8_CbCr8, kvImageNoFlags);
  }

  int convert_bgra_to_nv12(CVPixelBufferRef bgra, CVPixelBufferRef nv12, const vImage_ARGBToYpCbCr &info) {
    const auto nv12_format {CVPixelBufferGetPixelFormatType(nv12)};
    if (CVPixelBufferGetPixelFormatType(bgra) != kCVPixelFormatType_32BGRA || (nv12_format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange && nv12_format != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)) {
      BOOST_LOG(error) << "BGRA to NV12: unexpected pixel formats"sv;
      return -1;
    }

    const auto width {CVPixelBufferGetWidth(bgra)};
    const auto height {CVPixelBufferGetHeight(bgra)};
    if (width != CVPixelBufferGetWidth(nv12) || height != CVPixelBufferGetHeight(nv12)) {
      BOOST_LOG(error) << "BGRA to NV12: captured frame is "sv << width << 'x' << height
                       << " but encoder frame is "sv << CVPixelBufferGetWidth(nv12) << 'x' << CVPixelBufferGetHeight(nv12);
      return -1;
    }

    if (CVPixelBufferLockBaseAddress(nv12, 0) != kCVReturnSuccess) {
      BOOST_LOG(error) << "BGRA to NV12: unable to lock destination buffer"sv;
      return -1;
    }

    const vImage_Buffer src {CVPixelBufferGetBaseAddress(bgra), height, width, CVPixelBufferGetBytesPerRow(bgra)};
    const vImage_Buffer dst_yp {
      CVPixelBufferGetBaseAddressOfPlane(nv12, 0),
      CVPixelBufferGetHeightOfPlane(nv12, 0),
      CVPixelBufferGetWidthOfPlane(nv12, 0),
      CVPixelBufferGetBytesPerRowOfPlane(nv12, 0),
    };
    const vImage_Buffer dst_cbcr {
      CVPixelBufferGetBaseAddressOfPlane(nv12, 1),
      CVPixelBufferGetHeightOfPlane(nv12, 1),
      CVPixelBufferGetWidthOfPlane(nv12, 1),
      CVPixelBufferGetBytesPerRowOfPlane(nv12, 1),
    };

    // vImage expects ARGB channel order; the source bytes are B, G, R, A.
    const uint8_t bgra_to_argb[4] {3, 2, 1, 0};
    const auto status {vImageConvert_ARGB8888To420Yp8_CbCr8(&src, &dst_yp, &dst_cbcr, &info, bgra_to_argb, kvImageNoFlags)};

    CVPixelBufferUnlockBaseAddress(nv12, 0);

    if (status != kvImageNoError) {
      BOOST_LOG(error) << "BGRA to NV12: vImage conversion failed ("sv << status << ')';
      return -1;
    }
    return 0;
  }

  nv12_zero_device::~nv12_zero_device() {
    CVPixelBufferPoolRelease(nv12_pool);
  }

  int nv12_zero_device::convert(platf::img_t &img) {
    auto *av_img = (av_img_t *) &img;
    CVPixelBufferRef pixel_buffer {av_img->pixel_buffer->buf};

    // All sessions share one capture output, configured by whichever session started it. A
    // session with a different resolution or bit depth gets frames it can't convert; hand those
    // to VideoToolbox unchanged, which converts (and decimates chroma) as it did before.
    const bool convertible {
      CVPixelBufferGetPixelFormatType(pixel_buffer) == kCVPixelFormatType_32BGRA &&
      (int) CVPixelBufferGetWidth(pixel_buffer) == frame->width && (int) CVPixelBufferGetHeight(pixel_buffer) == frame->height
    };
    if (convert_bgra && !convertible && !warned_unconvertible) {
      BOOST_LOG(warning) << "Captured frames don't match this session's format; encoding them without chroma filtering"sv;
      warned_unconvertible = true;
    }

    if (convert_bgra && convertible) {
      if (!ypcbcr_info_ready || !nv12_pool) {
        BOOST_LOG(error) << "BGRA to NV12 conversion is not initialized"sv;
        return -1;
      }

      CVPixelBufferRef nv12 {};
      if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, nv12_pool, &nv12) != kCVReturnSuccess) {
        BOOST_LOG(error) << "Unable to allocate NV12 pixel buffer"sv;
        return -1;
      }
      if (convert_bgra_to_nv12(pixel_buffer, nv12, ypcbcr_info)) {
        CVPixelBufferRelease(nv12);
        return -1;
      }
      pixel_buffer = nv12;  // +1 reference, handed to the AVBufferRef below
    } else {
      CFRetain(pixel_buffer);
    }

    // Release any existing CVPixelBuffer previously retained for encoding
    av_buffer_unref(&av_frame->buf[0]);

    // Attach an AVBufferRef to this frame which will retain ownership of the CVPixelBuffer
    // until av_buffer_unref() is called (above) or the frame is freed with av_frame_free().
    //
    // The presence of the AVBufferRef allows FFmpeg to simply add a reference to the buffer
    // rather than having to perform a deep copy of the data buffers in avcodec_send_frame().
    av_frame->buf[0] = av_buffer_create((uint8_t *) pixel_buffer, 0, free_buffer, nullptr, 0);

    // Place a CVPixelBufferRef at data[3] as required by AV_PIX_FMT_VIDEOTOOLBOX
    av_frame->data[3] = (uint8_t *) pixel_buffer;

    return 0;
  }

  int nv12_zero_device::set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) {
    this->frame = frame;

    av_frame.reset(frame);

    resolution_fn(this->display, frame->width, frame->height);

    return 0;
  }

  void nv12_zero_device::apply_colorspace() {
    if (!convert_bgra) {
      return;
    }

    ypcbcr_info_ready = make_argb_to_ypcbcr_info(colorspace, ypcbcr_info) == kvImageNoError;
    if (!ypcbcr_info_ready) {
      BOOST_LOG(error) << "Unable to build BGRA to NV12 conversion for this colorspace"sv;
    }

    CVPixelBufferPoolRelease(nv12_pool);
    nv12_pool = nullptr;

    const int32_t format = colorspace.full_range ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    const int32_t width {frame->width};
    const int32_t height {frame->height};
    CFNumberRef format_num {CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &format)};
    CFNumberRef width_num {CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &width)};
    CFNumberRef height_num {CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &height)};
    // An empty IOSurface properties dictionary makes the buffers IOSurface-backed, so the
    // hardware encoder can read them without a copy.
    CFDictionaryRef iosurface {CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)};

    const void *keys[] {kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferIOSurfacePropertiesKey};
    const void *values[] {format_num, width_num, height_num, iosurface};
    CFDictionaryRef attributes {CFDictionaryCreate(kCFAllocatorDefault, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)};

    if (CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, attributes, &nv12_pool) != kCVReturnSuccess) {
      BOOST_LOG(error) << "Unable to create NV12 pixel buffer pool"sv;
      nv12_pool = nullptr;
    }

    CFRelease(attributes);
    CFRelease(iosurface);
    CFRelease(height_num);
    CFRelease(width_num);
    CFRelease(format_num);
  }

  int nv12_zero_device::init(void *display, pix_fmt_e pix_fmt, resolution_fn_t resolution_fn, const pixel_format_fn_t &pixel_format_fn) {
    convert_bgra = pix_fmt == pix_fmt_e::nv12;
    pixel_format_fn(display, convert_bgra ? kCVPixelFormatType_32BGRA : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange);

    this->display = display;
    this->resolution_fn = std::move(resolution_fn);

    // we never use this pointer, but its existence is checked/used
    // by the platform independent code
    data = this;

    return 0;
  }

}  // namespace platf
