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

#define MIR_LOG_COMPONENT "miracle_ipc"

#include "ipc.h"
#include "command_controller.h"
#include "config.h"
#include "ipc_command_executor.h"
#include "output.h"
#include "version.h"
#include "workspace.h"

#include <fcntl.h>
#include <mir/log.h>
#include <nlohmann/json.hpp>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using json = nlohmann::json;
using namespace miracle;

static const char ipc_magic[] = { 'i', '3', '-', 'i', 'p', 'c' };

#define IPC_HEADER_SIZE (sizeof(ipc_magic) + 8)
#define event_mask(ev) (1 << (ev & 0x7F))

namespace
{

struct sockaddr_un* ipc_user_sockaddr()
{
    auto ipc_sockaddr = (sockaddr_un*)malloc(sizeof(struct sockaddr_un));
    if (ipc_sockaddr == nullptr)
    {
        mir::log_error("Can't allocate ipc_sockaddr");
        exit(1);
    }

    ipc_sockaddr->sun_family = AF_UNIX;
    int path_size = sizeof(ipc_sockaddr->sun_path);

    // Env var typically set by logind, e.g. "/run/user/<user-id>"
    const char* dir = getenv("XDG_RUNTIME_DIR");
    if (!dir)
        dir = "/tmp";

    if (path_size <= snprintf(ipc_sockaddr->sun_path, path_size,
            "%s/miracle-wm-ipc.%u.%i.sock", dir, getuid(), getpid()))
    {
        mir::log_error("Socket path won't fit into ipc_sockaddr->sun_path");
        exit(1);
    }

    return ipc_sockaddr;
}

bool fd_is_valid(int fd)
{
    return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

json mode_event_to_json(WindowManagerMode mode)
{
    switch (mode)
    {
    case WindowManagerMode::normal:
        return {
            { "change",       "default" },
            { "pango_markup", true      }
        };
    case WindowManagerMode::resizing:
        return {
            { "change",       "resize" },
            { "pango_markup", true     }
        };
    case WindowManagerMode::selecting:
        return {
            { "change",       "selecting" },
            { "pango_markup", true        }
        };
    case WindowManagerMode::dragging:
        return {
            { "change",       "dragging" },
            { "pango_markup", true       }
        };
    case WindowManagerMode::moving:
        return {
            { "change",       "moving" },
            { "pango_markup", true     }
        };
    default:
    {
        mir::fatal_error("handle_command: unknown binding state: %d", (int)mode);
        return {};
    }
    }
}
}

Ipc::Ipc(miral::MirRunner& runner,
    CommandController& policy,
    IpcCommandExecutor& executor,
    std::shared_ptr<Config> const& config) :
    policy { policy },
    executor { executor },
    config { config }
{
    auto ipc_socket_raw = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ipc_socket_raw == -1)
    {
        mir::log_error("Unable to create ipc socket");
        exit(1);
    }

    if (fcntl(ipc_socket_raw, F_SETFD, FD_CLOEXEC) == -1)
    {
        mir::log_error("Unable to set CLOEXEC on IPC socket");
        exit(1);
    }
    if (fcntl(ipc_socket_raw, F_SETFL, O_NONBLOCK) == -1)
    {
        mir::log_error("Unable to set NONBLOCK on IPC socket");
        exit(1);
    }

    ipc_sockaddr = ipc_user_sockaddr();
    if (getenv("SWAYSOCK") != nullptr && access(getenv("SWAYSOCK"), F_OK) == -1)
    {
        strncpy(ipc_sockaddr->sun_path, getenv("SWAYSOCK"), sizeof(ipc_sockaddr->sun_path) - 1);
        ipc_sockaddr->sun_path[sizeof(ipc_sockaddr->sun_path) - 1] = 0;
    }

    unlink(ipc_sockaddr->sun_path);
    if (bind(ipc_socket_raw, (struct sockaddr*)ipc_sockaddr, sizeof(*ipc_sockaddr)) == -1)
    {
        mir::log_error("Unable to bind IPC socket");
        exit(1);
    }

    if (listen(ipc_socket_raw, 3) == -1)
    {
        mir::log_error("Unable to listen on IPC socket");
        exit(1);
    }

    // Set i3 IPC socket path so that i3-msg works out of the box
    setenv("I3SOCK", ipc_sockaddr->sun_path, 1);
    setenv("SWAYSOCK", ipc_sockaddr->sun_path, 1);

    mir::log_info("Listening to IPC socket on path: %s", ipc_sockaddr->sun_path);

