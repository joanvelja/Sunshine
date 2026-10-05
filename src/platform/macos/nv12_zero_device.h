/**
 * @file src/platform/macos/nv12_zero_device.h
 * @brief Declarations for NV12 zero copy device on macOS.
 */
#pragma once

// platform includes
#include <Accelerate/Accelerate.h>
#include <CoreVideo/CoreVideo.h>

// local includes
#include "src/platform/common.h"

struct AVFrame;

namespace platf {
  /**
   * @brief Release an FFmpeg frame allocated by the capture or conversion backend.
   *
   * @param frame Video or graphics frame being processed.
   */
  void free_frame(AVFrame *frame);

  /**
   * @brief Build the vImage RGB to Y'CbCr conversion for a Sunshine colorspace.
   *
   * The matrix is derived from the colorspace's luma coefficients (Kr, Kb) and the pixel
   * range from its full/limited flag, so 8-bit Rec. 601, Rec. 709 and BT.2020 SDR are covered.
   *
   * @param colorspace Colorspace negotiated with the client.
   * @param info Conversion info to fill.
   * @return vImage status; kvImageNoError on success.
   */
  vImage_Error make_argb_to_ypcbcr_info(const video::sunshine_colorspace_t &colorspace, vImage_ARGBToYpCbCr &info);

  /**
   * @brief Convert a BGRA pixel buffer into an 8-bit 4:2:0 bi-planar (NV12) pixel buffer.
   *
   * vImage averages each 2x2 block for chroma, unlike AVFoundation's and VideoToolbox's
   * own conversions, which keep only one sample per block.
   *
   * @param bgra Locked source buffer in kCVPixelFormatType_32BGRA.
   * @param nv12 Destination buffer in a 420YpCbCr8BiPlanar format, same dimensions.
   * @param info Conversion from make_argb_to_ypcbcr_info().
   * @return 0 on success; nonzero on dimension or format mismatch, or vImage failure.
   */
  int convert_bgra_to_nv12(CVPixelBufferRef bgra, CVPixelBufferRef nv12, const vImage_ARGBToYpCbCr &info);

  /**
   * @brief macOS encode device that forwards AVFoundation frames to FFmpeg's VideoToolbox encoder.
   *
   * 10-bit frames are captured as P010 and forwarded zero-copy. 8-bit frames are captured as BGRA
   * and converted to NV12 with chroma filtering (see convert_bgra_to_nv12()). Frames that don't
   * match the session's size or format (another concurrent session configured the shared capture)
   * are forwarded unchanged.
   */
  class nv12_zero_device: public avcodec_encode_device_t {
    // display holds a pointer to an av_video object. Since the namespaces of AVFoundation
    // and FFMPEG collide, we need this opaque pointer and cannot use the definition
    void *display;

  public:
    // this function is used to set the resolution on an av_video object that we cannot
    // call directly because of namespace collisions between AVFoundation and FFMPEG
    /**
     * @brief Callback signature used to update the opaque AVFoundation display resolution.
     */
    using resolution_fn_t = std::function<void(void *display, int width, int height)>;
    resolution_fn_t resolution_fn;  ///< Callback stored for later AVFoundation resolution updates.
    /**
     * @brief Callback signature used to update the opaque AVFoundation pixel format.
     */
    using pixel_format_fn_t = std::function<void(void *display, int pixelFormat)>;

    ~nv12_zero_device() override;

    /**
     * @brief Initialize NV12 encoding for an AVFoundation display.
     *
     * @param display Display object or identifier associated with the operation.
     * @param pix_fmt Sunshine pixel format to convert or allocate for.
     * @param resolution_fn Callback used to resize the AVFoundation capture output.
     * @param pixel_format_fn Pixel format.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(void *display, pix_fmt_e pix_fmt, resolution_fn_t resolution_fn, const pixel_format_fn_t &pixel_format_fn);

    /**
     * @brief Hand a captured AVFoundation frame to FFmpeg, converting BGRA to NV12 when needed.
     *
     * @param img Image or frame object to read from or populate.
     * @return Conversion status.
     */
    int convert(img_t &img) override;
    /**
     * @brief Attach frame resources used by the next conversion or encode operation.
     *
     * @param frame Video or graphics frame being processed.
     * @param hw_frames_ctx FFmpeg hardware frames context associated with the frame.
     * @return Status from updating frame.
     */
    int set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) override;

    /**
     * @brief Build the BGRA to NV12 conversion for the negotiated colorspace.
     */
    void apply_colorspace() override;

  private:
    util::safe_ptr<AVFrame, free_frame> av_frame;
    bool convert_bgra {false};  ///< Whether captured frames are BGRA that must be converted to NV12.
    CVPixelBufferPoolRef nv12_pool {};  ///< Destination buffers for converted frames, sized to the encoder frame.
    vImage_ARGBToYpCbCr ypcbcr_info {};  ///< Conversion matching the negotiated colorspace.
    bool ypcbcr_info_ready {false};  ///< Whether ypcbcr_info has been generated.
    bool warned_unconvertible {false};  ///< Whether the unconvertible-frame warning was logged.
  };

}  // namespace platf
