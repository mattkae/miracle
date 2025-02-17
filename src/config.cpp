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

#define MIR_LOG_COMPONENT "config"

#include "config.h"
#include "yaml-cpp/node/node.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glib-2.0/glib.h>
#include <glm/fwd.hpp>
#include <libevdev-1.0/libevdev/libevdev.h>
#include <mir/log.h>
#include <mir/options/option.h>
#include <mir/server.h>
#include <miral/runner.h>
#include <sstream>
#include <sys/inotify.h>

using namespace miracle;

namespace
{
const char* MIRACLE_DEFAULT_CONFIG_DIR = "/usr/share/miracle-wm/default-config";

int program_exists(std::string const& name)
{
    std::stringstream out;
    out << "command -v " << name << " > /dev/null 2>&1";
    return !system(out.str().c_str());
}

std::string create_default_configuration_path()
{
    std::stringstream config_path_stream;
    config_path_stream << g_get_user_config_dir();
    config_path_stream << "/miracle-wm.yaml";
    return config_path_stream.str();
}

std::optional<MirKeyboardAction> from_string_keyboard_action(std::string const& action)
{
    if (action == "up")
        return MirKeyboardAction::mir_keyboard_action_up;
    else if (action == "down")
        return MirKeyboardAction::mir_keyboard_action_down;
    else if (action == "repeat")
        return MirKeyboardAction::mir_keyboard_action_repeat;
    else if (action == "modifiers")
        return MirKeyboardAction::mir_keyboard_action_modifiers;
    else
        return std::nullopt;
}

}

uint Config::process_modifier(uint modifier) const
{
    if (modifier & miracle_input_event_modifier_default)
        modifier = modifier & ~miracle_input_event_modifier_default | get_input_event_modifier();
    return modifier;
}

FilesystemConfiguration::FilesystemConfiguration(miral::MirRunner& runner) :
    FilesystemConfiguration { runner, create_default_configuration_path() }
{
}

FilesystemConfiguration::FilesystemConfiguration(
    miral::MirRunner& runner, std::string const& path, bool load_immediately) :
    runner { runner },
    default_config_path { path }
{
    if (load_immediately)
    {
        mir::log_info("FilesystemConfiguration: File is being loaded immediately on construction. "
                      "It is assumed that you are running this inside of a test");
        config_path = default_config_path;
        _init(std::nullopt, std::nullopt);
    }
}

void FilesystemConfiguration::load(mir::Server& server)
{
    const char* config_file_name_option = "config";
    server.add_configuration_option(
        config_file_name_option,
        "File path to the miracle-wm yaml configuration file",
        default_config_path);

    const char* no_config_option = "no-config";
    server.add_configuration_option(
        no_config_option,
        "If specified, the configuration file will not be loaded",
        false);

    const char* exec_option = "exec";
    server.add_configuration_option(
        exec_option,
        "Specifies an application that will run when miracle starts. When this application "
        "dies, miracle will also die.",
        "");

    const char* systemd_session_configure_option = "systemd-session-configure";
    server.add_configuration_option(
        systemd_session_configure_option,
        "If specified, this script will setup the systemd session before any apps are run",
        "");

    server.add_init_callback([this, config_file_name_option, no_config_option, exec_option, systemd_session_configure_option, &server]
    {
        auto const server_opts = server.get_options();
        no_config = server_opts->get<bool>(no_config_option);
        config_path = server_opts->get<std::string>(config_file_name_option);
        std::optional<StartupApp> systemd_app = std::nullopt;
        std::optional<StartupApp> exec_app = std::nullopt;

        auto systemd_session_configure = server_opts->get<std::string>(systemd_session_configure_option);
        if (!systemd_session_configure.empty())
            systemd_app = StartupApp { .command = systemd_session_configure };

        if (server_opts->is_set(exec_option))
        {
            auto command = server_opts->get<std::string>(exec_option);
            if (!command.empty())
            {
                exec_app = StartupApp {
                    .command = command,
                    .should_halt_compositor_on_death = true
                };
            }
        }

        _init(systemd_app, exec_app);
    });
}

