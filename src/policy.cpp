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

#define MIR_LOG_COMPONENT "miracle"

#include "policy.h"
#include "config.h"
#include "container_group_container.h"
#include "feature_flags.h"
#include "floating_tree_container.h"
#include "parent_container.h"
#include "shell_component_container.h"
#include "window_helpers.h"
#include "window_tools_accessor.h"
#include "workspace_manager.h"

#include <iostream>
#include <mir/geometry/rectangle.h>
#include <mir/log.h>
#include <mir/server.h>
#include <mir_toolkit/events/enums.h>
#include <miral/application_info.h>
#include <miral/runner.h>
#include <miral/toolkit_event.h>
#include <miral/window_specification.h>
#include <miral/zone.h>

using namespace miracle;

namespace
{
const int MODIFIER_MASK = mir_input_event_modifier_alt | mir_input_event_modifier_shift | mir_input_event_modifier_sym | mir_input_event_modifier_ctrl | mir_input_event_modifier_meta;
}

class Policy::Self : public WorkspaceObserver
{
public:
    explicit Self(Policy& policy) :
        policy { policy }
    {
    }

    void on_created(uint32_t) override { }
    void on_removed(uint32_t) override { }
    void on_focused(std::optional<uint32_t> old, uint32_t next) override
    {
        if (old)
        {
            auto const& last_workspace = policy.workspace_manager.workspace(old.value());
            if (!last_workspace)
            {
                mir::log_error("Policy::Self::on_focused missing last workspace");
                return;
            }

            auto const& next_workspace = policy.workspace_manager.workspace(next);
            if (!next_workspace)
            {
                mir::log_error("Policy::Self::on_focused missing next workspace");
                return;
            }

            if (last_workspace->get_output() != next_workspace->get_output())
                policy.move_cursor_to_output(*next_workspace->get_output());
        }
    }

    Policy& policy;
};

Policy::Policy(
    miral::WindowManagerTools const& tools,
    AutoRestartingLauncher& external_client_launcher,
    miral::MirRunner& runner,
    std::shared_ptr<Config> const& config,
    SurfaceTracker& surface_tracker,
    mir::Server const& server,
    CompositorState& compositor_state,
    std::shared_ptr<WindowToolsAccessor> const& window_tools_accessor) :
    window_manager_tools { tools },
    state { compositor_state },
    floating_window_manager(std::make_shared<MinimalWindowManager>(tools, config)),
    external_client_launcher { external_client_launcher },
    runner { runner },
    config { config },
    workspace_manager { WorkspaceManager(
        tools,
        workspace_observer_registrar,
        config,
        [this]()
{ return get_active_output(); },
        [this]()
{ return get_output_list(); }) },
    animator(server.the_main_loop(), config),
    window_controller(tools, animator, state),
    i3_command_executor(*this, workspace_manager, compositor_state, external_client_launcher, window_controller),
    surface_tracker { surface_tracker },
    ipc { std::make_shared<Ipc>(runner, workspace_manager, *this, server.the_main_loop(), i3_command_executor, config) },
    scratchpad_(window_controller, state),
    self { std::make_shared<Self>(*this) }
{
    animator.start();
    workspace_observer_registrar.register_interest(ipc);
    workspace_observer_registrar.register_interest(self);
    mode_observer_registrar.register_interest(ipc);
    window_tools_accessor->set_tools(tools);
}

Policy::~Policy()
{
    workspace_observer_registrar.unregister_interest(ipc.get());
    workspace_observer_registrar.unregister_interest(self.get());
    mode_observer_registrar.unregister_interest(ipc.get());
}

