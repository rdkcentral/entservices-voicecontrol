 /*
  * If not stated otherwise in this file or this component's LICENSE file the
  * following copyright and licenses apply:
  *
  * Copyright 2026 RDK Management
  *
  * Licensed under the Apache License, Version 2.0 (the "License");
  * you may not use this file except in compliance with the License.
  * You may obtain a copy of the License at
  *
  * http://www.apache.org/licenses/LICENSE-2.0
  *
  * Unless required by applicable law or agreed to in writing, software
  * distributed under the License is distributed on an "AS IS" BASIS,
  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  * See the License for the specific language governing permissions and
  * limitations under the License.
  */

#include "VoiceControl.h"
#include "PluginVersion.h"
#include "UtilsLogging.h"

namespace WPEFramework {

    namespace {
        static Plugin::Metadata<Plugin::VoiceControl> metadata(
            // Version (Major, Minor, Patch)
            API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH,
            // Preconditions
            {},
            // Terminations
            {},
            // Controls
            {}
        );
    }

namespace Plugin {

    SERVICE_REGISTRATION(VoiceControl, API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH);

    const string VoiceControl::Initialize(PluginHost::IShell* service)
    {
        string message;
        PluginHost::IShell* shell = service;
        Exchange::IVoiceControl* implementation = nullptr;
        Exchange::IConfiguration* configure = nullptr;
        uint32_t connectionId = 0;

        ASSERT(service != nullptr);
        _adminLock.Lock();
        ASSERT(_service == nullptr);
        ASSERT(_implementation == nullptr);
        ASSERT(_configure == nullptr);
        ASSERT(_connectionId == 0);
        _isShuttingDown = false;
        _service = shell;
        _service->AddRef();
        _adminLock.Unlock();

        shell->Register(&_connectionNotification);

        implementation = shell->Root<Exchange::IVoiceControl>(connectionId, 2000, _T("VoiceControlImplementation"));

        if (implementation != nullptr)
        {
            configure = implementation->QueryInterface<Exchange::IConfiguration>();
            if (configure != nullptr)
            {
                const uint32_t result = configure->Configure(shell);
                if (result != Core::ERROR_NONE)
                {
                    message = _T("VoiceControl could not be configured");
                }
            }
            else
            {
                message = _T("VoiceControl implementation did not provide a configuration interface");
            }

            if (message.empty())
            {
                const uint32_t registerResult = implementation->Register(&_notification);
                if (registerResult != Core::ERROR_NONE)
                {
                    message = _T("VoiceControl failed to register notification handler");
                }
                else
                {
                    _adminLock.Lock();
                    _implementation = implementation;
                    _configure = configure;
                    _connectionId = connectionId;
                    _adminLock.Unlock();

                    Exchange::JVoiceControl::Register(*this, implementation);
                }
            }
        }
        else
        {
            message = _T("VoiceControl could not be instantiated");
        }

        if (!message.empty())
        {
            _adminLock.Lock();
            _service = nullptr;
            _connectionId = 0;
            _isShuttingDown = false;
            _adminLock.Unlock();

            if (implementation != nullptr)
            {
                if (configure != nullptr)
                {
                    configure->Release();
                }
                RPC::IRemoteConnection* connection = shell->RemoteConnection(connectionId);
                VARIABLE_IS_NOT_USED const uint32_t result = implementation->Release();
                if (connection != nullptr)
                {
                    connection->Terminate();
                    connection->Release();
                }
            }

            shell->Unregister(&_connectionNotification);
            shell->Release();
        }

        return message;
    }

    void VoiceControl::Deinitialize(PluginHost::IShell* service)
    {
        Exchange::IVoiceControl* implementation = nullptr;
        Exchange::IConfiguration* configure = nullptr;
        PluginHost::IShell* shell = nullptr;
        uint32_t connectionId = 0;

        _adminLock.Lock();
        ASSERT(_service == service);
        _isShuttingDown = true;
        implementation = _implementation;
        configure = _configure;
        shell = _service;
        connectionId = _connectionId;
        _implementation = nullptr;
        _configure = nullptr;
        _service = nullptr;
        _connectionId = 0;
        _adminLock.Unlock();

        shell->Unregister(&_connectionNotification);

        if (implementation != nullptr)
        {
            implementation->Unregister(&_notification);
            Exchange::JVoiceControl::Unregister(*this);

            if (configure != nullptr) {
                configure->Release();
            }

            RPC::IRemoteConnection* connection = service->RemoteConnection(connectionId);
            VARIABLE_IS_NOT_USED const uint32_t result = implementation->Release();
            if (result != Core::ERROR_DESTRUCTION_SUCCEEDED)
            {
                LOGWARN("VoiceControl implementation release returned %u during shutdown; proceeding with remote connection termination.", result);
            }

            if (connection != nullptr)
            {
                connection->Terminate();
                connection->Release();
            }
        }

        shell->Release();

        _adminLock.Lock();
        _isShuttingDown = false;
        _adminLock.Unlock();
    }

    void VoiceControl::Deactivated(RPC::IRemoteConnection* connection)
    {
        PluginHost::IShell* shell = nullptr;

        _adminLock.Lock();
        if ((_isShuttingDown == false) && (_service != nullptr) && (connection->Id() == _connectionId)) {
            shell = _service;
            shell->AddRef();
        }
        _adminLock.Unlock();

        if (shell != nullptr) {
            Core::IWorkerPool::Instance().Submit(
                PluginHost::IShell::Job::Create(shell,
                    PluginHost::IShell::DEACTIVATED, PluginHost::IShell::FAILURE));
            shell->Release();
        }
    }

} // namespace Plugin
} // namespace WPEFramework