void FilesystemConfiguration::_init(std::optional<StartupApp> const& systemd_app, std::optional<StartupApp> const& exec_app)
{
    if (no_config)
    {
        mir::log_info("No configuration option was set, so the file will not be created");
    }
    else
    {
        mir::log_info("Configuration file path is: %s", config_path.c_str());
        if (!std::filesystem::exists(config_path))
        {

            if (!std::filesystem::exists(std::filesystem::path(config_path).parent_path()))
            {
                mir::log_info("Configuration directory path missing, creating it now");
                std::filesystem::create_directories(std::filesystem::path(config_path).parent_path());
            }
            if (std::filesystem::exists(MIRACLE_DEFAULT_CONFIG_DIR))
            {
                mir::log_info("Configuration hierarchy being copied from %s", MIRACLE_DEFAULT_CONFIG_DIR);
                const auto fs_copyopts = std::filesystem::copy_options::recursive;
                std::filesystem::copy(MIRACLE_DEFAULT_CONFIG_DIR, std::filesystem::path(config_path).parent_path(), fs_copyopts);
            }
            else
            {
                mir::log_info("Configuration being written blank");
                std::fstream file(config_path, std::ios::out | std::ios::in | std::ios::app);
            }
        }
    }

    reload();

    // If the user specified an --systemd-session-configure <APP_NAME>, let's add that to the list
    if (systemd_app)
    {
        options.startup_apps.insert(options.startup_apps.begin(), systemd_app.value());
    }

    // If the user specified an --exec <APP_NAME>, let's add that to the list
    if (exec_app)
    {
        mir::log_info("Miracle will die when the application specified with --exec dies");
        options.startup_apps.push_back(exec_app.value());
    }

    is_loaded_ = true;
    _watch(runner);
}

void FilesystemConfiguration::reload()
{
    std::lock_guard<std::mutex> lock(mutex);

    // Reset all
    options = ConfigDetails();

    if (no_config)
    {
        mir::log_info("No configuration was specified, so the config will not load.");
        return;
    }

    // Load the new configuration
    mir::log_info("Configuration is loading...");
    YAML::Node config = YAML::LoadFile(config_path);
    if (config["action_key"])
        read_action_key(config["action_key"]);
    if (config["default_action_overrides"])
        read_default_action_overrides(config["default_action_overrides"]);
    if (config["custom_actions"])
        read_custom_actions(config["custom_actions"]);
    if (config["inner_gaps"])
        read_inner_gaps(config["inner_gaps"]);
    if (config["outer_gaps"])
        read_outer_gaps(config["outer_gaps"]);
    if (config["startup_apps"])
        read_startup_apps(config["startup_apps"]);
    if (config["terminal"])
        read_terminal(config["terminal"]);
    if (config["resize_jump"])
        read_resize_jump(config["resize_jump"]);
    if (config["environment_variables"])
        read_environment_variables(config["environment_variables"]);
    if (config["border"])
        read_border(config["border"]);
    if (config["workspaces"])
        read_workspaces(config["workspaces"]);
    if (config["animations"])
        read_animation_definitions(config["animations"]);
    if (config["enable_animations"])
        read_enable_animations(config["enable_animations"]);
    if (config["move_modifier"])
        read_move_modifier(config["move_modifier"]);
    if (config["drag_and_drop"])
        read_drag_and_drop(config["drag_and_drop"]);

    error_handler.on_complete();
}

/// Helper method for quickly creating and reporting an error
void FilesystemConfiguration::add_error(YAML::Node const& node)
{
    error_handler.add_error({ node.Mark().line,
        node.Mark().column,
        ConfigurationInfo::Level::error,
        config_path,
        builder.str() });
    builder = std::stringstream();
}

void FilesystemConfiguration::read_action_key(YAML::Node const& node)
{
    if (auto modifier = try_parse_string_to_optional_value<std::optional<uint>>(node, try_parse_modifier))
        options.primary_modifier = modifier.value();
}

