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

#pragma once

#include "Module.h"
#include <interfaces/IVoiceControl.h>
#include <interfaces/IConfiguration.h>
#include <atomic>
#include <vector>
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
        Core::hresult GetApiVersionNumber(Exchange::VoiceControlGetApiVersionNumberResponse& response) override;
        Core::hresult GetVoiceStatus(Exchange::VoiceStatusResponse& response) override;
        Core::hresult ConfigureVoice(const string& payload, Exchange::VoiceControlSuccessResult& result) override;
        Core::hresult SetVoiceInit(const string& payload, Exchange::VoiceControlSuccessResult& result) override;
        Core::hresult SendVoiceMessage(const string& msgType, const string& trx, const uint64_t created, const string& msgPayload, Exchange::VoiceControlSuccessResult& result) override;
        Core::hresult VoiceSessionByText(const string& transcription, const Exchange::DeviceType type, Exchange::VoiceControlSuccessResult& result) override;
        Core::hresult GetVoiceSessionTypes(Exchange::GetVoiceSessionTypesResult& result) override;
        Core::hresult VoiceSessionRequest(const string& payload, string& result) override;
        Core::hresult VoiceSessionTerminate(const string& sessionId, Exchange::VoiceControlSuccessResult& result) override;
        Core::hresult VoiceSessionAudioStreamStart(const string& sessionId, Exchange::VoiceControlSuccessResult& result) override;

        virtual Core::hresult Register(Exchange::IVoiceControl::INotification* notification) override;
        virtual Core::hresult Unregister(const Exchange::IVoiceControl::INotification* notification) override;

        // IConfiguration interface
        Core::hresult Configure(PluginHost::IShell* service) override;

    private:
        bool InitializeIARM();
        void DeinitializeIARM();

        static void voiceEventHandler(const char* owner, IARM_EventId_t eventId, void* data, size_t len);
        void iarmEventHandler(const char* owner, IARM_EventId_t eventId, void* data, size_t len);

        void NotifySessionBegin(ctrlm_voice_iarm_event_json_t* eventData);
        void NotifyStreamBegin(ctrlm_voice_iarm_event_json_t* eventData);
        void NotifyKeywordVerification(ctrlm_voice_iarm_event_json_t* eventData);
        void NotifyServerMessage(ctrlm_voice_iarm_event_json_t* eventData);
        void NotifyStreamEnd(ctrlm_voice_iarm_event_json_t* eventData);
        void NotifySessionEnd(ctrlm_voice_iarm_event_json_t* eventData);

        std::vector<Exchange::IVoiceControl::INotification*> ObserverSnapshot();
        void ReleaseObserverSnapshot(std::vector<Exchange::IVoiceControl::INotification*>& observers);

        Core::hresult IARMBusCall(const string& method, const string& jsonParams, JsonObject& result);

        Core::CriticalSection _adminLock;
        PluginHost::IShell* _service;
        std::vector<Exchange::IVoiceControl::INotification*> _notifications;
        bool _hasOwnProcess;
        uint8_t _handlersRegistered;
        std::atomic<bool> _maskPii;

        static Core::CriticalSection _instanceLock;
        static VoiceControlImplementation* _instance;
    };

} // namespace Plugin
} // namespace WPEFramework
