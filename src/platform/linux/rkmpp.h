/**
 * @file src/platform/linux/rkmpp.h
 * @brief Declarations for RKMPP hardware accelerated capture.
 */
#pragma once

// local includes
#include "misc.h"
#include "src/platform/common.h"

#include <im2d.hpp>

namespace rkmpp {
  struct img_bufferinfo_t {
    uint32_t index;
    int dmafd;
    uint32_t sizeimage;
    rga_buffer_t rga_buffer;
  };

  struct img_t: public platf::img_t {
    ~img_t() override {
      requeue();
      close(v4lfd);
    }

    void requeue();

    void notify_unused() override {
      requeue();
    }

    int v4lfd;
    std::optional<img_bufferinfo_t> bufferinfo;
  };

  /**
   * Width --> Width of the image
   * Height --> Height of the image
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(int width, int height);
}  // namespace rkmpp