void FilesystemConfiguration::read_custom_actions(YAML::Node const& custom_actions)
{
    if (!custom_actions.IsSequence())
    {
        builder << "Custom actions must be an array";
        add_error(custom_actions);
        return;
    }

    for (auto const& sub_node : custom_actions)
    {
        std::string command;
        std::string key;
        if (!try_parse_value(sub_node, "command", command))
            continue;
        auto keyboard_action = try_parse_string_to_optional_value<std::optional<MirKeyboardAction>>(sub_node, "action", from_string_keyboard_action);
        if (!keyboard_action)
            continue;
        if (!try_parse_value(sub_node, "key", key))
            continue;

        auto const code = libevdev_event_code_from_name(EV_KEY,
            key.c_str()); // https://stackoverflow.com/questions/32059363/is-there-a-way-to-get-the-evdev-keycode-from-a-string
        if (code < 0)
        {
            builder << "Unknown keyboard code in configuration: " << key.c_str() << ". See the linux kernel for allowed codes: https://github.com/torvalds/linux/blob/master/include/uapi/linux/input-event-codes.h";
            add_error(sub_node["key"]);
            continue;
        }

        YAML::Node modifiers_node = sub_node["modifiers"];
        if (!modifiers_node)
        {
            builder << "Missing 'modifiers' in item";
            add_error(sub_node);
            continue;
        }

        uint modifiers = 0;
        if (!try_parse_modifiers(modifiers_node, modifiers))
            continue;

        options.custom_key_commands.push_back({ keyboard_action.value(),
            modifiers,
            code,
            command });
    }
}

void FilesystemConfiguration::read_inner_gaps(YAML::Node const& node)
{
    if (!try_parse_value(node, "x", options.inner_gaps_x))
        return;
    if (!try_parse_value(node, "y", options.inner_gaps_y))
        return;
}

void FilesystemConfiguration::read_outer_gaps(YAML::Node const& node)
{
    if (!try_parse_value(node, "x", options.outer_gaps_x))
        return;
    if (!try_parse_value(node, "y", options.outer_gaps_y))
        return;
}

void FilesystemConfiguration::read_startup_apps(YAML::Node const& startup_apps)
{
    if (!startup_apps.IsSequence())
    {
        builder << "Expected startup applications to be an array";
        add_error(startup_apps);
        return;
    }

    for (auto const& node : startup_apps)
    {
        std::string command;
        if (!try_parse_value(node, "command", command))
            continue;

        bool restart_on_death = false;
        if (node["restart_on_death"])
        {
            if (!try_parse_value(node, "restart_on_death", restart_on_death))
                continue;
        }

        bool in_systemd_scope = false;
        if (node["in_systemd_scope"])
        {
            if (!try_parse_value(node, "in_systemd_scope", in_systemd_scope))
                continue;
        }

        options.startup_apps.push_back({ .command = std::move(command),
            .restart_on_death = restart_on_death,
            .in_systemd_scope = in_systemd_scope });
    }
}

void FilesystemConfiguration::read_terminal(YAML::Node const& node)
{
    std::string desired_terminal;
    if (!try_parse_value(node, desired_terminal))
        return;

    if (!program_exists(desired_terminal))
    {
        builder << "Cannot find requested terminal program: " << desired_terminal;
        add_error(node);
        return;
    }

    options.terminal = desired_terminal;
}

void FilesystemConfiguration::read_resize_jump(YAML::Node const& node)
{
    try_parse_value(node, options.resize_jump);
}

void FilesystemConfiguration::read_environment_variables(YAML::Node const& env)
{
    if (!env.IsSequence())
    {
        builder << "Expected environment variables to be an array";
        add_error(env);
        return;
    }

    for (auto const& node : env)
    {
        std::string key, value;
        if (!try_parse_value(node, "key", key))
            continue;
        if (!try_parse_value(node, "value", value))
            continue;
        options.environment_variables.push_back({ key, value });
    }
}

