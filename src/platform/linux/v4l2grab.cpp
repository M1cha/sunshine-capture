/**
 * @file src/platform/linux/v4l2grab.cpp
 * @brief Definitions for v4l2 capture.
 */
// standard includes
#include <thread>

// local includes
#include "rkmpp.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"
#include "v4l2grab.h"

#include <im2d.hpp>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>

extern "C" {
#include <rockchip/mpp_log.h>
}

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

using namespace std::literals;

namespace v4l2 {
  struct v4l2grab_buffer_t {
    uint32_t dmafd;
    uint32_t sizeimage;
    rga_buffer_handle_t rga_buffer_handle;
    rga_buffer_t rga_buffer;
  };

  std::chrono::steady_clock::time_point timeval_to_timepoint(const timeval &tv) {
    using namespace std::chrono;
    return steady_clock::time_point(seconds(tv.tv_sec) + microseconds(tv.tv_usec));
  }

  uint32_t fourcc_rk_from_v4l2(uint32_t v4l2) {
    switch (v4l2) {
      case V4L2_PIX_FMT_BGR24:
        return RK_FORMAT_BGR_888;
      case V4L2_PIX_FMT_NV12:
        return RK_FORMAT_YCbCr_420_SP;
      default:
        return RK_FORMAT_UNKNOWN;
    }
  }

  void log_v4l_fmt(v4l2_format &fmt, std::string prefix) {
    BOOST_LOG(info)
      << "G_FMT(" << prefix << "): width="
      << fmt.fmt.pix_mp.width
      << " height="
      << fmt.fmt.pix_mp.height
      << " 4cc="
      << (char) (fmt.fmt.pix_mp.pixelformat & 0xff)
      << (char) ((fmt.fmt.pix_mp.pixelformat >> 8) & 0xff)
      << (char) ((fmt.fmt.pix_mp.pixelformat >> 16) & 0xff)
      << (char) ((fmt.fmt.pix_mp.pixelformat >> 24) & 0xff);
  }

  class v4l2_t: public platf::display_t {
  public:
    ~v4l2_t() override {
      if (dmaheapfd > 0) {
        close(dmaheapfd);
      }
      if (v4lfd > 0) {
        close(v4lfd);
      }

      delete_buffers();
    }

    void delete_buffers() {
      for (auto buffer : buffers) {
        releasebuffer_handle(buffer.rga_buffer_handle);
        close(buffer.dmafd);
      }
      buffers.clear();
    }

