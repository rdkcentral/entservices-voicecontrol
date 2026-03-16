#include "VoiceControl.h"

namespace WPEFramework {
namespace Plugin {

    namespace {
        static Metadata<VoiceControl> metadata(1, 0, 0, {}, {}, {});
    }

    const string VoiceControl::Initialize(PluginHost::IShell* service)
    {
        string message;
        _service = service;
        _service->AddRef();
        _service->Register(&_connectionNotification);

        _implementation = _service->Root<Exchange::IVoiceControl>(_connectionId, 2000, _T("VoiceControlImplementation"));
        if (_implementation == nullptr) {
            message = _T("VoiceControl could not be instantiated");
        } else {
            _implementation->Register(&_notification);
            Exchange::JVoiceControl::Register(*this, _implementation);
        }
        return message;
    }

    void VoiceControl::Deinitialize(PluginHost::IShell* service)
    {
        if (_implementation != nullptr) {
            Exchange::JVoiceControl::Unregister(*this);
            _implementation->Unregister(&_notification);

            RPC::IRemoteConnection* connection = _service->RemoteConnection(_connectionId);
            _implementation->Release();
            _implementation = nullptr;
            if (connection != nullptr) {
                connection->Terminate();
                connection->Release();
            }
        }
        _service->Unregister(&_connectionNotification);
        _service->Release();
        _service = nullptr;
        _connectionId = 0;
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