bool FilesystemConfiguration::try_parse_color(YAML::Node const& node, glm::vec4& color)
{
    constexpr float MAX_COLOR_VALUE = 255;
    float r, g, b, a;
    if (node.IsMap())
    {
        if (!try_parse_value(node, "r", r))
            return false;

        if (!try_parse_value(node, "g", g))
            return false;

        if (!try_parse_value(node, "b", b))
            return false;

        if (!try_parse_value(node, "a", a))
            return false;

        r = r / MAX_COLOR_VALUE;
        g = g / MAX_COLOR_VALUE;
        b = b / MAX_COLOR_VALUE;
        a = a / MAX_COLOR_VALUE;
    }
    else if (node.IsSequence())
    {
        if (node.size() != 4)
        {
            builder << "Expected color values to be an array of size 4";
            add_error(node);
            return false;
        }

        // Parse as [r, g, b, a] array
        r = node[0].as<float>() / MAX_COLOR_VALUE;
        g = node[1].as<float>() / MAX_COLOR_VALUE;
        b = node[2].as<float>() / MAX_COLOR_VALUE;
        a = node[3].as<float>() / MAX_COLOR_VALUE;
    }
    else
    {
        // Parse as hex color
        std::string value;
        if (!try_parse_value(node, value))
            return false;

        try
        {
            unsigned int const i = std::stoul(value, nullptr, 16);
            r = static_cast<float>(((i >> 24) & 0xFF)) / MAX_COLOR_VALUE;
            g = static_cast<float>(((i >> 16) & 0xFF)) / MAX_COLOR_VALUE;
            b = static_cast<float>(((i >> 8) & 0xFF)) / MAX_COLOR_VALUE;
            a = static_cast<float>((i & 0xFF)) / MAX_COLOR_VALUE;
        }
        catch (std::invalid_argument const&)
        {
            builder << "Invalid argument for hex value";
            add_error(node);
            return false;
        }
    }

    r = std::clamp(r, 0.f, 1.f);
    g = std::clamp(g, 0.f, 1.f);
    b = std::clamp(b, 0.f, 1.f);
    a = std::clamp(a, 0.f, 1.f);

    color = { r, g, b, a };
    return true;
}

bool FilesystemConfiguration::try_parse_color(YAML::Node const& root, const char* key, glm::vec4& color)
{
    if (!root[key])
    {
        builder << "Node is missing key: " << key;
        add_error(root);
        return false;
    }

    return try_parse_color(root[key], color);
}

bool FilesystemConfiguration::try_parse_modifiers(YAML::Node const& node, uint& modifiers)
{
    if (!node.IsSequence())
    {
        builder << "Modifiers list must be an array";
        add_error(node);
        return false;
    }

    modifiers = 0;
    for (auto const& modifier_item : node)
    {
        if (auto const modifier = try_parse_string_to_optional_value<std::optional<uint>>(modifier_item, try_parse_modifier))
            modifiers = modifiers | modifier.value();
        else
        {
            builder << "Modifier is invalid";
            add_error(modifier_item);
            return false;
        }
    }

    return true;
}

void FilesystemConfiguration::read_border(YAML::Node const& border)
{
    int size;
    if (!try_parse_value(border, "size", size))
        return;

    glm::vec4 color;
    if (!try_parse_color(border, "color", color))
        return;

    glm::vec4 focus_color;
    if (!try_parse_color(border, "focus_color", focus_color))
        return;

    options.border_config = { size, focus_color, color };
}

void FilesystemConfiguration::read_workspaces(YAML::Node const& workspaces)
{
    if (!workspaces.IsSequence())
    {
        builder << "Expected workspaces to be a sequence";
        add_error(workspaces);
        return;
    }

    for (auto const& workspace : workspaces)
    {
        int num;
        if (!try_parse_value(workspace, "number", num))
            continue;

        std::optional<ContainerType> type;
        if (workspace["key"])
        {
            type = try_parse_string_to_optional_value<std::optional<ContainerType>>(workspace, "layout", container_type_from_string);
            if (!type || type.value() == ContainerType::none)
                continue;
        }

        std::string name;
        if (!try_parse_value(workspace, "name", name, true))
            continue;

        options.workspace_configs.push_back({ num,
            type,
            name.empty() ? std::optional<std::string>(std::nullopt) : name });
    }
}

