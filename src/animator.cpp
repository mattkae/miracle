/**
Copyright (C) 2024  Matthew Kosarek

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
**/

#include "animator.h"
#include "miracle_config.h"
#include <mir/scene/surface.h>
#include <mir/server_action_queue.h>
#include <chrono>
#define MIR_LOG_COMPONENT "animator"
#include <mir/log.h>
#define _USE_MATH_DEFINES
#include <cmath>

using namespace miracle;
using namespace std::chrono_literals;

AnimationHandle const miracle::none_animation_handle = 0;

Animation::Animation(
    AnimationHandle handle,
    AnimationDefinition const& definition,
    std::function<void(AnimationStepResult const&)> const& callback)
    : handle{handle},
      definition{definition},
      callback{callback}
{
}

Animation& Animation::operator=(miracle::Animation const& other)
{
    handle = other.handle;
    definition = other.definition;
    from = other.from;
    to = other.to;
    callback = other.callback;
    return *this;
}

Animation Animation::window_move(
    AnimationHandle handle,
    AnimationDefinition const& definition,
    mir::geometry::Rectangle const& _from,
    mir::geometry::Rectangle const& _to,
    std::function<void(AnimationStepResult const&)> const& callback)
{
    Animation result(handle, definition, callback);
    result.from = _from;
    result.to = _to;
    return result;
}

namespace
{
float ease_out_bounce(AnimationDefinition const& defintion, float x)
{
    if (x < 1 / defintion.d1) {
        return defintion.n1 * x * x;
    } else if (x < 2 / defintion.d1) {
        return defintion.n1 * (x -= 1.5f / defintion.d1) * x + 0.75f;
    } else if (x < 2.5 / defintion.d1) {
        return defintion.n1 * (x -= 2.25f / defintion.d1) * x + 0.9375f;
    } else {
        return defintion.n1 * (x -= 2.625f / defintion.d1) * x + 0.984375f;
    }
}


inline float ease(AnimationDefinition const& defintion, float t)
{
    // https://easings.net/
    switch (defintion.function)
    {
    case EaseFunction::linear:
        return t;
    case EaseFunction::ease_in_sine:
        return 1 - cosf((t * M_PIf) / 2.f);
    case EaseFunction::ease_in_out_sine:
        return -(cosf(M_PIf * t) - 1) / 2;
    case EaseFunction::ease_out_sine:
        return sinf((t * M_PIf) / 2.f);
    case EaseFunction::ease_in_quad:
        return t * t;
    case EaseFunction::ease_out_quad:
        return 1 - (1 - t) * (1 - t);
    case EaseFunction::ease_in_out_quad:
        return t < 0.5 ? 2 * t * t : 1 - powf(-2 * t + 2, 2) / 2;
    case EaseFunction::ease_in_cubic:
        return t * t * t;
    case EaseFunction::ease_out_cubic:
        return 1 - powf(1 - t, 3);
    case EaseFunction::ease_in_out_cubic:
        return t < 0.5 ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2;
    case EaseFunction::ease_in_quart:
        return t * t * t * t;
    case EaseFunction::ease_out_quart:
        return 1 - powf(1 - t, 4);
    case EaseFunction::ease_in_out_quart:
        return t < 0.5 ? 8 * t * t * t * t : 1 - powf(-2 * t + 2, 4) / 2;
    case EaseFunction::ease_in_quint:
        return t * t * t * t * t;
    case EaseFunction::ease_out_quint:
        return 1 - powf(1 - t, 5);
    case EaseFunction::ease_in_out_quint:
        return t < 0.5 ? 16 * t * t * t * t * t : 1 - powf(-2 * t + 2, 5) / 2;
    case EaseFunction::ease_in_expo:
        return t == 0 ? 0 : powf(2, 10 * t - 10);
    case EaseFunction::ease_out_expo:
        return t == 1 ? 1 : 1 - powf(2, -10 * t);
    case EaseFunction::ease_in_out_expo:
        return t == 0
            ? 0
            : t == 1
                ? 1
                : t < 0.5 ? powf(2, 20 * t - 10) / 2
                          : (2 - powf(2, -20 * t + 10)) / 2;
    case EaseFunction::ease_in_circ:
        return 1 - sqrtf(1 - powf(t, 2));
    case EaseFunction::ease_out_circ:
        return sqrtf(1 - powf(t - 1, 2));
    case EaseFunction::ease_in_out_circ:
        return t < 0.5f
            ? (1 - sqrtf(1 - powf(2 * t, 2))) / 2
            : (sqrtf(1 - powf(-2 * t + 2, 2)) + 1) / 2;
    case EaseFunction::ease_in_back:
        return defintion.c3 * t * t * t - defintion.c1 * t * t;
    case EaseFunction::ease_out_back:
    {
        return 1 + defintion.c3 * powf(t - 1, 3) + defintion.c1 * powf(t - 1, 2);
    }
    case EaseFunction::ease_in_out_back:
        return t < 0.5
            ? (powf(2 * t, 2) * ((defintion.c2 + 1) * 2 * t - defintion.c2)) / 2
            : (powf(2 * t - 2, 2) * ((defintion.c2 + 1) * (t * 2 - 2) + defintion.c2) + 2) / 2;
    case EaseFunction::ease_in_elastic:
        return t == 0
            ? 0
            : t == 1
                ? 1
                : -powf(2, 10 * t - 10) * sinf((t * 10 - 10.75f) * defintion.c4);
    case EaseFunction::ease_out_elastic:
        return t == 0
            ? 0
            : t == 1
                ? 1
                : powf(2, -10 * t) * sinf((t * 10 - 0.75f) * defintion.c4) + 1;
    case EaseFunction::ease_in_out_elastic:
        return t == 0
            ? 0
            : t == 1
                ? 1
                : t < 0.5
                    ? -(powf(2, 20 * t - 10) * sinf((20 * t - 11.125f) * defintion.c5)) / 2
                    : (powf(2, -20 * t + 10) * sinf((20 * t - 11.125f) * defintion.c5)) / 2 + 1;
    case EaseFunction::ease_in_bounce:
        return 1 - ease_out_bounce(defintion, 1 - t);
    case EaseFunction::ease_out_bounce:
        return ease_out_bounce(defintion, t);
    case EaseFunction::ease_in_out_bounce:
        return t < 0.5
            ? (1 - ease_out_bounce(defintion, 1 - 2 * t)) / 2
            : (1 + ease_out_bounce(defintion, 2 * t - 1)) / 2;
    default:
        return 1.f;
    }
}
}

