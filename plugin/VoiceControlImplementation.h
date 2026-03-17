#pragma once

#include "Module.h"
#include <interfaces/IVoiceControl.h>
#include <interfaces/IConfiguration.h>

#include "libIBus.h"
#include "ctrlm_ipc.h"
#include "ctrlm_ipc_voice.h"

namespace WPEFramework {
namespace Plugin {

    class VoiceControlImplementation : public Exchange::IVoiceControl, public Exchange::IConfiguration {
    public:
        VoiceControlImplementation(const VoiceControlImplementation&) = delete;
        VoiceControlImplementation& operator=(const VoiceControlImplementation&) = delete;

        VoiceControlImplementation();
        ~VoiceControlImplementation() override;

        BEGIN_INTERFACE_MAP(VoiceControlImplementation)
            INTERFACE_ENTRY(Exchange::IVoiceControl)
            INTERFACE_ENTRY(Exchange::IConfiguration)
        END_INTERFACE_MAP

        // IVoiceControl methods
        Core::hresult GetApiVersionNumber(Exchange::GetApiVersionNumberResponse& response) override;
        Core::hresult SendNotify_(const string& eventName, string& parameters) override;
        Core::hresult GetVoiceStatus(Exchange::VoiceStatusResponse& response, Exchange::IStringIterator*& capabilities) override;
        Core::hresult ConfigureVoice(const Exchange::ConfigureVoiceRequest& request, bool& success) override;
        Core::hresult SetVoiceInit(const string& language, Exchange::IStringIterator* const capabilities, bool& success) override;
        Core::hresult SendVoiceMessage(const Exchange::ServerMessageEvent& request, bool& success) override;
        Core::hresult VoiceSessionByText(const Exchange::VoiceSessionByTextRequest& request, bool& success) override;
        Core::hresult GetVoiceSessionTypes(bool& success, Exchange::IStringIterator*& types) override;
        Core::hresult VoiceSessionRequest(const Exchange::VoiceSessionRequestData& request, bool& success) override;
        Core::hresult VoiceSessionTerminate(const Exchange::VoiceSessionTerminateRequest& request, bool& success) override;
        Core::hresult VoiceSessionAudioStreamStart(const Exchange::VoiceSessionTerminateRequest& request, bool& success) override;

        virtual Core::hresult Register(Exchange::IVoiceControl::INotification* notification) override;
        virtual Core::hresult Unregister(const Exchange::IVoiceControl::INotification* notification) override;

        // IConfiguration interface
        uint32_t Configure(PluginHost::IShell* service) override;

    private:
        void InitializeIARM();
        void DeinitializeIARM();

        static void voiceEventHandler(const char* owner, IARM_EventId_t eventId, void* data, size_t len);
        void iarmEventHandler(const char* owner, IARM_EventId_t eventId, void* data, size_t len);

        void NotifySessionBegin(ctrlm_voice_iarm_event_json_t* eventData);
        void NotifyStreamBegin(ctrlm_voice_iarm_event_json_t* eventData);
        void NotifyKeywordVerification(ctrlm_voice_iarm_event_json_t* eventData);
        void NotifyServerMessage(ctrlm_voice_iarm_event_json_t* eventData);
        void NotifyStreamEnd(ctrlm_voice_iarm_event_json_t* eventData);
        void NotifySessionEnd(ctrlm_voice_iarm_event_json_t* eventData);

        Core::hresult IARMBusCall(const string& method, const string& jsonParams, JsonObject& result);

        Core::CriticalSection _adminLock;
        PluginHost::IShell* _service;
        std::vector<Exchange::IVoiceControl::INotification*> _notifications;
        bool _hasOwnProcess;
        bool _maskPii;

        static VoiceControlImplementation* _instance;
    };

} // namespace Plugin
} // namespace WPEFramework