bool Policy::handle_keyboard_event(MirKeyboardEvent const* event)
{
    auto const action = miral::toolkit::mir_keyboard_event_action(event);
    auto const scan_code = miral::toolkit::mir_keyboard_event_scan_code(event);
    auto const modifiers = miral::toolkit::mir_keyboard_event_modifiers(event) & MODIFIER_MASK;
    state.modifiers = modifiers;

    auto custom_key_command = config->matches_custom_key_command(action, scan_code, modifiers);
    if (custom_key_command != nullptr)
    {
        external_client_launcher.launch({ custom_key_command->command });
        return true;
    }

    return config->matches_key_command(action, scan_code, modifiers, [&](DefaultKeyCommand key_command)
    {
        if (key_command == DefaultKeyCommand::MAX)
            return false;

        switch (key_command)
        {
        case DefaultKeyCommand::Terminal:
        {
            auto terminal_command = config->get_terminal_command();
            if (terminal_command)
                external_client_launcher.launch({ terminal_command.value() });
            return true;
        }
        case DefaultKeyCommand::RequestVertical:
            return try_request_vertical();
        case DefaultKeyCommand::RequestHorizontal:
            return try_request_horizontal();
        case DefaultKeyCommand::ToggleResize:
            try_toggle_resize_mode();
            return true;
        case DefaultKeyCommand::ResizeUp:
            return try_resize(Direction::up);
        case DefaultKeyCommand::ResizeDown:
            return try_resize(Direction::down);
        case DefaultKeyCommand::ResizeLeft:
            return try_resize(Direction::left);
        case DefaultKeyCommand::ResizeRight:
            return try_resize(Direction::right);
        case DefaultKeyCommand::MoveUp:
            return try_move(Direction::up);
        case DefaultKeyCommand::MoveDown:
            return try_move(Direction::down);
        case DefaultKeyCommand::MoveLeft:
            return try_move(Direction::left);
        case DefaultKeyCommand::MoveRight:
            return try_move(Direction::right);
        case DefaultKeyCommand::SelectUp:
            return try_select(Direction::up);
        case DefaultKeyCommand::SelectDown:
            return try_select(Direction::down);
        case DefaultKeyCommand::SelectLeft:
            return try_select(Direction::left);
        case DefaultKeyCommand::SelectRight:
            return try_select(Direction::right);
        case DefaultKeyCommand::QuitActiveWindow:
            return try_close_window();
        case DefaultKeyCommand::QuitCompositor:
            return quit();
        case DefaultKeyCommand::Fullscreen:
            return try_toggle_fullscreen();
        case DefaultKeyCommand::SelectWorkspace1:
            return select_workspace(1);
        case DefaultKeyCommand::SelectWorkspace2:
            return select_workspace(2);
        case DefaultKeyCommand::SelectWorkspace3:
            return select_workspace(3);
        case DefaultKeyCommand::SelectWorkspace4:
            return select_workspace(4);
        case DefaultKeyCommand::SelectWorkspace5:
            return select_workspace(5);
        case DefaultKeyCommand::SelectWorkspace6:
            return select_workspace(6);
        case DefaultKeyCommand::SelectWorkspace7:
            return select_workspace(7);
        case DefaultKeyCommand::SelectWorkspace8:
            return select_workspace(8);
        case DefaultKeyCommand::SelectWorkspace9:
            return select_workspace(9);
        case DefaultKeyCommand::SelectWorkspace0:
            return select_workspace(0);
        case DefaultKeyCommand::MoveToWorkspace1:
            return move_active_to_workspace(1);
        case DefaultKeyCommand::MoveToWorkspace2:
            return move_active_to_workspace(2);
        case DefaultKeyCommand::MoveToWorkspace3:
            return move_active_to_workspace(3);
        case DefaultKeyCommand::MoveToWorkspace4:
            return move_active_to_workspace(4);
        case DefaultKeyCommand::MoveToWorkspace5:
            return move_active_to_workspace(5);
        case DefaultKeyCommand::MoveToWorkspace6:
            return move_active_to_workspace(6);
        case DefaultKeyCommand::MoveToWorkspace7:
            return move_active_to_workspace(7);
        case DefaultKeyCommand::MoveToWorkspace8:
            return move_active_to_workspace(8);
        case DefaultKeyCommand::MoveToWorkspace9:
            return move_active_to_workspace(9);
        case DefaultKeyCommand::MoveToWorkspace0:
            return move_active_to_workspace(0);
        case DefaultKeyCommand::ToggleFloating:
            return toggle_floating();
        case DefaultKeyCommand::TogglePinnedToWorkspace:
            return toggle_pinned_to_workspace();
        case DefaultKeyCommand::ToggleTabbing:
            return toggle_tabbing();
        case DefaultKeyCommand::ToggleStacking:
            return toggle_stacking();
        default:
            std::cerr << "Unknown key_command: " << static_cast<int>(key_command) << std::endl;
            break;
        }
        return false;
    });
}