    int configure_v4l2() {
      v4l2_capability caps = {};
      int ret = ioctl(v4lfd, VIDIOC_QUERYCAP, &caps);
      if (ret != 0) {
        BOOST_LOG(error) << "Failed to query capabilities"sv;
        return ret;
      }
      if (!(caps.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
        BOOST_LOG(error) << "Device doesn't support mplane"sv;
        return ret;
      }

      v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
      };
      ret = ioctl(v4lfd, VIDIOC_G_FMT, &fmt);
      if (ret != 0) {
        BOOST_LOG(error) << "Failed to get video format"sv;
        return ret;
      }
      log_v4l_fmt(fmt, "start");

      ret = ioctl(v4lfd, VIDIOC_S_FMT, &fmt);
      if (ret != 0) {
        BOOST_LOG(error) << "Failed to set video format"sv;
        return ret;
      }

      ret = ioctl(v4lfd, VIDIOC_G_FMT, &fmt);
      if (ret != 0) {
        BOOST_LOG(error) << "Failed to get video format"sv;
        return ret;
      }
      log_v4l_fmt(fmt, "final");

      if (fmt.fmt.pix_mp.num_planes != 1) {
        BOOST_LOG(error) << "Unsupported number of planes: " << fmt.fmt.pix_mp.num_planes;
        return ret;
      }

      uint32_t rkformat = fourcc_rk_from_v4l2(fmt.fmt.pix_mp.pixelformat);
      if (rkformat == RK_FORMAT_UNKNOWN) {
        BOOST_LOG(error) << "Unsupported V4L2 pixel format";
        return ret;
      }

      width = fmt.fmt.pix_mp.width;
      height = fmt.fmt.pix_mp.height;

      this->env_width = width;
      this->env_height = height;

      v4l2_requestbuffers req = {
        .count = 3,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_DMABUF,
      };
      ret = ioctl(v4lfd, VIDIOC_REQBUFS, &req);
      if (ret != 0) {
        BOOST_LOG(error) << "Failed to request buffers"sv;
        return ret;
      }
      // It's okay if we receive less than we requested, but less than 2 would
      // give us bad performance.
      if (req.count < 2) {
        BOOST_LOG(error) << "V4L allocated " << req.count << " buffers only"sv;
        return -ENOBUFS;
      }

      for (uint32_t i = 0; i < req.count; i += 1) {
        uint32_t sizeimage = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

        dma_heap_allocation_data alloc_data = {
          .len = sizeimage,
          .fd_flags = O_RDWR,
        };
        ret = ioctl(dmaheapfd, DMA_HEAP_IOCTL_ALLOC, &alloc_data);
        if (ret != 0) {
          BOOST_LOG(error) << "Failed to allocate dma buffer"sv;
          return ret;
        }

        rga_buffer_handle_t rga_buffer_handle = importbuffer_fd(alloc_data.fd, sizeimage);
        if (rga_buffer_handle == 0) {
          BOOST_LOG(error) << "Failed to import source buffer"sv;
          return -1;
        }
        rga_buffer_t rga_buffer = wrapbuffer_handle(rga_buffer_handle, width, height, rkformat);

        buffers.push_back(v4l2grab_buffer_t {
          .dmafd = alloc_data.fd,
          .sizeimage = sizeimage,
          .rga_buffer_handle = rga_buffer_handle,
          .rga_buffer = rga_buffer,
        });
      }

      for (uint32_t i = 0; i < buffers.size(); i += 1) {
        v4l2_plane planes[] = {{
          .length = buffers[i].sizeimage,
          .m = {
            .fd = (int) buffers[i].dmafd,
          },
        }};
        v4l2_buffer buf = {
          .index = i,
          .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
          .memory = V4L2_MEMORY_DMABUF,
          .m = {
            .planes = planes,
          },
          .length = ARRAY_SIZE(planes),
        };

        ret = ioctl(v4lfd, VIDIOC_QBUF, &buf);
        if (ret != 0) {
          BOOST_LOG(error) << "Failed to queue buffer"sv;
          return ret;
        }
      }

      enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      ret = ioctl(v4lfd, VIDIOC_STREAMON, &type);
      if (ret != 0) {
        BOOST_LOG(error) << "Failed to enable streaming"sv;
        return ret;
      }

      return 0;
    }

    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      delay = std::chrono::nanoseconds {1s} / config.framerate;
      mem_type = hwdevice_type;

      BOOST_LOG(info) << "Selected monitor ["sv << display_name << "] for streaming"sv;

      std::string devName = "/dev/video" + display_name;

      v4lfd = open(devName.c_str(), O_RDWR | O_CLOEXEC);
      if (v4lfd < 0) {
        BOOST_LOG(error) << "Failed to open v4l2 device"sv;
        return -errno;
      }

      dmaheapfd = open("/dev/dma_heap/cma-uncached", O_RDWR);
      if (dmaheapfd < 0) {
        BOOST_LOG(error) << "Failed to open dma_heap device"sv;
        close(v4lfd);
        v4lfd = -1;
        return -errno;
      }

      int rc = configure_v4l2();
      if (rc) {
        close(dmaheapfd);
        dmaheapfd = -1;

        close(v4lfd);
        v4lfd = -1;

        delete_buffers();
        return rc;
      }

      return 0;
    }

    int dummy_img(platf::img_t *img) override {
      // TODO
      return 0;
    }

    platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      sleep_overshoot_logger.reset();

      // NOTE: Other implementations do a sleep here, but for v4l this leads to
      //       high latency streaming for unknown reasons. Since we can use the
      //       V4L API in a blocking way, that sleep is not necessary though.
      // NOTE: We don't implement a timeout right now. This might cause issues
      //       depending on the expectations of video.cpp, but should reduce
      //       latency by not needing to poll before receiving the frame.

