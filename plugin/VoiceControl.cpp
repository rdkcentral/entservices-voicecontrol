#include "VoiceControl.h"

namespace WPEFramework {
namespace Plugin {

    namespace {
        static Metadata<VoiceControl> metadata(1, 0, 0, {}, {}, {});
    }

    const string VoiceControl::Initialize(PluginHost::IShell* service)
    {
        string message;

        ASSERT(nullptr != service);
        ASSERT(nullptr == _service);
        ASSERT(nullptr == _implementation);
        ASSERT(0 == _connectionId);

        _service = service;
        _service->AddRef();
        _service->Register(&_connectionNotification);

        _implementation = _service->Root<Exchange::IVoiceControl>(_connectionId, 2000, _T("VoiceControlImplementation"));

        if (nullptr != _implementation)
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

            _implementation->Register(&_notification);
            Exchange::JVoiceControl::Register(*this, _implementation);
        }
        else
        {
            message = _T("VoiceControl could not be instantiated");
        }

        return message;
    }

    void VoiceControl::Deinitialize(PluginHost::IShell* service)
    {
        ASSERT(_service == service);

        _service->Unregister(&_connectionNotification);

        if (nullptr != _implementation)
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