void FilesystemConfiguration::read_default_action_overrides(YAML::Node const& default_action_overrides)
{
    if (!default_action_overrides.IsSequence())
    {
        builder << "Default action overrides must be an array";
        add_error(default_action_overrides);
        return;
    }

    for (auto const& sub_node : default_action_overrides)
    {
        std::string name;
        if (!try_parse_value(sub_node, "name", name))
            continue;

        std::string action;
        if (!try_parse_value(sub_node, "action", action))
            continue;

        std::string key;
        if (!try_parse_value(sub_node, "key", key))
            return;

        auto const& modifiers_node = sub_node["modifiers"];

        DefaultKeyCommand key_command;
        if (name == "terminal")
            key_command = DefaultKeyCommand::Terminal;
        else if (name == "request_vertical_layout")
            key_command = DefaultKeyCommand::RequestVertical;
        else if (name == "request_horizontal_layout")
            key_command = DefaultKeyCommand::RequestHorizontal;
        else if (name == "toggle_resize")
            key_command = DefaultKeyCommand::ToggleResize;
        else if (name == "resize_up")
            key_command = DefaultKeyCommand::ResizeUp;
        else if (name == "resize_down")
            key_command = DefaultKeyCommand::ResizeDown;
        else if (name == "resize_left")
            key_command = DefaultKeyCommand::ResizeLeft;
        else if (name == "resize_right")
            key_command = DefaultKeyCommand::ResizeRight;
        else if (name == "move_up")
            key_command = DefaultKeyCommand::MoveUp;
        else if (name == "move_down")
            key_command = DefaultKeyCommand::MoveDown;
        else if (name == "move_left")
            key_command = DefaultKeyCommand::MoveLeft;
        else if (name == "move_right")
            key_command = DefaultKeyCommand::MoveRight;
        else if (name == "select_up")
            key_command = DefaultKeyCommand::SelectUp;
        else if (name == "select_down")
            key_command = DefaultKeyCommand::SelectDown;
        else if (name == "select_left")
            key_command = DefaultKeyCommand::SelectLeft;
        else if (name == "select_right")
            key_command = DefaultKeyCommand::SelectRight;
        else if (name == "quit_active_window")
            key_command = DefaultKeyCommand::QuitActiveWindow;
        else if (name == "quit_compositor")
            key_command = DefaultKeyCommand::QuitCompositor;
        else if (name == "fullscreen")
            key_command = DefaultKeyCommand::Fullscreen;
        else if (name == "select_workspace_1")
            key_command = DefaultKeyCommand::SelectWorkspace1;
        else if (name == "select_workspace_2")
            key_command = DefaultKeyCommand::SelectWorkspace2;
        else if (name == "select_workspace_3")
            key_command = DefaultKeyCommand::SelectWorkspace3;
        else if (name == "select_workspace_4")
            key_command = DefaultKeyCommand::SelectWorkspace4;
        else if (name == "select_workspace_5")
            key_command = DefaultKeyCommand::SelectWorkspace5;
        else if (name == "select_workspace_6")
            key_command = DefaultKeyCommand::SelectWorkspace6;
        else if (name == "select_workspace_7")
            key_command = DefaultKeyCommand::SelectWorkspace7;
        else if (name == "select_workspace_8")
            key_command = DefaultKeyCommand::SelectWorkspace8;
        else if (name == "select_workspace_9")
            key_command = DefaultKeyCommand::SelectWorkspace9;
        else if (name == "select_workspace_0")
            key_command = DefaultKeyCommand::SelectWorkspace0;
        else if (name == "move_to_workspace_1")
            key_command = DefaultKeyCommand::MoveToWorkspace1;
        else if (name == "move_to_workspace_2")
            key_command = DefaultKeyCommand::MoveToWorkspace2;
        else if (name == "move_to_workspace_3")
            key_command = DefaultKeyCommand::MoveToWorkspace3;
        else if (name == "move_to_workspace_4")
            key_command = DefaultKeyCommand::MoveToWorkspace4;
        else if (name == "move_to_workspace_5")
            key_command = DefaultKeyCommand::MoveToWorkspace5;
        else if (name == "move_to_workspace_6")
            key_command = DefaultKeyCommand::MoveToWorkspace6;
        else if (name == "move_to_workspace_7")
            key_command = DefaultKeyCommand::MoveToWorkspace7;
        else if (name == "move_to_workspace_8")
            key_command = DefaultKeyCommand::MoveToWorkspace8;
        else if (name == "move_to_workspace_9")
            key_command = DefaultKeyCommand::MoveToWorkspace9;
        else if (name == "move_to_workspace_0")
            key_command = DefaultKeyCommand::MoveToWorkspace0;
        else if (name == "toggle_floating")
            key_command = DefaultKeyCommand::ToggleFloating;
        else if (name == "toggle_pinned_to_workspace")
            key_command = DefaultKeyCommand::TogglePinnedToWorkspace;
        else if (name == "toggle_tabbing")
            key_command = DefaultKeyCommand::ToggleTabbing;
        else if (name == "toggle_stacking")
            key_command = DefaultKeyCommand::ToggleStacking;
        else
        {
            builder << "Unknown key command override: " << sub_node["name"];
            add_error(sub_node["name"]);
            continue;
        }

        auto keyboard_action = try_parse_string_to_optional_value<std::optional<MirKeyboardAction>>(sub_node, "action", from_string_keyboard_action);
        if (!keyboard_action)
            continue;

        auto const code = libevdev_event_code_from_name(EV_KEY, key.c_str()); // https://stackoverflow.com/questions/32059363/is-there-a-way-to-get-the-evdev-keycode-from-a-string
        if (code < 0)
        {
            builder << "Unknown keyboard code in configuration: " << key.c_str() << ". See the linux kernel for allowed codes: https://github.com/torvalds/linux/blob/master/include/uapi/linux/input-event-codes.h";
            add_error(sub_node["key"]);
            continue;
        }

        uint modifiers = 0;
        if (!try_parse_modifiers(modifiers_node, modifiers))
            continue;

        options.key_commands[static_cast<int>(key_command)].push_back({ keyboard_action.value(),
            modifiers,
            code });
    }
}

