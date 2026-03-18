#include "VoiceControl.h"

#define API_VERSION_NUMBER_MAJOR 1
#define API_VERSION_NUMBER_MINOR 0
#define API_VERSION_NUMBER_PATCH 1

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

    SERVICE_REGISTRATION(VoiceControl, API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH)

    const string VoiceControl::Initialize(PluginHost::IShell* service)
    {
        string message;

        ASSERT(service != nullptr);
        ASSERT(_service == nullptr);
        ASSERT(_implementation == nullptr);
        ASSERT(_connectionId == 0);

        _service = service;
        _service->AddRef();
        _service->Register(&_connectionNotification);

        _implementation = _service->Root<Exchange::IVoiceControl>(_connectionId, 2000, _T("VoiceControlImplementation"));

        if (_implementation != nullptr)
        {
            _configure = _implementation->QueryInterface<Exchange::IConfiguration>();
            if (_configure != nullptr)
            {
                uint32_t result = _configure->Configure(_service);
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
                _implementation->Register(&_notification);
                Exchange::JVoiceControl::Register(*this, _implementation);
            }
        }
        else
        {
            message = _T("VoiceControl could not be instantiated");
        }

        if (!message.empty())
        {
            if (_implementation != nullptr)
            {
                if (_configure != nullptr)
                {
                    _configure->Release();
                    _configure = nullptr;
                }
                RPC::IRemoteConnection* connection = _service->RemoteConnection(_connectionId);
                VARIABLE_IS_NOT_USED uint32_t result = _implementation->Release();
                _implementation = nullptr;
                if (connection != nullptr)
                {
                    connection->Terminate();
                    connection->Release();
                }
            }

            _service->Unregister(&_connectionNotification);
            _connectionId = 0;
            _service->Release();
            _service = nullptr;
        }

        return message;
    }

    void VoiceControl::Deinitialize(PluginHost::IShell* service)
    {
        ASSERT(_service == service);

        _service->Unregister(&_connectionNotification);

        if (_implementation != nullptr)
        {
            _implementation->Unregister(&_notification);
            Exchange::JVoiceControl::Unregister(*this);

            if (_configure != nullptr) {
                _configure->Release();
                _configure = nullptr;
            }

            RPC::IRemoteConnection* connection = service->RemoteConnection(_connectionId);
            VARIABLE_IS_NOT_USED uint32_t result = _implementation->Release();
            _implementation = nullptr;

            ASSERT(result == Core::ERROR_DESTRUCTION_SUCCEEDED);

            if (nullptr != connection)
            {
                connection->Terminate();
                connection->Release();
            }
        }

        _connectionId = 0;
        _service->Release();
        _service = nullptr;
    }

    void VoiceControl::Deactivated(RPC::IRemoteConnection* connection)
    {
        if (connection->Id() == _connectionId) {
            Core::IWorkerPool::Instance().Submit(
                PluginHost::IShell::Job::Create(_service,
                    PluginHost::IShell::DEACTIVATED, PluginHost::IShell::FAILURE));
        }
    }

} // namespace Plugin
} // namespace WPEFramework