    ipc_socket = mir::Fd { ipc_socket_raw };
    socket_handle = runner.register_fd_handler(ipc_socket, [&](int fd)
    {
        int client_fd = accept(ipc_socket, NULL, NULL);
        if (client_fd == -1)
        {
            mir::log_error("Unable to accept IPC client connection");
            return;
        }

        int flags;
        if ((flags = fcntl(client_fd, F_GETFD)) == -1
            || fcntl(client_fd, F_SETFD, flags | FD_CLOEXEC) == -1)
        {
            mir::log_error("Unable to set CLOEXEC on IPC client socket");
            close(client_fd);
            return;
        }
        if ((flags = fcntl(client_fd, F_GETFL)) == -1
            || fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            mir::log_error("Unable to set NONBLOCK on IPC client socket");
            close(client_fd);
            return;
        }

        auto mir_fd = mir::Fd { client_fd };
        clients.push_back({ mir_fd,
            runner.register_fd_handler(mir_fd, [this](int fd)
        {
            auto& client = get_client(fd);

            int read_available;
            if (ioctl(client.client_fd, FIONREAD, &read_available) == -1)
            {
                mir::log_error("Unable to read IPC socket buffer size");
                disconnect(client);
                return;
            }

            if (client.pending_read_length > 0)
            {
                if ((uint32_t)read_available >= client.pending_read_length)
                {
                    // Reset pending values.
                    uint32_t pending_length = client.pending_read_length;
                    IpcType pending_type = client.pending_type;
                    client.pending_read_length = 0;
                    handle_command(client, pending_length, pending_type);
                }
                return;
            }

            if (read_available < (int)IPC_HEADER_SIZE)
            {
                return;
            }

            uint8_t buf[IPC_HEADER_SIZE];
            // Should be fully available, because read_available >= IPC_HEADER_SIZE
            ssize_t received = recv(client.client_fd, buf, IPC_HEADER_SIZE, 0);
            if (received == -1)
            {
                mir::log_error("Unable to receive header from IPC client");
                disconnect(client);
                return;
            }

            if (memcmp(buf, ipc_magic, sizeof(ipc_magic)) != 0)
            {
                mir::log_error("IPC header check failed");
                disconnect(client);
                return;
            }

            memcpy(&client.pending_read_length, buf + sizeof(ipc_magic), sizeof(uint32_t));
            memcpy(&client.pending_type, buf + sizeof(ipc_magic) + sizeof(uint32_t), sizeof(uint32_t));
            mir::log_debug("Received request from IPC client: %d", (int)client.pending_type);

            if (read_available - received >= (long)client.pending_read_length)
            {
                // Reset pending values.
                uint32_t pending_length = client.pending_read_length;
                IpcType pending_type = client.pending_type;
                client.pending_read_length = 0;
                handle_command(client, pending_length, pending_type);
            }
        }) });
    });
}

Ipc::~Ipc()
{
}

void Ipc::on_created(uint32_t id)
{
    json j = {
        { "change",  "init"                       },
        { "old",     nullptr                      },
        { "current", policy.workspace_to_json(id) }
    };

    auto serialized_value = to_string(j);
    for (auto& client : clients)
    {
        if ((client.subscribed_events & event_mask(IPC_EVENT_WORKSPACE)) == 0)
        {
            continue;
        }

        send_reply(client, IPC_EVENT_WORKSPACE, serialized_value);
    }
}

void Ipc::on_removed(uint32_t id)
{
    json j = {
        { "change",  "empty"                      },
        { "current", policy.workspace_to_json(id) }
    };

    auto serialized_value = to_string(j);
    for (auto& client : clients)
    {
        if ((client.subscribed_events & event_mask(IPC_EVENT_WORKSPACE)) == 0)
        {
            continue;
        }

        send_reply(client, IPC_EVENT_WORKSPACE, serialized_value);
    }
}

void Ipc::on_focused(
    std::optional<uint32_t> previous_id,
    uint32_t current_id)
{
    json j = {
        { "change",  "focus"                              },
        { "current", policy.workspace_to_json(current_id) }
    };

    if (previous_id)
        j["old"] = policy.workspace_to_json(previous_id.value());
    else
        j["old"] = nullptr;

    auto serialized_value = to_string(j);
    for (auto& client : clients)
    {
        if ((client.subscribed_events & event_mask(IPC_EVENT_WORKSPACE)) == 0)
        {
            continue;
        }

        send_reply(client, IPC_EVENT_WORKSPACE, serialized_value);
    }
}