bool Policy::handle_pointer_event(MirPointerEvent const* event)
{
    auto x = miral::toolkit::mir_pointer_event_axis_value(event, MirPointerAxis::mir_pointer_axis_x);
    auto y = miral::toolkit::mir_pointer_event_axis_value(event, MirPointerAxis::mir_pointer_axis_y);
    auto action = miral::toolkit::mir_pointer_event_action(event);
    state.cursor_position = { x, y };

    // Select the output first
    for (auto const& output : output_list)
    {
        if (output->point_is_in_output(static_cast<int>(x), static_cast<int>(y)))
        {
            if (state.active_output != output)
            {
                if (state.active_output)
                    state.active_output->set_is_active(false);
                state.active_output = output;
                state.active_output->set_is_active(true);
                if (auto active = output->active())
                    workspace_manager.request_focus(active->id());
            }
            break;
        }
    }

    if (state.active_output && state.mode != WindowManagerMode::resizing)
    {
        if (MIRACLE_FEATURE_FLAG_MULTI_SELECT && action == mir_pointer_action_button_down)
        {
            if (state.modifiers == config->get_primary_modifier())
            {
                // We clicked while holding the modifier, so we're probably in the middle of a multi-selection.
                if (state.mode != WindowManagerMode::selecting)
                {
                    state.mode = WindowManagerMode::selecting;
                    group_selection = std::make_shared<ContainerGroupContainer>(state);
                    state.add(group_selection);
                    mode_observer_registrar.advise_changed(state.mode);
                }
            }
            else if (state.mode == WindowManagerMode::selecting)
            {
                // We clicked while we were in selection mode, so let's stop being in selection mode
                // TODO: Would it be better to check what we clicked in case it's in the group? Then we wouldn't
                //  exit selection mode in this case.
                state.mode = WindowManagerMode::normal;
                mode_observer_registrar.advise_changed(state.mode);
            }
        }

        // Get Container intersection. Depending on the state, do something with that Container
        std::shared_ptr<Container> intersected = state.active_output->intersect(event);
        switch (state.mode)
        {
        case WindowManagerMode::normal:
        {
            if (intersected)
            {
                if (auto window = intersected->window().value())
                {
                    if (state.active() != intersected)
                        window_controller.select_active_window(window);
                }
            }

            if (state.has_clicked_floating_window || (state.active() && state.active()->get_type() == ContainerType::floating_window))
            {
                if (action == mir_pointer_action_button_down)
                    state.has_clicked_floating_window = true;
                else if (action == mir_pointer_action_button_up)
                    state.has_clicked_floating_window = false;
                return floating_window_manager->handle_pointer_event(event);
            }

            return false;
        }
        case WindowManagerMode::selecting:
        {
            if (intersected && action == mir_pointer_action_button_down)
                group_selection->add(intersected);
            return true;
        }
        default:
            return false;
        }
    }

    return false;
}

auto Policy::place_new_window(
    const miral::ApplicationInfo& app_info,
    const miral::WindowSpecification& requested_specification) -> miral::WindowSpecification
{
    if (!state.active_output)
    {
        mir::log_warning("place_new_window: no output available");
        return requested_specification;
    }

    auto new_spec = requested_specification;
    pending_allocation = state.active_output->allocate_position(app_info, new_spec, {});
    return new_spec;
}