AnimationStepResult Animation::step()
{
    runtime_seconds += timestep_seconds;
    if (runtime_seconds >= definition.duration_seconds)
    {
        return {
            handle,
            true,
            glm::vec2(to.top_left.x.as_int(), to.top_left.y.as_int()),
            glm::vec2(to.size.width.as_int(), to.size.height.as_int()),
            glm::mat4(1.f),
        };
    }

    float t = (runtime_seconds / definition.duration_seconds);
    switch (definition.type)
    {
    case AnimationType::slide:
    {
        auto p = ease(definition, t);
        auto distance = to.top_left - from.top_left;
        float x = (float)distance.dx.as_int() * p;
        float y = (float)distance.dy.as_int() * p;

        glm::vec2 position = {
            (float)from.top_left.x.as_int() + x,
            (float)from.top_left.y.as_int() + y
        };

        return {
            handle,
            false,
            position,
            glm::vec2(to.size.width.as_int(), to.size.height.as_int()),
            glm::mat4(1.f)
        };
    }
    case AnimationType::grow:
    {
        auto p = ease(definition, t);
        glm::mat4 transform(
            p, 0, 0, 0,
            0, p, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1);
        return { handle, false, {}, {}, transform };
    }
    case AnimationType::shrink:
    {
        auto p = 1.f - ease(definition, t);
        glm::mat4 transform(
            p, 0, 0, 0,
            0, p, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1);
        return { handle, false, {}, {}, transform };
    }
    default:
        return {
            handle,
            false,
            glm::vec2(to.top_left.x.as_int(), to.top_left.y.as_int()),
            glm::vec2(to.size.width.as_int(), to.size.height.as_int()),
            glm::mat4(1.f)
        };
    }
}

