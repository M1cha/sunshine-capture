/**
 * @file src/platform/linux/input/udcinput.h
 * @brief Declarations for udcinput input handling.
 */
#pragma once

// lib includes
#include <udcinput.hpp>

// local includes
#include "src/config.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf {
  using joypads_t = std::variant<udcinput::SwitchPro>;

  struct joypad_state {
    std::unique_ptr<joypads_t> joypad;
    std::unique_ptr<std::thread> thread;
    udcinput::Loop loop;
    gamepad_feedback_msg_t last_rumble;
    gamepad_feedback_msg_t last_rgb_led;
    thread_pool_util::ThreadPool::task_id_t repeat_task {};
  };

  struct input_raw_t {
    input_raw_t():
        gamepads(MAX_GAMEPADS) {
      if (gadget.init("/sys/kernel/config", "g1", true) < 0) {
        return;
      }

      if (gadget.configure_as_switchpro() < 0) {
        return;
      }
    }

    ~input_raw_t() {
      // Make sure their destructors are called before we destroy the gadget.
      // Additionlly, we have to send a stop to the loop, since the thread holds
      // a reference, so it won't be destructed by deleting our reference from
      // this array.

      for (auto gamepad : gamepads) {
        gamepad->loop.stop();
        gamepad->thread->join();
        gamepad->joypad.reset();
      }

      gamepads.clear();
    }

    udcinput::Gadget gadget;

    /**
     * A list of gamepads that are currently connected.
     * The pointer is shared because that state will be shared with background threads that deal with rumble and LED
     */
    std::vector<std::shared_ptr<joypad_state>> gamepads;
  };

  struct client_input_raw_t: public client_input_t {
    client_input_raw_t(input_t &input) {
      global = (input_raw_t *) input.get();
    }

    input_raw_t *global;
  };
}  // namespace platf