void FilesystemConfiguration::read_animation_definitions(YAML::Node const& animations_node)
{
    if (!animations_node.IsSequence())
    {
        builder << "Animation definitions must be a sequence";
        add_error(animations_node);
        return;
    }

    for (auto const& node : animations_node)
    {
        auto const& event = try_parse_string_to_optional_value<std::optional<AnimateableEvent>>(
            node,
            "event",
            from_string_animateable_event);
        if (!event)
            continue;

        auto const& type = try_parse_string_to_optional_value<std::optional<AnimationType>>(
            node,
            "type",
            from_string_animation_type);
        if (type == AnimationType::max)
            continue;

        auto const& function = try_parse_string_to_optional_value<std::optional<EaseFunction>>(
            node,
            "function",
            from_string_ease_function);
        if (function == EaseFunction::max)
            continue;

        int const event_as_int = static_cast<int>(event.value());
        options.animation_definitions[event_as_int].type = type.value();
        options.animation_definitions[event_as_int].function = function.value();
        try_parse_value(node, "duration", options.animation_definitions[event_as_int].duration_seconds, true);
        try_parse_value(node, "c1", options.animation_definitions[event_as_int].c1, true);
        try_parse_value(node, "c2", options.animation_definitions[event_as_int].c2, true);
        try_parse_value(node, "c3", options.animation_definitions[event_as_int].c3, true);
        try_parse_value(node, "c4", options.animation_definitions[event_as_int].c4, true);
        try_parse_value(node, "n1", options.animation_definitions[event_as_int].n1, true);
        try_parse_value(node, "d1", options.animation_definitions[event_as_int].d1, true);
    }
}

void FilesystemConfiguration::read_enable_animations(YAML::Node const& node)
{
    try_parse_value(node, options.animations_enabled);
}

void FilesystemConfiguration::read_move_modifier(YAML::Node const& node)
{
    try_parse_modifiers(node, options.move_modifier);
}

void FilesystemConfiguration::read_drag_and_drop(YAML::Node const& node)
{
    try_parse_value(node, "enabled", options.drag_and_drop.enabled, true);
    uint modifiers = 0;
    if (node["modifiers"])
    {
        if (!try_parse_modifiers(node["modifiers"], modifiers))
            return;

        options.drag_and_drop.modifiers = modifiers;
    }
}

void FilesystemConfiguration::_watch(miral::MirRunner& runner)
{
    if (no_config)
    {
        mir::log_info("No configuration was selected, so the configuration will not be watched");
        return;
    }

    inotify_fd = mir::Fd { inotify_init() };
    file_watch = inotify_add_watch(inotify_fd, config_path.c_str(), IN_MODIFY);
    if (file_watch < 0)
        mir::fatal_error("Unable to watch the config file");

    watch_handle = runner.register_fd_handler(inotify_fd, [&](int file_fd)
    {
        union
        {
            inotify_event event;
            char buffer[sizeof(inotify_event) + NAME_MAX + 1];
        } inotify_buffer;

        if (read(inotify_fd, &inotify_buffer, sizeof(inotify_buffer)) < static_cast<ssize_t>(sizeof(inotify_event)))
            return;

        if (inotify_buffer.event.mask & (IN_MODIFY))
        {
            reload();
            has_changes = true;
        }
    });
}