Animator::Animator(
    std::shared_ptr<mir::ServerActionQueue> const& server_action_queue,
    std::shared_ptr<MiracleConfig> const& config)
    : server_action_queue{server_action_queue},
      config{config},
      run_thread([&]() { run(); })
{
}

Animator::~Animator()
{
    stop();
};

AnimationHandle Animator::window_move(
    AnimationHandle previous,
    mir::geometry::Rectangle const& from,
    mir::geometry::Rectangle const& to,
    std::function<void(AnimationStepResult const&)> const& callback)
{
    std::lock_guard<std::mutex> lock(processing_lock);
    for (auto it = queued_animations.begin(); it != queued_animations.end(); it++)
    {
        if (it->get_handle() == previous)
        {
            queued_animations.erase(it);
            break;
        }
    }

    // If animations aren't enabled, let's give them the position that
    // they want to go to immediately and don't bother animating anything.
    auto handle = next_handle++;
    if (!config->are_animations_enabled())
    {
        callback(
            { handle,
            true,
            glm::vec2(to.top_left.x.as_int(), to.top_left.y.as_int()),
            glm::vec2(to.size.width.as_int(), to.size.height.as_int()),
            glm::mat4(1.f) });
        return handle;
    }

    queued_animations.push_back(Animation::window_move(
        handle,
        config->get_animation_definitions()[(int)AnimateableEvent::window_move],
        from,
        to,
        callback));
    cv.notify_one();
    return handle;
}

AnimationHandle Animator::window_open(
    AnimationHandle previous,
    std::function<void(AnimationStepResult const&)> const& callback)
{
    std::lock_guard<std::mutex> lock(processing_lock);
    for (auto it = queued_animations.begin(); it != queued_animations.end(); it++)
    {
        if (it->get_handle() == previous)
        {
            queued_animations.erase(it);
            break;
        }
    }

    // If animations aren't enabled, let's give them the position that
    // they want to go to immediately and don't bother animating anything.
    auto handle = next_handle++;
    if (!config->are_animations_enabled())
    {
        callback({ handle, true});
        return handle;
    }

    queued_animations.push_back({
        handle,
        config->get_animation_definitions()[(int)AnimateableEvent::window_open],
        callback});
    cv.notify_one();
    return handle;
}

namespace
{
struct PendingUpdateData
{
    AnimationStepResult result;
    std::function<void(miracle::AnimationStepResult const&)> callback;
};
}

void Animator::run()
{
    // https://gist.github.com/mariobadr/673bbd5545242fcf9482
    using clock = std::chrono::high_resolution_clock;
    constexpr std::chrono::nanoseconds timestep(16ms);
    std::chrono::nanoseconds lag(0ns);
    auto time_start = clock::now();
    running = true;

    while (running)
    {
        {
            std::unique_lock lock(processing_lock);
            if (queued_animations.empty())
            {
                cv.wait(lock);
                time_start = clock::now();
            }
        }

        auto delta_time = clock::now() - time_start;
        time_start = clock::now();
        lag += std::chrono::duration_cast<std::chrono::nanoseconds>(delta_time);

        while(lag >= timestep) {
            lag -= timestep;

            std::vector<PendingUpdateData> update_data;
            {
                std::lock_guard<std::mutex> lock(processing_lock);
                for (auto it = queued_animations.begin(); it != queued_animations.end();)
                {
                    auto& item = *it;
                    auto result = item.step();

                    update_data.push_back({ result, item.get_callback() });
                    if (result.is_complete)
                        it = queued_animations.erase(it);
                    else
                        it++;
                }
            }

            server_action_queue->enqueue(this, [&, update_data]() {
                if (!running)
                    return;

                for (auto const& update_item : update_data)
                    update_item.callback(update_item.result);
            });
        }
    }
}

void Animator::stop()
{
    if (!running)
        return;

    running = false;
    run_thread.join();
}