void Policy::advise_new_window(miral::WindowInfo const& window_info)
{
    if (!state.active_output)
    {
        mir::log_warning("create_container: output unavailable");
        auto window = window_info.window();
        if (!output_list.empty())
        {
            // Our output is gone! Let's try to add it to a different output
            output_list.front()->add_immediately(window);
        }
        else
        {
            // We have no output! Let's add it to a list of orphans. Such
            // windows are considered to be in the "other" category until
            // we have more data on them.
            orphaned_window_list.push_back(window);
            surface_tracker.add(window);
        }

        return;
    }

    auto container = state.active_output->create_container(window_info, pending_allocation);
    container->animation_handle(animator.register_animateable());
    container->on_open();
    state.add(container);

    pending_allocation.container_type = ContainerType::none;
    surface_tracker.add(window_info.window());
}

void Policy::handle_window_ready(miral::WindowInfo& window_info)
{
    auto container = window_controller.get_container(window_info.window());
    if (!container)
    {
        mir::log_error("handle_window_ready: container is not provided");
        return;
    }

    container->handle_ready();
}

mir::geometry::Rectangle
Policy::confirm_placement_on_display(
    miral::WindowInfo const& window_info,
    MirWindowState new_state,
    mir::geometry::Rectangle const& new_placement)
{
    auto container = window_controller.get_container(window_info.window());
    if (!container)
    {
        mir::log_warning("confirm_placement_on_display: window lacks container");
        return new_placement;
    }

    return container->confirm_placement(new_state, new_placement);
}

void Policy::advise_focus_gained(const miral::WindowInfo& window_info)
{
    auto container = window_controller.get_container(window_info.window());
    if (!container)
    {
        mir::log_error("advise_focus_gained: container is not provided");
        return;
    }

    switch (state.mode)
    {
    case WindowManagerMode::selecting:
        group_selection->add(container);
        container->on_focus_gained();
        break;
    default:
    {
        auto* workspace = container->get_workspace();
        state.focus(container);
        container->on_focus_gained();
        if (workspace && workspace != state.active_output->active())
            workspace_manager.request_focus(workspace->id());

        if (workspace)
            workspace->advise_focus_gained(container);
        break;
    }
    }
}

void Policy::advise_focus_lost(const miral::WindowInfo& window_info)
{
    auto container = window_controller.get_container(window_info.window());
    if (!container)
    {
        mir::log_error("advise_focus_lost: container is not provided");
        return;
    }

    state.unfocus(container);
    container->on_focus_lost();
}

void Policy::advise_delete_window(const miral::WindowInfo& window_info)
{
    for (auto it = orphaned_window_list.begin(); it != orphaned_window_list.end(); it++)
    {
        if (*it == window_info.window())
        {
            orphaned_window_list.erase(it);
            surface_tracker.remove(window_info.window());
            return;
        }
    }

    auto container = window_controller.get_container(window_info.window());
    if (!container)
    {
        mir::log_error("delete_container: container is not provided");
        return;
    }

    if (auto output = container->get_output())
        output->delete_container(container);
    else
        scratchpad_.remove(container);

    surface_tracker.remove(window_info.window());
    state.unfocus(container);
}

void Policy::advise_move_to(miral::WindowInfo const& window_info, geom::Point top_left)
{
    auto container = window_controller.get_container(window_info.window());
    if (!container)
    {
        mir::log_error("advise_move_to: container is not provided: %s", window_info.application_id().c_str());
        return;
    }

    container->on_move_to(top_left);
}

void Policy::advise_output_create(miral::Output const& output)
{
    auto output_content = std::make_shared<Output>(
        output, workspace_manager, output.extents(), window_manager_tools,
        floating_window_manager, state, config, window_controller, animator);
    output_list.push_back(output_content);
    workspace_manager.request_first_available_workspace(output_content.get());
    if (state.active_output == nullptr)
    {
        state.active_output = output_content;
        state.active_output->set_is_active(true);
    }

    // Let's rehome some orphan windows if we need to
    if (!orphaned_window_list.empty())
    {
        mir::log_info("Policy::advise_output_create: orphaned windows are being added to the new output, num=%zu", orphaned_window_list.size());
        for (auto& window : orphaned_window_list)
        {
            state.active_output->add_immediately(window);
        }
        orphaned_window_list.clear();
    }
}

void Policy::advise_output_update(miral::Output const& updated, miral::Output const& original)
{
    for (auto& output : output_list)
    {
        if (output->get_output().is_same_output(original))
        {
            output->update_area(updated.extents());
            break;
        }
    }
}

