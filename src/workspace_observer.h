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

#ifndef MIRACLEWM_WORKSPACE_OBSERVER_H
#define MIRACLEWM_WORKSPACE_OBSERVER_H

#include "observer_registrar.h"
#include "output.h"
#include <memory>
#include <mir/executor.h>

namespace miracle
{

class Output;

class WorkspaceObserver
{
public:
    virtual ~WorkspaceObserver() = default;
    virtual void on_created(Output const&, int) = 0;
    virtual void on_removed(Output const&, int) = 0;
    virtual void on_focused(Output const* previous, int, Output const* current, int) = 0;
};

class WorkspaceObserverRegistrar : public ObserverRegistrar<WorkspaceObserver>
{
public:
    WorkspaceObserverRegistrar() = default;
    void advise_created(Output const&, int);
    void advise_removed(Output const&, int);
    void advise_focused(Output const* previous, int, Output const* current, int);
};

} // miracle

#endif // MIRACLEWM_WORKSPACE_OBSERVER_H