void Ipc::on_changed(WindowManagerMode mode)
{
    auto response = to_string(mode_event_to_json(mode));
    for (auto& client : clients)
    {
        if ((client.subscribed_events & event_mask(IPC_EVENT_MODE)) == 0)
        {
            continue;
        }

        send_reply(client, IPC_EVENT_MODE, response);
    }
}

void Ipc::on_shutdown()
{
    auto response = to_string(json({
        { "change", "exit" }
    }));
    for (auto& client : clients)
    {
        if ((client.subscribed_events & event_mask(IPC_EVENT_SHUTDOWN)) == 0)
        {
            continue;
        }

        send_reply(client, IPC_EVENT_SHUTDOWN, response);
    }
}

Ipc::IpcClient& Ipc::get_client(int fd)
{
    for (auto& client : clients)
    {
        if (client.client_fd == fd)
            return client;
    }

    throw std::runtime_error("Could not find IPC client");
}

void Ipc::disconnect(Ipc::IpcClient& client)
{
    auto it = std::find_if(clients.begin(), clients.end(), [&](IpcClient const& other)
    {
        return other.client_fd.operator int() == client.client_fd.operator int();
    });
    if (it != clients.end())
    {
        if (fd_is_valid(client.client_fd))
            shutdown(client.client_fd, SHUT_RDWR);
        mir::log_info("Disconnected client: %d", (int)client.client_fd);
        clients.erase(it);
    }
    else
    {
        mir::log_error("Unable to disconnect client");
    }
}

void Ipc::handle_command(miracle::Ipc::IpcClient& client, uint32_t payload_length, miracle::IpcType payload_type)
{
    char* buf = (char*)malloc(payload_length + 1);
    if (!buf)
    {
        mir::log_error("Unable to allocate IPC payload");
        disconnect(client);
        return;
    }

    if (payload_length > 0)
    {
        // Payload should be fully available
        ssize_t received = recv(client.client_fd, buf, payload_length, 0);
        if (received == -1)
        {
            mir::log_error("Unable to receive payload from IPC client");
            disconnect(client);
            free(buf);
            return;
        }
    }
    buf[payload_length] = '\0';

    switch (payload_type)
    {
    case IPC_COMMAND:
    {
        mir::log_debug("Processing i3_command: %s", buf);
        auto result = parse_i3_command(buf);
        if (result.success)
        {
            const std::string msg = "[{\"success\": true}]";
            send_reply(client, payload_type, msg);
        }
        else
        {
            json j = json::array();
            j.push_back({
                { "success",     false              },
                { "parse_error", result.parse_error },
                { "error",       result.error       },
            });
            const std::string msg = to_string(j);
            send_reply(client, payload_type, msg);
        }
        break;
    }
    case IPC_GET_WORKSPACES:
    {
        auto json_string = to_string(policy.workspaces_json());
        send_reply(client, payload_type, json_string);
        break;
    }
    case IPC_GET_OUTPUTS:
    {
        auto json_string = to_string(policy.outputs_json());
        send_reply(client, payload_type, json_string);
        break;
    }
    case IPC_SUBSCRIBE:
    {
        json j = json::parse(buf);
        bool success = true;
        bool send_event_tick = false;
        for (auto const& i : j)
        {
            std::string event_type = i.template get<std::string>();
            mir::log_debug("Received subscription request from IPC client for event: %s", event_type.c_str());
            if (event_type == "workspace")
                client.subscribed_events |= event_mask(IPC_EVENT_WORKSPACE);
            else if (event_type == "window")
                client.subscribed_events |= event_mask(IPC_EVENT_WINDOW);
            else if (event_type == "input")
                client.subscribed_events |= event_mask(IPC_EVENT_INPUT);
            else if (event_type == "mode")
                client.subscribed_events |= event_mask(IPC_EVENT_MODE);
            else if (event_type == "tick")
            {
                client.subscribed_events |= event_mask(IPC_EVENT_TICK);
                send_event_tick = true;
            }
            else if (event_type == "shutdown")
                client.subscribed_events |= event_mask(IPC_EVENT_SHUTDOWN);
            else
            {
                mir::log_error("Cannot process IPC subscription event for event_type: %s", event_type.c_str());
                disconnect(client);
                success = false;
                break;
            }
        }

        if (success)
        {
            const std::string msg = "{\"success\": true}";
            send_reply(client, payload_type, msg);
        }

        if (send_event_tick)
        {
            json response = {
                { "first",   true },
                { "payload", ""   }
            };
            send_reply(client, IPC_EVENT_TICK, to_string(response));
        }

        break;
    }
    case IPC_GET_TREE:
    {
        auto json_string = to_string(policy.to_json());
        send_reply(client, payload_type, json_string);
        break;
    }
    case IPC_GET_VERSION:
    {
        json response = {
            { "major",                   MIRACLE_WM_MAJOR       },
            { "minor",                   MIRACLE_WM_MINOR       },
            { "patch",                   MIRACLE_WM_PATCH       },
            { "human_readable",          MIRACLE_VERSION_STRING },
            { "loaded_config_file_name", config->get_filename() }
        };
        send_reply(client, payload_type, to_string(response));
        break;
    }
    case IPC_GET_BINDING_MODES:
    {
        json response;
        response.push_back("default");
        response.push_back("resize");
        response.push_back("selecting");
        send_reply(client, payload_type, to_string(response));
        break;
    }
    case IPC_GET_BINDING_STATE:
    {
        send_reply(client, payload_type, to_string(policy.mode_to_json()));
        break;
    }
    case IPC_SEND_TICK:
    {
        const std::string msg = "{\"success\": true}";
        send_reply(client, payload_type, msg);

        for (auto& other_client : clients)
        {
            if ((other_client.subscribed_events & event_mask(IPC_EVENT_TICK)) == 0)
            {
                continue;
            }

            json response = {
                { "first",   false            },
                { "payload", std::string(buf) }
            };
            send_reply(other_client, IPC_EVENT_TICK, to_string(response));
        }
        break;
    }
    default:
        mir::log_warning("Unknown payload type: %d", payload_type);
        disconnect(client);
        return;
    }
}