void FilesystemConfiguration::try_process_change()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!has_changes)
        return;

    has_changes = false;
    for (auto const& on_change : on_change_listeners)
    {
        on_change.listener(*this);
    }
}

uint FilesystemConfiguration::get_primary_modifier() const
{
    return options.primary_modifier;
}

std::optional<uint> FilesystemConfiguration::try_parse_modifier(std::string const& stringified_action_key)
{
    if (stringified_action_key == "alt")
        return mir_input_event_modifier_alt;
    else if (stringified_action_key == "alt_left")
        return mir_input_event_modifier_alt_left;
    else if (stringified_action_key == "alt_right")
        return mir_input_event_modifier_alt_right;
    else if (stringified_action_key == "shift")
        return mir_input_event_modifier_shift;
    else if (stringified_action_key == "shift_left")
        return mir_input_event_modifier_shift_left;
    else if (stringified_action_key == "shift_right")
        return mir_input_event_modifier_shift_right;
    else if (stringified_action_key == "sym")
        return mir_input_event_modifier_sym;
    else if (stringified_action_key == "function")
        return mir_input_event_modifier_function;
    else if (stringified_action_key == "ctrl")
        return mir_input_event_modifier_ctrl;
    else if (stringified_action_key == "ctrl_left")
        return mir_input_event_modifier_ctrl_left;
    else if (stringified_action_key == "ctrl_right")
        return mir_input_event_modifier_ctrl_right;
    else if (stringified_action_key == "meta")
        return mir_input_event_modifier_meta;
    else if (stringified_action_key == "meta_left")
        return mir_input_event_modifier_meta_left;
    else if (stringified_action_key == "meta_right")
        return mir_input_event_modifier_meta_right;
    else if (stringified_action_key == "caps_lock")
        return mir_input_event_modifier_caps_lock;
    else if (stringified_action_key == "num_lock")
        return mir_input_event_modifier_num_lock;
    else if (stringified_action_key == "scroll_lock")
        return mir_input_event_modifier_scroll_lock;
    else if (stringified_action_key == "primary")
        return miracle_input_event_modifier_default;
    else
        return std::nullopt;
}

std::string const& FilesystemConfiguration::get_filename() const
{
    return config_path;
}

MirInputEventModifier FilesystemConfiguration::get_input_event_modifier() const
{
    return static_cast<MirInputEventModifier>(options.primary_modifier);
}

CustomKeyCommand const*
FilesystemConfiguration::matches_custom_key_command(MirKeyboardAction action, int scan_code, unsigned int modifiers) const
{
    // TODO: Copy & paste
    for (auto const& command : options.custom_key_commands)
    {
        if (action != command.action)
            continue;

        auto command_modifiers = process_modifier(command.modifiers);
        if (command_modifiers != modifiers)
            continue;

        if (scan_code == command.key)
            return &command;
    }

    return nullptr;
}

bool FilesystemConfiguration::matches_key_command(MirKeyboardAction action, int scan_code, unsigned int modifiers, std::function<bool(DefaultKeyCommand)> const& f) const
{
    for (int i = static_cast<int>(DefaultKeyCommand::MAX) - 1; i >= 0; i--)
    {
        for (auto command : options.key_commands[i])
        {
            if (action != command.action)
                continue;

            auto command_modifiers = process_modifier(command.modifiers);
            if (command_modifiers != modifiers)
                continue;

            if (scan_code == command.key)
            {
                if (f(static_cast<DefaultKeyCommand>(i)))
                    return true;
            }
        }
    }

    return false;
}

int FilesystemConfiguration::get_inner_gaps_x() const
{
    return options.inner_gaps_x;
}

int FilesystemConfiguration::get_inner_gaps_y() const
{
    return options.inner_gaps_y;
}

int FilesystemConfiguration::get_outer_gaps_x() const
{
    return options.outer_gaps_x;
}