void Policy::advise_output_delete(miral::Output const& output)
{
    for (auto it = output_list.begin(); it != output_list.end(); it++)
    {
        auto other_output = *it;
        if (other_output->get_output().is_same_output(output))
        {
            auto const remove_workspaces = [&]()
            {
                // WARNING: We copy the workspace numbers first because we shouldn't delete while iterating
                std::vector<uint32_t> workspaces;
                workspaces.reserve(other_output->get_workspaces().size());
                for (auto const& workspace : other_output->get_workspaces())
                    workspaces.push_back(workspace->id());

                for (auto const w : workspaces)
                    workspace_manager.delete_workspace(w);
            };

            output_list.erase(it);
            if (output_list.empty())
            {
                // All nodes should become orphaned
                for (auto& window : other_output->collect_all_windows())
                {
                    orphaned_window_list.push_back(window);
                    window_controller.set_user_data(window, std::make_shared<ShellComponentContainer>(window, window_controller));
                }

                remove_workspaces();

                mir::log_info("Policy::advise_output_delete: final output has been removed and windows have been orphaned");
                state.active_output = nullptr;
            }
            else
            {
                state.active_output = output_list.front();
                state.active_output->set_is_active(true);
                for (auto& window : other_output->collect_all_windows())
                {
                    state.active_output->add_immediately(window);
                }

                remove_workspaces();
            }
            break;
        }
    }
}

void Policy::handle_modify_window(
    miral::WindowInfo& window_info,
    const miral::WindowSpecification& modifications)
{
    auto container = window_controller.get_container(window_info.window());
    if (!container)
    {
        mir::log_error("handle_modify_window: container is not provided");
        return;
    }

    auto const* workspace = container->get_workspace();
    if (workspace)
    {
        if (workspace != state.active_output->active())
            return;
    }
    else if (scratchpad_.contains(container) && !scratchpad_.is_showing(container))
        return;

    container->handle_modify(modifications);
}

void Policy::handle_raise_window(miral::WindowInfo& window_info)
{
    auto container = window_controller.get_container(window_info.window());
    if (!container)
    {
        mir::log_error("handle_raise_window: container is not provided");
        return;
    }

    container->handle_raise();
}

bool Policy::handle_touch_event(const MirTouchEvent* event)
{
    return false;
}

void Policy::handle_request_move(miral::WindowInfo& window_info, const MirInputEvent* input_event)
{
    auto container = window_controller.get_container(window_info.window());
    if (!container)
    {
        mir::log_error("handle_request_move: window lacks container");
        return;
    }

    container->handle_request_move(input_event);
}

void Policy::handle_request_resize(
    miral::WindowInfo& window_info,
    const MirInputEvent* input_event,
    MirResizeEdge edge)
{
    auto container = window_controller.get_container(window_info.window());
    if (!container)
    {
        mir::log_error("handle_request_resize: window lacks container");
        return;
    }

    container->handle_request_resize(input_event, edge);
}

mir::geometry::Rectangle Policy::confirm_inherited_move(
    const miral::WindowInfo& window_info,
    mir::geometry::Displacement movement)
{
    return { window_info.window().top_left() + movement, window_info.window().size() };
}

void Policy::advise_application_zone_create(miral::Zone const& application_zone)
{
    for (auto const& output : output_list)
    {
        output->advise_application_zone_create(application_zone);
    }
}

void Policy::advise_application_zone_update(miral::Zone const& updated, miral::Zone const& original)
{
    for (auto const& output : output_list)
    {
        output->advise_application_zone_update(updated, original);
    }
}

void Policy::advise_application_zone_delete(miral::Zone const& application_zone)
{
    for (auto const& output : output_list)
    {
        output->advise_application_zone_delete(application_zone);
    }
}

void Policy::advise_end()
{
    if (is_starting_)
    {
        is_starting_ = false;
        for (auto const& app : config->get_startup_apps())
        {
            external_client_launcher.launch(app);
        }
    }
}

