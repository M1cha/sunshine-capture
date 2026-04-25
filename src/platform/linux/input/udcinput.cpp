/**
 * @file src/platform/linux/input/udcinput.cpp
 * @brief Definitions for the udcinput Linux input handling.
 */
// local includes
#include "udcinput.h"

#include "src/config.h"
#include "src/platform/common.h"
#include "src/utility.h"

#include <boost/algorithm/hex.hpp>
#include <boost/endian/conversion.hpp>
#include <cstdarg>
#include <cstdio>
#include <iostream>

extern "C" {
#include <udcinput/log.h>
}

using namespace std::literals;

extern "C" void udcinput_write_log(uint8_t level, const char *tag, const char *fmt, ...) {
  auto logger = debug;
  switch (level) {
    case UDCINPUT_LOG_LEVEL_ERR:
      logger = error;
      break;
    case UDCINPUT_LOG_LEVEL_WRN:
      logger = warning;
      break;
    case UDCINPUT_LOG_LEVEL_INF:
      logger = info;
      break;
    case UDCINPUT_LOG_LEVEL_DBG:
      logger = debug;
      break;
  }

  va_list ap;
  va_start(ap, fmt);

  va_list ap_copy;
  va_copy(ap_copy, ap);
  auto nbytes = vsnprintf(NULL, 0, fmt, ap_copy);
  va_end(ap_copy);

  std::vector<char> buffer(nbytes + 1);
  vsnprintf(buffer.data(), buffer.size(), fmt, ap);

  va_end(ap);

  std::stringstream tagstr;
  if (tag) {
    tagstr << tag << ": ";
  }

  BOOST_LOG(logger) << "udcinput: "sv << tagstr.str() << std::string(buffer.data(), nbytes);
}

namespace platf {
  input_t input() {
    return {new input_raw_t()};
  }

  void freeInput(void *p) {
    auto *input = (input_raw_t *) p;
    delete input;
  }

  std::unique_ptr<client_input_t> allocate_client_input_context(input_t &input) {
    return std::make_unique<client_input_raw_t>(input);
  }

  void move_mouse(input_t &input, int deltaX, int deltaY) {
  }

  void abs_mouse(input_t &input, const touch_port_t &touch_port, float x, float y) {
  }

  void button_mouse(input_t &input, int button, bool release) {
  }

  void scroll(input_t &input, int high_res_distance) {
  }

  void hscroll(input_t &input, int high_res_distance) {
  }