int FilesystemConfiguration::get_outer_gaps_y() const
{
    return options.outer_gaps_y;
}

const std::vector<StartupApp>& FilesystemConfiguration::get_startup_apps() const
{
    return options.startup_apps;
}

int FilesystemConfiguration::register_listener(std::function<void(miracle::Config&)> const& func)
{
    return register_listener(func, 5);
}

int FilesystemConfiguration::register_listener(std::function<void(miracle::Config&)> const& func, int priority)
{
    int handle = next_listener_handle++;

    for (auto it = on_change_listeners.begin(); it != on_change_listeners.end(); it++)
    {
        if (it->priority >= priority)
        {
            on_change_listeners.insert(it, { func, priority, handle });
            return handle;
        }
    }

    on_change_listeners.push_back({ func, priority, handle });
    return handle;
}

void FilesystemConfiguration::unregister_listener(int handle)
{
    for (auto it = on_change_listeners.begin(); it != on_change_listeners.end(); it++)
    {
        if (it->handle == handle)
        {
            on_change_listeners.erase(it);
            return;
        }
    }
}

std::optional<std::string> const& FilesystemConfiguration::get_terminal_command() const
{
    return options.terminal;
}

int FilesystemConfiguration::get_resize_jump() const
{
    return options.resize_jump;
}

std::vector<EnvironmentVariable> const& FilesystemConfiguration::get_env_variables() const
{
    return options.environment_variables;
}

BorderConfig const& FilesystemConfiguration::get_border_config() const
{
    return options.border_config;
}

std::array<AnimationDefinition, static_cast<int>(AnimateableEvent::max)> const& FilesystemConfiguration::get_animation_definitions() const
{
    return options.animation_definitions;
}

bool FilesystemConfiguration::are_animations_enabled() const
{
    return options.animations_enabled;
}

WorkspaceConfig FilesystemConfiguration::get_workspace_config(std::optional<int> const& num, std::optional<std::string> const& name) const
{
    for (auto const& config : options.workspace_configs)
    {
        if (num && config.num == num.value())
            return config;
        else if (name && config.name == name.value())
            return config;
    }

    return { num, ContainerType::leaf, name };
}

LayoutScheme FilesystemConfiguration::get_default_layout_scheme() const
{
    return LayoutScheme::horizontal;
}

DragAndDropConfiguration FilesystemConfiguration::drag_and_drop() const
{
    return options.drag_and_drop;
}

uint FilesystemConfiguration::move_modifier() const
{
    return options.move_modifier;
}

FilesystemConfiguration::ConfigDetails::ConfigDetails()
{
    const KeyCommand default_key_commands[static_cast<int>(DefaultKeyCommand::MAX)] = {
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_ENTER },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_V     },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_H     },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_R     },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_UP    },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_DOWN  },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_LEFT  },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_RIGHT },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_UP    },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_DOWN  },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_LEFT  },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_RIGHT },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_UP    },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_DOWN  },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_LEFT  },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_RIGHT },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_Q     },
        { MirKeyboardAction::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_E     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_F     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_1     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_2     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_3     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_4     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_5     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_6     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_7     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_8     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_9     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_0     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_1     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_2     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_3     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_4     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_5     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_6     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_7     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_8     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_9     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_0     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_SPACE },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default | mir_input_event_modifier_shift,
         KEY_P     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_W     },
        { MirKeyboardAction ::mir_keyboard_action_down,
         miracle_input_event_modifier_default,
         KEY_S     }
    };
    for (int i = 0; i < static_cast<int>(DefaultKeyCommand::MAX); i++)
    {
        if (key_commands[i].empty())
            key_commands[i].push_back(default_key_commands[i]);
    }

    std::array<AnimationDefinition, static_cast<int>(AnimateableEvent::max)> parsed({
        {
         AnimationType::grow,
         EaseFunction::ease_in_sine,
         0.3f,
         },
        {
         AnimationType::slide,
         EaseFunction::ease_in_out_back,
         0.25f,
         },
        {
         AnimationType::shrink,
         EaseFunction::ease_out_back,
         0.25f,
         },
        { AnimationType::slide,
         EaseFunction::ease_out_sine,
         0.25f }
    });
    animation_definitions = parsed;
}