void Policy::try_toggle_resize_mode()
{
    if (!state.active())
    {
        state.mode = WindowManagerMode::normal;
        return;
    }

    if (state.active()->get_type() != ContainerType::leaf)
    {
        state.mode = WindowManagerMode::normal;
        return;
    }

    if (state.mode == WindowManagerMode::resizing)
        state.mode = WindowManagerMode::normal;
    else
        state.mode = WindowManagerMode::resizing;

    mode_observer_registrar.advise_changed(state.mode);
}

bool Policy::try_request_vertical()
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    if (!state.active())
        return false;

    state.active()->request_vertical_layout();
    return true;
}

bool Policy::try_toggle_layout(bool cycle_thru_all)
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    if (!state.active())
        return false;

    state.active()->toggle_layout(cycle_thru_all);
    return true;
}

bool Policy::try_request_horizontal()
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    if (!state.active())
        return false;

    state.active()->request_horizontal_layout();
    return true;
}

bool Policy::try_resize(miracle::Direction direction)
{
    if (state.mode != WindowManagerMode::resizing)
        return false;

    if (!state.active())
        return false;

    return state.active()->resize(direction);
}

bool Policy::try_move(miracle::Direction direction)
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    if (!state.active())
        return false;

    return state.active()->move(direction);
}

bool Policy::try_move_by(miracle::Direction direction, int pixels)
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    if (!state.active())
        return false;

    return state.active()->move_by(direction, pixels);
}

bool Policy::try_move_to(int x, int y)
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    if (!state.active())
        return false;

    return state.active()->move_to(x, y);
}

bool Policy::try_select(miracle::Direction direction)
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    if (!state.active())
        return false;

    return state.active()->select_next(direction);
}

bool Policy::try_select_parent()
{
    if (state.mode != WindowManagerMode::normal)
        return false;

    if (!state.active())
        return false;

    if (!state.active()->get_parent().expired())
    {
        state.focus(state.active()->get_parent().lock());
        return true;
    }
    else
    {
        mir::log_error("try_select_parent: no parent to select");
        return false;
    }
}

bool Policy::try_select_floating()
{
    if (state.mode != WindowManagerMode::normal)
        return false;

    if (auto to_select = state.get_first_with_type(ContainerType::floating_window))
    {
        if (auto const& window = to_select->window())
        {
            window_controller.select_active_window(window.value());
            return true;
        }
    }

    return false;
}

bool Policy::try_select_tiling()
{
    if (state.mode != WindowManagerMode::normal)
        return false;

    if (auto to_select = state.get_first_with_type(ContainerType::leaf))
    {
        if (auto const& window = to_select->window())
        {
            window_controller.select_active_window(window.value());
            return true;
        }
    }

    return false;
}

bool Policy::try_select_toggle()
{
    if (state.mode != WindowManagerMode::normal)
        return false;

    if (auto const active = state.active())
    {
        if (active->get_type() == ContainerType::leaf)
            return try_select_floating();
        else if (active->get_type() == ContainerType::floating_window)
            return try_select_tiling();
    }

    return false;
}

bool Policy::try_close_window()
{
    if (!state.active())
        return false;

    auto window = state.active()->window();
    if (!window)
        return false;

    window_controller.close(window.value());
    return true;
}

bool Policy::quit()
{
    ipc->on_shutdown();
    runner.stop();
    return true;
}

bool Policy::try_toggle_fullscreen()
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    if (!state.active())
        return false;

    return state.active()->toggle_fullscreen();
}

bool Policy::select_workspace(int number, bool back_and_forth)
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    if (!state.active_output)
        return false;

    workspace_manager.request_workspace(state.active_output.get(), number, back_and_forth);
    return true;
}

bool Policy::select_workspace(std::string const& name, bool back_and_forth)
{
    // TODO: Handle back_and_forth
    if (state.mode == WindowManagerMode::resizing)
        return false;

    return workspace_manager.request_workspace(state.active_output.get(), name, back_and_forth);
}

bool Policy::next_workspace()
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    workspace_manager.request_next(state.active_output);
    return true;
}

bool Policy::prev_workspace()
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    workspace_manager.request_prev(state.active_output);
    return true;
}

