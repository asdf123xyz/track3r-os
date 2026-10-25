// SPDX-License-Identifier: GPL-3.0-or-later
// Job actions (OS-002c).
#include "action_handlers.h"

namespace o3os::app {

// Cancels every running job that can be (an import that has half changed the project cannot). The progress strip then says so,
// from the pass that sees the slot Cancelled (App::report_jobs).
bool job_cancel(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.session == nullptr) {
    return false;
  }
  const unsigned before = ctx.session->jobs.running();
  ctx.session->jobs.cancel_all();
  return ctx.session->jobs.running() < before;
}

}  // namespace o3os::app
