#pragma once

#include <boost/capy/concept/executor.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cstddef>
#include <exception>
#include <stop_token>
#include <utility>

namespace nghttp2_corosio::detail
{

// =================================================================================================
//
// An execution_context::service that tracks every coroutine spawned "into" it via spawn(), and
// lets an owner cancel and wait for all of them to actually finish -- not merely ask them to. This
// is what makes it safe to destroy the owning io_context afterward: nothing is left suspended for
// the scheduler to tear down mid-flight (destroying an io_context while a coroutine frame is still
// suspended corrupts memory -- capy's stop_token/stop_callback machinery ends up double-freed; see
// the "Known issue" this replaces, formerly documented in CLAUDE.md).
//
// Looked up via `ex.context().use_service<TaskGroup>()`, so any code holding an executor bound to
// this context can join in: a Server's accept loop, its accepted sessions, their per-request
// handlers, and any Client session connected on the same context all share one instance (services
// are keyed by type per execution_context, see capy's execution_context.hpp) -- draining it waits
// for everything, not just what one particular owner spawned.
//
// =================================================================================================

class TaskGroup : public boost::capy::execution_context::service
{
public:
   explicit TaskGroup(boost::capy::execution_context&) noexcept {}

   std::stop_token get_token() noexcept { return source_.get_token(); }

   /// Spawns `task`, detached, tracked so drain() can wait for it. `ex` must be bound to this
   /// service's owning execution_context.
   template <boost::capy::Executor Ex>
   void spawn(Ex ex, boost::capy::task<> task)
   {
      ++count_;
      boost::capy::run_async(std::move(ex), get_token(), [this] { --count_; },
                             [this](std::exception_ptr) { --count_; })(std::move(task));
   }

   /// Number of tracked coroutines that haven't completed yet.
   std::size_t count() const noexcept { return count_; }

   /// Requests cancellation of everything tracked. Does not block -- required by
   /// execution_context::service::shutdown() -- so this alone does not make it safe to destroy
   /// the context; the owner must still drive it (poll()/run_one()) until count() reaches 0.
   void request_stop() { source_.request_stop(); }

protected:
   void shutdown() override { request_stop(); }

private:
   std::stop_source source_;
   std::size_t count_ = 0;
};

} // namespace nghttp2_corosio::detail