      while (true) {
        std::shared_ptr<platf::img_t> img_out;
        auto status = snapshot(pull_free_image_cb, img_out);
        switch (status) {
          case platf::capture_e::reinit:
          case platf::capture_e::error:
          case platf::capture_e::interrupted:
            return status;
          case platf::capture_e::timeout:
            if (!push_captured_image_cb(std::move(img_out), false)) {
              return platf::capture_e::ok;
            }
            break;
          case platf::capture_e::ok:
            if (!push_captured_image_cb(std::move(img_out), true)) {
              return platf::capture_e::ok;
            }
            break;
          default:
            BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
            return status;
        }
      }

      return platf::capture_e::ok;
    }

    platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out) {
      if (!pull_free_image_cb(img_out)) {
        return platf::capture_e::interrupted;
      }

      v4l2_plane planes[1];
      v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
        .m = {
          .planes = planes,
        },
        .length = ARRAY_SIZE(planes),
      };
      int ret = ioctl(v4lfd, VIDIOC_DQBUF, &buf);
      if (ret != 0) {
        BOOST_LOG(error) << "Failed to dequeue buffer"sv;
        return platf::capture_e::error;
      }

      std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
      if (buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
        frame_timestamp = timeval_to_timepoint(buf.timestamp);
      }

      auto img = (rkmpp::img_t *) img_out.get();
      img->bufferinfo = rkmpp::img_bufferinfo_t {
        .index = buf.index,
        .dmafd = planes[0].m.fd,
        .sizeimage = planes[0].length,
        .rga_buffer = buffers[buf.index].rga_buffer,
      };
      img->wants_unused_notify = true;
      img->frame_timestamp = frame_timestamp;

      return platf::capture_e::ok;
    }

    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
      if (mem_type == platf::mem_type_e::rkmpp) {
        return rkmpp::make_avcodec_encode_device(width, height);
      }

      return std::make_unique<platf::avcodec_encode_device_t>();
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      if (v4lfd < 0) {
        BOOST_LOG(warning) << "tried to alloc before init";
        return nullptr;
      }

      auto img = std::make_shared<rkmpp::img_t>();

      img->width = width;
      img->height = height;
      img->data = nullptr;
      img->bufferinfo = std::nullopt;
      img->v4lfd = dup(v4lfd);

      return img;
    }

    platf::mem_type_e mem_type;
    std::chrono::nanoseconds delay;

    int v4lfd = -1;
    int dmaheapfd = -1;
    std::vector<v4l2grab_buffer_t> buffers = {};
  };
}  // namespace v4l2

namespace platf {
  std::shared_ptr<display_t> v4l2_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (hwdevice_type != platf::mem_type_e::rkmpp) {
      BOOST_LOG(error) << "Could not initialize display with the given hw device type."sv;
      return nullptr;
    }

    auto v4l2 = std::make_shared<v4l2::v4l2_t>();
    if (v4l2->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return v4l2;
  }

  std::vector<std::string> v4l2_display_names() {
    std::vector<std::string> display_names;

    BOOST_LOG(info) << "-------- Start of V4L2 monitor list --------"sv;

    for (size_t x = 0; x < 64; ++x) {
      std::string devName = "/dev/video" + std::to_string(x);
      v4l2_capability cap;

      int fd = open(devName.c_str(), O_RDWR | O_CLOEXEC);
      if (fd < 0) {
        if (errno == ENOENT) {
          break;
        }

        continue;
      }

      if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        close(fd);
        continue;
      }

      if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
        close(fd);
        continue;
      }

      BOOST_LOG(info) << "Monitor " << x << " is "sv << std::string(reinterpret_cast<char *>(cap.card));

      display_names.emplace_back(std::to_string(x));
      close(fd);
    }

    BOOST_LOG(info) << "--------- End of V4L2 monitor list ---------"sv;

    return display_names;
  }

}  // namespace platf