  void keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t flags) {
  }

  void unicode(input_t &input, char *utf8, int size) {
  }

  void touch_update(client_input_t *input, const touch_port_t &touch_port, const touch_input_t &touch) {
  }

  void pen_update(client_input_t *input, const touch_port_t &touch_port, const pen_input_t &pen) {
  }

  void gamepad_thread(const std::shared_ptr<joypad_state> &gamepad) {
    BOOST_LOG(debug) << "gamepad thread started";

    std::visit([gamepad](auto &&gc) {
      auto ret = gamepad->loop.run(&gc.raw, gc.loop_callbacks());
      if (ret < 0) {
        BOOST_LOG(error) << "gamepad loop failed";
      }
    },
               *gamepad->joypad);

    BOOST_LOG(debug) << "gamepad thread stopped";
  }

  int alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue) {
    auto raw = (input_raw_t *) input.get();

    BOOST_LOG(info) << "alloc gamepad: global="sv << id.globalIndex << " relative="sv << static_cast<unsigned int>(id.clientRelativeIndex);

    // Disable it so we can add a new one. Will fail if it's not enabled.
    raw->gadget.disable();

    auto fg = util::fail_guard([&raw]() {
      // Re-enable it in case it was enabled. This will fail if there are no
      // active gamepads.
      raw->gadget.enable(config::input.udc_name.c_str());
    });

    auto switchPro = udcinput::SwitchPro::create(raw->gadget);
    if (!switchPro) {
      BOOST_LOG(warning) << "Unable to create virtual Switch Pro controller";
      return -1;
    }

    auto loop = udcinput::Loop::create();
    if (!loop) {
      BOOST_LOG(warning) << "Unable to create loop";
      return -1;
    }

    auto gamepad = std::make_shared<joypad_state>(joypad_state {
      .loop = std::move(*loop),
    });

    auto on_rumble_fn = [feedback_queue, idx = id.clientRelativeIndex, gamepad](uint16_t low_freq, uint16_t high_freq) {
      // Don't resend duplicate rumble data
      if (gamepad->last_rumble.type == platf::gamepad_feedback_e::rumble && gamepad->last_rumble.data.rumble.lowfreq == low_freq && gamepad->last_rumble.data.rumble.highfreq == high_freq) {
        return;
      }

      gamepad_feedback_msg_t msg = gamepad_feedback_msg_t::make_rumble(idx, low_freq, high_freq);
      feedback_queue->raise(msg);
      gamepad->last_rumble = msg;
    };
    switchPro->set_callbacks(udcinput::GamepadCallbacks {
      .on_rumble = on_rumble_fn,
    });

    gamepad->joypad = std::make_unique<joypads_t>(std::move(*switchPro));
    gamepad->thread = std::make_unique<std::thread>(std::thread(gamepad_thread, gamepad));

    raw->gamepads[id.globalIndex] = std::move(gamepad);

    return 0;
  }

  void free_gamepad(input_t &input, int nr) {
    auto raw = (input_raw_t *) input.get();

    BOOST_LOG(info) << "free gamepad: nr="sv << nr;

    raw->gamepads[nr]->loop.stop();
    raw->gamepads[nr]->thread->join();
    raw->gamepads[nr]->joypad.reset();
    raw->gamepads[nr].reset();

    // Re-enable it in case we still have other gamepads, because deleting a
    // function will disable it implicitly.
    // This will fail if there are no active gamepads.
    raw->gadget.enable(config::input.udc_name.c_str());
  }

  void trysend_update(std::shared_ptr<joypad_state> gamepad, const udcinput_gamepad_state udcinput_state) {
    if (gamepad->repeat_task) {
      task_pool.cancel(gamepad->repeat_task);
      gamepad->repeat_task = nullptr;
    }

    bool retry = false;
    std::visit([&retry, gamepad, udcinput_state](auto &&gc) {
      retry = gc.set_state(udcinput_state);
    },
               *gamepad->joypad);

    if (retry) {
      // 8ms are a little bit less than half the time between USB polls with a 60Hz rate.
      gamepad->repeat_task = task_pool.pushDelayed(trysend_update, 8ms, gamepad, udcinput_state).task_id;
    }
  }

  void gamepad_update(input_t &input, int nr, const gamepad_state_t &gamepad_state) {
    auto raw = (input_raw_t *) input.get();
    auto gamepad = raw->gamepads[nr];
    if (!gamepad) {
      return;
    }

    udcinput_gamepad_state udcinput_state = {
      .buttons = gamepad_state.buttonFlags,
      .trigger_left = gamepad_state.lt,
      .trigger_right = gamepad_state.rt,
      .stick_left = {
        .x = gamepad_state.lsX,
        .y = gamepad_state.lsY,
      },
      .stick_right = {
        .x = gamepad_state.rsX,
        .y = gamepad_state.rsY,
      },
    };
    trysend_update(gamepad, udcinput_state);
  }

  void gamepad_touch(input_t &input, const gamepad_touch_t &touch) {
  }

  void gamepad_motion(input_t &input, const gamepad_motion_t &motion) {
  }

  void gamepad_battery(input_t &input, const gamepad_battery_t &battery) {
  }

  platform_caps::caps_t get_capabilities() {
    platform_caps::caps_t caps = 0;
    return caps;
  }

  util::point_t get_mouse_loc(input_t &input) {
    return {0, 0};
  }

  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input) {
    static std::vector gps {
      supported_gamepad_t {"switch", true, ""},
    };

    return gps;
  }
}  // namespace platf
