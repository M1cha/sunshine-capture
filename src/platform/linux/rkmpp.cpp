/**
 * @file src/platform/linux/rkmpp.cpp
 * @brief Definitions for RKMPP hardware accelerated capture.
 */
// standard includes
#include <fcntl.h>
#include <format>
#include <sstream>
#include <string>

extern "C" {
#include <libavutil/hwcontext_rkmpp.h>
#include <libavutil/macros.h>
}

// platform includes
#include <drm_fourcc.h>
#include <im2d.hpp>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <RgaUtils.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// local includes
#include "misc.h"
#include "rkmpp.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"
#include "src/video.h"

using namespace std::literals;

extern "C" struct AVBufferRef;

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

struct AVFrame;

namespace rkmpp {
  void free_frame(AVFrame *frame) {
    av_frame_free(&frame);
  }

  using frame_t = util::safe_ptr<AVFrame, free_frame>;

  void img_t::requeue() {
    int ret;

    if (!bufferinfo.has_value()) {
      return;
    }
    if (v4lfd < 0) {
      BOOST_LOG(warning) << "can't requeue buffer due to missing v4lfd"sv;
      return;
    }

    auto bufferinfo_value = bufferinfo.value();

    v4l2_plane planes[] = {{
      .length = bufferinfo_value.sizeimage,
      .m = {
        .fd = (int) bufferinfo_value.dmafd,
      },
    }};
    v4l2_buffer buf = {
      .index = bufferinfo_value.index,
      .type = bufferinfo_value.buf_type,
      .memory = V4L2_MEMORY_DMABUF,
    };

    switch (bufferinfo_value.buf_type) {
      case V4L2_BUF_TYPE_VIDEO_CAPTURE:
        buf.m.fd = bufferinfo_value.dmafd;
        buf.length = bufferinfo_value.sizeimage;
        break;
      case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
        buf.m.planes = planes;
        buf.length = ARRAY_SIZE(planes);
        break;
      default:
        BOOST_LOG(warning) << "can't requeue unsupported buffer type"sv;
        return;
    }

    ret = ioctl(v4lfd, VIDIOC_QBUF, &buf);
    if (ret != 0) {
      BOOST_LOG(error) << "Failed to queue buffer"sv;
      return;
    }

    bufferinfo = std::nullopt;
  }

  class rkmpp_t: public platf::avcodec_encode_device_t {
  public:
    ~rkmpp_t() override {
      if (rga_buffer_handle != 0) {
        releasebuffer_handle(rga_buffer_handle);
      }
    }

    int set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx_buf) override {
      if (rga_buffer_handle != 0) {
        releasebuffer_handle(rga_buffer_handle);
        rga_buffer_handle = 0;
      }

      this->hwframe.reset(frame);
      this->frame = frame;

      if (!frame->buf[0]) {
        if (av_hwframe_get_buffer(hw_frames_ctx_buf, frame, 0)) {
          BOOST_LOG(error) << "Couldn't get hwframe for RKMPP"sv;
          return -1;
        }
      }

      auto desc = (AVRKMPPDRMFrameDescriptor *) frame->data[0];

      if (desc->drm_desc.nb_objects != 1) {
        BOOST_LOG(error) << "Unsupported number of objects: "sv << desc->drm_desc.nb_objects;
        return -1;
      }
      if (desc->drm_desc.nb_layers != 1) {
        BOOST_LOG(error) << "Unsupported number of layers: "sv << desc->drm_desc.nb_layers;
        return -1;
      }
      auto object = &desc->drm_desc.objects[0];
      auto layer = &desc->drm_desc.layers[0];

      rga_buffer_handle = importbuffer_fd(desc->drm_desc.objects[0].fd, desc->drm_desc.objects[0].size);
      if (rga_buffer_handle == 0) {
        BOOST_LOG(error) << "Failed to import destination buffer"sv;
        return -1;
      }

      rga_buffer = wrapbuffer_handle(rga_buffer_handle, frame->width, frame->height, layer->format, object->format_modifier, frame->width, frame->height);

      return 0;
    }

    void apply_colorspace() override {
    }

    int convert(platf::img_t &platf_img) override {
      IM_STATUS imstatus;
      auto img = (img_t *) &platf_img;

      if (!img->bufferinfo.has_value()) {
        const im_rect rect = {
          .x = 0,
          .y = 0,
          .width = this->frame->width,
          .height = this->frame->height,
        };
        const im_color_t color = {
          .red = 0,
          .green = 0,
          .blue = 0,
          .alpha = 0,
        };

        imstatus = imcheck({}, rga_buffer, {}, rect, IM_COLOR_FILL);
        if (imstatus != IM_STATUS_NOERROR) {
          BOOST_LOG(error) << "imcheck failed for fill: "sv << imStrError(imstatus);
          return -1;
        }

        imstatus = imfill(rga_buffer, rect, color.value);
        if (imstatus != IM_STATUS_SUCCESS) {
          BOOST_LOG(error) << "imfill failed: "sv << imStrError(imstatus);
          return -1;
        }

        return 0;
      }
      auto img_bufferinfo = img->bufferinfo.value();

      imstatus = imcheck(img_bufferinfo.rga_buffer, rga_buffer, {}, {});
      if (imstatus != IM_STATUS_NOERROR) {
        BOOST_LOG(error) << "imcheck failed: "sv << imStrError(imstatus);
        return -1;
      }

      imstatus = improcess(img_bufferinfo.rga_buffer, rga_buffer, {}, {}, {}, {}, -1, NULL, NULL, IM_SYNC);
      if (imstatus != IM_STATUS_SUCCESS) {
        BOOST_LOG(error) << "improcess failed: "sv << imStrError(imstatus);
        return -1;
      }

      return 0;
    }

    int init(int in_width, int in_height) {
      this->data = (void *) 0x1;
      width = in_width;
      height = in_height;

      return 0;
    }

    int width, height;

    frame_t hwframe;

    rga_buffer_handle_t rga_buffer_handle = 0;
    rga_buffer_t rga_buffer;
  };

  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(int width, int height) {
    auto mpp = std::make_unique<rkmpp::rkmpp_t>();
    if (mpp->init(width, height)) {
      return nullptr;
    }

    return mpp;
  }
}  // namespace rkmpp