void Ipc::send_reply(miracle::Ipc::IpcClient& client, miracle::IpcType command_type, const std::string& payload)
{
    if (!fd_is_valid(client.client_fd.operator int()))
    {
        mir::log_warning("Unable to send reply to client: file descriptor is invalid");
        disconnect(client);
        return;
    }

    const uint32_t payload_length = payload.size();
    char data[IPC_HEADER_SIZE];

    const auto casted_command = static_cast<uint32_t>(command_type);
    memcpy(data, ipc_magic, sizeof(ipc_magic));
    memcpy(data + sizeof(ipc_magic), &payload_length, sizeof(payload_length));
    memcpy(data + sizeof(ipc_magic) + sizeof(payload_length), &casted_command, sizeof(casted_command));

    auto new_buffer_size = client.buffer.size();
    while (client.write_buffer_len + IPC_HEADER_SIZE + payload_length >= new_buffer_size)
    {
        if (new_buffer_size == 0)
            new_buffer_size = 1;
        new_buffer_size *= 2;
    }

    if (new_buffer_size > 4e6)
    { // 4 MB
        mir::log_error("Client write buffer too big (%zu), disconnecting client", client.buffer.size());
        disconnect(client);
        return;
    }

    client.buffer.resize(new_buffer_size);

    memcpy(client.buffer.data() + client.write_buffer_len, data, IPC_HEADER_SIZE);
    client.write_buffer_len += IPC_HEADER_SIZE;
    memcpy(client.buffer.data() + client.write_buffer_len, payload.c_str(), payload_length);
    client.write_buffer_len += payload_length;
    handle_writeable(client);
}

namespace
{
// https://stackoverflow.com/questions/24920748/how-to-handle-a-sigpipe-error-inside-the-object-that-generated-it
ssize_t write_nosigpipe(int fd, void* buf, size_t len)
{
    sigset_t oldset, newset;
    ssize_t result;
    siginfo_t si;
    struct timespec ts = { 0 };

    sigemptyset(&newset);
    sigaddset(&newset, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &newset, &oldset);

    result = write(fd, buf, len);

    while (sigtimedwait(&newset, &si, &ts) >= 0 || errno != EAGAIN)
        ;
    pthread_sigmask(SIG_SETMASK, &oldset, 0);

    return result;
}
}

void Ipc::handle_writeable(miracle::Ipc::IpcClient& client)
{
    while (client.write_buffer_len > 0)
    {
        ssize_t written = write_nosigpipe(client.client_fd, client.buffer.data(), client.write_buffer_len);
        if (written == -1 && errno == EAGAIN)
        {
            return;
        }
        else if (written == -1)
        {
            mir::log_error("Unable to send data from queue to IPC client");
            disconnect(client);
            return;
        }

        memmove(client.buffer.data(), client.buffer.data() + written, client.write_buffer_len - written);
        client.write_buffer_len -= written;
    }

    client.write_buffer_len = 0;
}

IpcValidationResult Ipc::parse_i3_command(const char* command)
{
    IpcCommandParser parser(command);
    auto const pending_commands = parser.parse();
    return executor.process(pending_commands);
}
