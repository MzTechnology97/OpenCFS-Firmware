// SPDX-License-Identifier: Apache-2.0
#include "board_app.hpp"

namespace cfs::board {

BoardApp* BoardApp::active_ = nullptr;

BoardApp::BoardApp(uint8_t address, const BoardVariant& variant)
    : variant_(variant), hw_(board_.hardware()), app_(hw_, address) {
    // Before init(): the very first peripheral set-up already sees the gates.
    board_.configure(variant_.motion_locked, variant_.settings_read_only);
    app_.set_motion_locked(variant_.motion_locked);
}

void BoardApp::init() {
    board_.init();
    active_ = this;
    board_.clock.idle_hook = &BoardApp::idle;
    app_.init();
    board_ms_ = board_.clock.now_ms();
    next_env_ms_ = board_ms_;
}

void BoardApp::service_board() {
    if (in_service_) return;
    in_service_ = true;
    const uint32_t now = board_.clock.now_ms();
    while (board_ms_ != now) {  // catch up if a pass took longer than 1 ms
        ++board_ms_;
        board_.tick_1ms(board_ms_);
    }
    in_service_ = false;
}

void BoardApp::idle() {
    if (!active_) return;
    active_->service_board();
    active_->app_.background();
}

void BoardApp::loop_once() {
    service_board();
    app_.loop_once();
    service_board();
    if (service_) service_->service(board_.clock.now_ms());
    const uint32_t now = board_.clock.now_ms();
    if (static_cast<int32_t>(now - next_env_ms_) >= 0) {
        next_env_ms_ = now + kEnvironmentMs;
        board_.update_environment(app_.box.box_index);
    }
}

}  // namespace cfs::board