bool Policy::back_and_forth_workspace()
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    workspace_manager.request_back_and_forth();
    return true;
}

bool Policy::next_workspace_on_output(miracle::Output const& output)
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    return workspace_manager.request_next_on_output(output);
}

bool Policy::prev_workspace_on_output(miracle::Output const& output)
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    return workspace_manager.request_prev_on_output(output);
}

bool Policy::move_active_to_workspace(int number, bool back_and_forth)
{
    if (!can_move_container())
        return false;

    auto container = state.active();
    container->get_output()->delete_container(container);
    state.unfocus(container);

    if (workspace_manager.request_workspace(
            state.active_output.get(), number, back_and_forth))
    {
        state.active_output->graft(container);
        if (container->window().value())
            window_controller.select_active_window(container->window().value());
        return true;
    }

    return false;
}

bool Policy::move_active_to_workspace_named(std::string const& name, bool back_and_forth)
{
    if (!can_move_container())
        return false;

    auto container = state.active();
    container->get_output()->delete_container(container);
    state.unfocus(nullptr);

    if (workspace_manager.request_workspace(state.active_output.get(), name, back_and_forth))
    {
        state.active_output->graft(container);
        return true;
    }

    return false;
}

bool Policy::move_active_to_next()
{
    if (!can_move_container())
        return false;

    auto container = state.active();
    container->get_output()->delete_container(container);
    state.unfocus(container);

    if (workspace_manager.request_next(state.active_output))
    {
        state.active_output->graft(container);
        return true;
    }

    return false;
}

bool Policy::move_active_to_prev()
{
    if (!can_move_container())
        return false;

    auto container = state.active();
    container->get_output()->delete_container(container);
    state.unfocus(container);

    if (workspace_manager.request_prev(state.active_output))
    {
        state.active_output->graft(container);
        return true;
    }

    return false;
}

bool Policy::move_active_to_back_and_forth()
{
    if (!can_move_container())
        return false;

    auto container = state.active();
    container->get_output()->delete_container(container);
    state.unfocus(container);

    if (workspace_manager.request_back_and_forth())
    {
        state.active_output->graft(container);
        return true;
    }

    return false;
}

bool Policy::move_to_scratchpad()
{
    if (!can_move_container())
        return false;

    // Only floating or tiled windows can be moved to the scratchpad
    auto container = state.active();
    if (container->get_type() != ContainerType::floating_window
        && container->get_type() != ContainerType::leaf)
    {
        mir::log_error("move_to_scratchpad: cannot move window to scratchpad: %d", static_cast<int>(container->get_type()));
        return false;
    }

    // If the window isn't floating already, we should make it floating
    if (container->get_type() != ContainerType::floating_window)
    {
        if (!state.active_output)
            return false;

        container = toggle_floating_internal(container);
    }

    // Remove it from its current workspace since it is no longer wanted there
    if (auto workspace = container->get_workspace())
        workspace->remove_floating_hack(container);

    return scratchpad_.move_to(container);
}

bool Policy::show_scratchpad()
{
    // TODO: Only show the window that meets the criteria
    return scratchpad_.toggle_show_all();
}

bool Policy::can_move_container() const
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    if (!state.active())
        return false;

    if (state.active()->is_fullscreen())
        return false;

    return true;
}

std::shared_ptr<Container> Policy::toggle_floating_internal(std::shared_ptr<Container> const& container)
{
    auto const handle_ready = [&](
                                  miral::Window const& window,
                                  AllocationHint const& result)
    {
        auto& info = window_controller.info_for(window);
        auto new_container = state.active_output->create_container(info, result);
        new_container->handle_ready();
        state.add(new_container);
        window_controller.select_active_window(state.active()->window().value());
        return new_container;
    };

    switch (container->get_type())
    {
    case ContainerType::leaf:
    {
        auto window = container->window();
        if (!window)
            return nullptr;

        // First, remove the container
        container->get_output()->delete_container(window_controller.get_container(*window));

        // Next, place the new container
        auto& prev_info = window_controller.info_for(*window);
        auto spec = window_helpers::copy_from(prev_info);
        spec.top_left() = geom::Point { window->top_left().x.as_int() + 20, window->top_left().y.as_int() + 20 };
        window_controller.noclip(*window);
        auto result = state.active_output->allocate_position(window_manager_tools.info_for(window->application()), spec, { ContainerType::floating_window });
        window_controller.modify(*window, spec);

        state.remove(container);

        // Finally, declare it ready
        return handle_ready(*window, result);
    }
    case ContainerType::floating_window:
    {
        auto window = container->window();
        if (!window)
            return nullptr;

        // First, remove the container
        if (scratchpad_.contains(container))
            scratchpad_.remove(container);
        else
            container->get_output()->delete_container(window_controller.get_container(*window));

        // Next, place the container
        auto& prev_info = window_controller.info_for(*window);
        miral::WindowSpecification spec = window_helpers::copy_from(prev_info);
        auto result = state.active_output->allocate_position(window_manager_tools.info_for(window->application()), spec, { ContainerType::leaf });
        window_controller.modify(*window, spec);

        state.remove(container);

        // Finally, declare it ready
        return handle_ready(*window, result);
    }
    default:
        mir::log_warning("toggle_floating: has no effect on window of type: %d", (int)container->get_type());
        return nullptr;
    }
}

bool Policy::toggle_floating()
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    if (!state.active())
        return false;

    toggle_floating_internal(state.active());
    return true;
}

bool Policy::toggle_pinned_to_workspace()
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    if (!state.active())
        return false;

    return state.active()->pinned(!state.active()->pinned());
}

bool Policy::set_is_pinned(bool pinned)
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    if (!state.active())
        return false;

    return state.active()->pinned(pinned);
}

bool Policy::toggle_tabbing()
{
    if (!can_set_layout())
        return false;

    return state.active()->toggle_tabbing();
}

bool Policy::toggle_stacking()
{
    if (!can_set_layout())
        return false;

    return state.active()->toggle_stacking();
}

bool Policy::set_layout(LayoutScheme scheme)
{
    if (!can_set_layout())
        return false;

    return state.active()->set_layout(scheme);
}

bool Policy::set_layout_default()
{
    if (!can_set_layout())
        return false;

    return state.active()->set_layout(config->get_default_layout_scheme());
}

void Policy::move_cursor_to_output(Output const& output)
{
    auto const& extents = output.get_output().extents();
    window_manager_tools.move_cursor_to({ extents.top_left.x.as_int() + extents.size.width.as_int() / 2.f,
        extents.top_left.y.as_int() + extents.size.height.as_int() / 2.f });
}

bool Policy::try_select_next_output()
{
    for (size_t i = 0; i < output_list.size(); i++)
    {
        if (output_list[i] == state.active_output)
        {
            size_t j = i + 1;
            if (j == output_list.size())
                j = 0;

            move_cursor_to_output(*output_list[j]);
            return true;
        }
    }

    return false;
}

bool Policy::try_select_prev_output()
{
    for (int i = output_list.size() - 1; i >= 0; i++)
    {
        if (output_list[i] == state.active_output)
        {
            size_t j = i - 1;
            if (j < 0)
                j = output_list.size() - 1;

            move_cursor_to_output(*output_list[j]);
            return true;
        }
    }

    return false;
}

bool Policy::try_select_output(Direction direction)
{
    return false;
}

bool Policy::try_select_output(std::vector<std::string> const& names)
{
    if (!state.active_output)
        return false;

    auto current_name = state.active_output->get_output().name();
    size_t next = 0;
    for (size_t i = 0; i < names.size(); i++)
    {
        if (names[i] == current_name)
        {
            next = i + 1;
            break;
        }
    }

    if (next == names.size())
        next = 0;

    for (auto const& output : output_list)
    {
        if (output->get_output().name() == names[next])
        {
            move_cursor_to_output(*output);
            return true;
        }
    }

    return false;
}

bool Policy::can_set_layout() const
{
    if (state.mode == WindowManagerMode::resizing)
        return false;

    if (!state.active())
        return false;

    return true;
}
