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

#include "VoiceControlImplementation.h"
#include "PluginVersion.h"
#include "libIBusDaemon.h"
#include "UtilsIarm.h"

#include <algorithm>
#include <list>

namespace WPEFramework {
namespace Plugin {

    namespace {
        // ─── Enum ↔ string helpers for ctrlm IARM serialization ───
        // ctrlm sends string representations of enums over JSON.

        template <typename E>
        E stringToEnum(const string& str, E defaultValue);

        // --- DeviceType: ctrlm sends uppercase "PTT"/"FF"/"MIC" ---
        template <>
        Exchange::DeviceType stringToEnum<Exchange::DeviceType>(const string& str, Exchange::DeviceType defaultValue) {
            if (str == "PTT") return Exchange::DeviceType::PTT;
            if (str == "FF")  return Exchange::DeviceType::FF;
            if (str == "MIC") return Exchange::DeviceType::MIC;
            return defaultValue;
        }

        // --- SessionResult: ctrlm sends lowercase/camelCase "success"/"error"/"abort"/"shortUtterance" ---
        template <>
        Exchange::SessionResult stringToEnum<Exchange::SessionResult>(const string& str, Exchange::SessionResult defaultValue) {
            if (str == "success")        return Exchange::SessionResult::SUCCESS;
            if (str == "error")          return Exchange::SessionResult::ERROR;
            if (str == "abort")          return Exchange::SessionResult::ABORT;
            if (str == "shortUtterance") return Exchange::SessionResult::SHORT_UTTERANCE;
            return defaultValue;
        }

        const char* voiceSessionRequestTypeToString(const Exchange::VoiceSessionRequestType type)
        {
            switch (type) {
            case Exchange::VoiceSessionRequestType::PTT_TRANSCRIPTION:
                return "ptt_transcription";
            case Exchange::VoiceSessionRequestType::PTT_AUDIO_FILE:
                return "ptt_audio_file";
            case Exchange::VoiceSessionRequestType::FF_TRANSCRIPTION:
                return "ff_transcription";
            case Exchange::VoiceSessionRequestType::MIC_TRANSCRIPTION:
                return "mic_transcription";
            case Exchange::VoiceSessionRequestType::MIC_AUDIO_FILE:
                return "mic_audio_file";
            case Exchange::VoiceSessionRequestType::MIC_STREAM_DEFAULT:
                return "mic_stream_default";
            case Exchange::VoiceSessionRequestType::MIC_STREAM_SINGLE:
                return "mic_stream_single";
            case Exchange::VoiceSessionRequestType::MIC_STREAM_MULTI:
                return "mic_stream_multi";
            case Exchange::VoiceSessionRequestType::MIC_TAP_STREAM_SINGLE:
                return "mic_tap_stream_single";
            case Exchange::VoiceSessionRequestType::MIC_TAP_STREAM_MULTI:
                return "mic_tap_stream_multi";
            case Exchange::VoiceSessionRequestType::MIC_FACTORY_TEST:
                return "mic_factory_test";
            default:
                return "ptt_transcription";
            }
        }

        bool tryParseJsonValue(const string& serialized, JsonValue& value)
        {
            if (serialized.empty()) {
                return false;
            }

            Core::OptionalType<Core::JSON::Error> error;
            value.FromString(serialized, error);

            return (error.IsSet() == false);
        }

        string jsonValueToString(const JsonValue& value)
        {
            // Variant stores the pre-serialized representation in Value(), which
            // String() returns directly: raw unquoted content for STRING, and the
            // already-serialized JSON form ({...}, [...], 42, true) for all other
            // types. Avoids Variant::ToString(string&) which is declared inline in
            // this Thunder version but has no reachable definition.
            return value.String();
        }
    } // anonymous namespace

    SERVICE_REGISTRATION(VoiceControlImplementation, API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH);

    VoiceControlImplementation* VoiceControlImplementation::_instance = nullptr;

    VoiceControlImplementation::VoiceControlImplementation()
        : _adminLock()
        , _service(nullptr)
        , _notifications()
        , _hasOwnProcess(false)
        , _handlersRegistered(0)
        , _maskPii(true)  // Defaults to 'true' as Configure() will load the real value
    {
        _instance = this;
    }

    VoiceControlImplementation::~VoiceControlImplementation()
    {
        DeinitializeIARM();
        _instance = nullptr;

        // Release any remaining notification observers to avoid leaking references
        _adminLock.Lock();
        for (auto* notification : _notifications) {
            if (notification != nullptr) {
                notification->Release();
            }
        }
        _notifications.clear();
        _adminLock.Unlock();

        if (_service != nullptr) {
            _service->Release();
            _service = nullptr;
        }
    }

    Core::hresult VoiceControlImplementation::Configure(PluginHost::IShell* service)
    {
        LOGINFO("Configuring VoiceControlImplementation");
        ASSERT(service != nullptr);
        ASSERT(_service == nullptr);
        _service = service;
        _service->AddRef();

        if (InitializeIARM() == false) {
            LOGERR("Failed to initialize IARM for VoiceControlImplementation, configuration will fail");
            _service->Release();
            _service = nullptr;
            return Core::ERROR_GENERAL;
        }
        if (Utils::IARM::isConnected() == false) {
            LOGERR("Failed to initialize IARM for VoiceControlImplementation, configuration will fail");
            DeinitializeIARM();
            _service->Release();
            _service = nullptr;
            return Core::ERROR_GENERAL;
        }

        // Query the initial maskPii setting from the voice status; default to true (mask PII) if absent.
        JsonObject statusResult;
        Core::hresult result = IARMBusCall(CTRLM_VOICE_IARM_CALL_STATUS, "{}", statusResult);
        if (result == Core::ERROR_NONE) {
            _maskPii = statusResult.HasLabel("maskPii") ? statusResult["maskPii"].Boolean() : true;
            LOGINFO("Mask pii set to %s.", (_maskPii ? "True" : "False"));
        } else {
            _maskPii = true;
            LOGERR("Failed to query initial voice status, defaulting maskPii to true. Error: %d", result);
        }

        return Core::ERROR_NONE;
    }

    // ─── INotification management ───

    Core::hresult VoiceControlImplementation::Register(Exchange::IVoiceControl::INotification* notification)
    {
        ASSERT(notification != nullptr);

        _adminLock.Lock();
        auto it = std::find(_notifications.begin(), _notifications.end(), notification);
        if (it == _notifications.end()) {
            notification->AddRef();
            _notifications.push_back(notification);
        }
        _adminLock.Unlock();
        return Core::ERROR_NONE;
    }

    Core::hresult VoiceControlImplementation::Unregister(const Exchange::IVoiceControl::INotification* notification)
    {
        ASSERT(notification != nullptr);

        _adminLock.Lock();
        auto it = std::find(_notifications.begin(), _notifications.end(), notification);
        if (it != _notifications.end()) {
            (*it)->Release();
            _notifications.erase(it);
        }
        _adminLock.Unlock();
        return Core::ERROR_NONE;
    }

    // ─── IARM lifecycle ───

    bool VoiceControlImplementation::InitializeIARM()
    {
        bool alreadyConnected = Utils::IARM::isConnected();
        if (Utils::IARM::init()) {
            _hasOwnProcess = !alreadyConnected;
            IARM_Result_t res;
#define VC_REGISTER(EVENT) \
            IARM_CHECK( IARM_Bus_RegisterEventHandler(CTRLM_MAIN_IARM_BUS_NAME, EVENT, voiceEventHandler) ); \
            if (res != IARM_RESULT_SUCCESS) { \
                LOGERR("Failed to register IARM event handler for " #EVENT ", rolling back."); \
                DeinitializeIARM(); \
                return false; \
            } \
            _handlersRegistered++;
            VC_REGISTER(CTRLM_VOICE_IARM_EVENT_JSON_SESSION_BEGIN)
            VC_REGISTER(CTRLM_VOICE_IARM_EVENT_JSON_STREAM_BEGIN)
            VC_REGISTER(CTRLM_VOICE_IARM_EVENT_JSON_KEYWORD_VERIFICATION)
            VC_REGISTER(CTRLM_VOICE_IARM_EVENT_JSON_SERVER_MESSAGE)
            VC_REGISTER(CTRLM_VOICE_IARM_EVENT_JSON_STREAM_END)
            VC_REGISTER(CTRLM_VOICE_IARM_EVENT_JSON_SESSION_END)
#undef VC_REGISTER
        } else {
            _hasOwnProcess = false;
            return false;
        }
        return true;
    }

    void VoiceControlImplementation::DeinitializeIARM()
    {
        if (_handlersRegistered > 0) {
            // Handlers are registered in order; remove only those that were successfully registered, in reverse.
            // The registration order is:
            //   1: CTRLM_VOICE_IARM_EVENT_JSON_SESSION_BEGIN
            //   2: CTRLM_VOICE_IARM_EVENT_JSON_STREAM_BEGIN
            //   3: CTRLM_VOICE_IARM_EVENT_JSON_KEYWORD_VERIFICATION
            //   4: CTRLM_VOICE_IARM_EVENT_JSON_SERVER_MESSAGE
            //   5: CTRLM_VOICE_IARM_EVENT_JSON_STREAM_END
            //   6: CTRLM_VOICE_IARM_EVENT_JSON_SESSION_END
            IARM_Result_t res;
            if (_handlersRegistered >= 6) { IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_SESSION_END,          voiceEventHandler) ); }
            if (_handlersRegistered >= 5) { IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_STREAM_END,           voiceEventHandler) ); }
            if (_handlersRegistered >= 4) { IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_SERVER_MESSAGE,       voiceEventHandler) ); }
            if (_handlersRegistered >= 3) { IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_KEYWORD_VERIFICATION, voiceEventHandler) ); }
            if (_handlersRegistered >= 2) { IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_STREAM_BEGIN,         voiceEventHandler) ); }
            if (_handlersRegistered >= 1) { IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_SESSION_BEGIN,        voiceEventHandler) ); }
            _handlersRegistered = 0;
        }

        if (_hasOwnProcess) {
            IARM_Result_t res;
            IARM_CHECK( IARM_Bus_Disconnect() );
            IARM_CHECK( IARM_Bus_Term() );
            _hasOwnProcess = false;
        }
    }

    // ─── IARM event dispatching ───

    void VoiceControlImplementation::voiceEventHandler(const char* owner, IARM_EventId_t eventId, void* data, size_t len)
    {
        if (_instance != nullptr) {
            _instance->iarmEventHandler(owner, eventId, data, len);
        } else {
            LOGWARN("WARNING - cannot handle IARM events without a VoiceControlImplementation instance!");
        }
    }

    void VoiceControlImplementation::iarmEventHandler(const char* owner, IARM_EventId_t eventId, void* data, size_t len)
    {
        if (strcmp(owner, CTRLM_MAIN_IARM_BUS_NAME)) {
            LOGERR("ERROR - unexpected event: owner %s, eventId: %u, data: %p, size: %zu.", owner, (unsigned)eventId, data, len);
            return;
        }

        if (data == nullptr || len == 0 || len <= sizeof(ctrlm_voice_iarm_event_json_t)) {
            LOGERR("ERROR - got eventId(%u) with INVALID DATA: data: %p, len: %zu.", (unsigned)eventId, data, len);
            return;
        }

        // Ensure there is a null character at the end of the data area.
        char* str = (char*)data;
        str[len - 1] = '\0';

        ctrlm_voice_iarm_event_json_t* eventData = static_cast<ctrlm_voice_iarm_event_json_t*>(data);

        if (CTRLM_VOICE_IARM_BUS_API_REVISION != eventData->api_revision) {
            LOGERR("ERROR - got eventId(%u) with wrong VOICE IARM API revision - should be %d, event has %d.",
                   (unsigned)eventId, CTRLM_VOICE_IARM_BUS_API_REVISION, (int)eventData->api_revision);
            return;
        }

        switch (eventId) {
            case CTRLM_VOICE_IARM_EVENT_JSON_SESSION_BEGIN:
                LOGWARN("Got CTRLM_VOICE_IARM_EVENT_JSON_SESSION_BEGIN event.");
                NotifySessionBegin(eventData);
                break;
            case CTRLM_VOICE_IARM_EVENT_JSON_STREAM_BEGIN:
                LOGWARN("Got CTRLM_VOICE_IARM_EVENT_JSON_STREAM_BEGIN event.");
                NotifyStreamBegin(eventData);
                break;
            case CTRLM_VOICE_IARM_EVENT_JSON_KEYWORD_VERIFICATION:
                LOGWARN("Got CTRLM_VOICE_IARM_EVENT_JSON_KEYWORD_VERIFICATION event.");
                NotifyKeywordVerification(eventData);
                break;
            case CTRLM_VOICE_IARM_EVENT_JSON_SERVER_MESSAGE:
                LOGWARN("Got CTRLM_VOICE_IARM_EVENT_JSON_SERVER_MESSAGE event.");
                NotifyServerMessage(eventData);
                break;
            case CTRLM_VOICE_IARM_EVENT_JSON_STREAM_END:
                LOGWARN("Got CTRLM_VOICE_IARM_EVENT_JSON_STREAM_END event.");
                NotifyStreamEnd(eventData);
                break;
            case CTRLM_VOICE_IARM_EVENT_JSON_SESSION_END:
                LOGWARN("Got CTRLM_VOICE_IARM_EVENT_JSON_SESSION_END event.");
                NotifySessionEnd(eventData);
                break;
            default:
                LOGERR("ERROR - unexpected ControlMgr event: eventId: %u, data: %p, size: %zu.",
                       (unsigned)eventId, data, len);
                break;
        }
    }

    // ─── Event notifications to observers ───

    std::vector<Exchange::IVoiceControl::INotification*> VoiceControlImplementation::ObserverSnapshot()
    {
        std::vector<Exchange::IVoiceControl::INotification*> observers;

        _adminLock.Lock();
        for (auto* notification : _notifications) {
            notification->AddRef();
            observers.push_back(notification);
        }
        _adminLock.Unlock();

        return observers;
    }

    void VoiceControlImplementation::ReleaseObserverSnapshot(std::vector<Exchange::IVoiceControl::INotification*>& observers)
    {
        for (auto* notification : observers) {
            notification->Release();
        }
        observers.clear();
    }

    void VoiceControlImplementation::NotifySessionBegin(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);
        LOGINFO("Notify onSessionBegin %s", eventData->payload);

        Exchange::SessionBeginEvent event;
        event.remoteId = params.HasLabel("remoteId") ? static_cast<uint32_t>(params["remoteId"].Number()) : 0;
        event.sessionId = params.HasLabel("sessionId") ? params["sessionId"].String() : "";
        event.deviceType = params.HasLabel("deviceType") ? stringToEnum<Exchange::DeviceType>(params["deviceType"].String(), Exchange::DeviceType::PTT) : Exchange::DeviceType::PTT;
        event.keywordVerification = params.HasLabel("keywordVerification") ? params["keywordVerification"].Boolean() : false;

        auto observers = ObserverSnapshot();

        for (auto* notification : observers) {
            notification->OnSessionBegin(event);
        }

        ReleaseObserverSnapshot(observers);
    }

    void VoiceControlImplementation::NotifyStreamBegin(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);
        LOGINFO("Notify onStreamBegin %s", eventData->payload);

        Exchange::StreamBeginEvent event;
        event.remoteId = params.HasLabel("remoteId") ? static_cast<uint32_t>(params["remoteId"].Number()) : 0;
        event.sessionId = params.HasLabel("sessionId") ? params["sessionId"].String() : "";

        auto observers = ObserverSnapshot();

        for (auto* notification : observers) {
            notification->OnStreamBegin(event);
        }

        ReleaseObserverSnapshot(observers);
    }

    void VoiceControlImplementation::NotifyKeywordVerification(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);
        LOGINFO("Notify onKeywordVerification %s", eventData->payload);

        Exchange::KeywordVerificationEvent event;
        event.remoteId = params.HasLabel("remoteId") ? static_cast<uint32_t>(params["remoteId"].Number()) : 0;
        event.sessionId = params.HasLabel("sessionId") ? params["sessionId"].String() : "";
        event.verified = params.HasLabel("verified") ? params["verified"].Boolean() : false;

        auto observers = ObserverSnapshot();

        for (auto* notification : observers) {
            notification->OnKeywordVerification(event);
        }

        ReleaseObserverSnapshot(observers);
    }

    void VoiceControlImplementation::NotifyServerMessage(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);
        LOGINFO("Notify onServerMessage %s", _maskPii ? "<***>" : eventData->payload);

        Exchange::ServerMessageEvent event;
        event.msgType = params.HasLabel("msgType") ? params["msgType"].String() : "";
        event.trx = params.HasLabel("trx") ? params["trx"].String() : "";
        event.created = params.HasLabel("created") ? static_cast<uint64_t>(params["created"].Number()) : 0;
        event.msgPayload = params.HasLabel("msgPayload") ? jsonValueToString(params["msgPayload"]) : "";
        if (_maskPii) {
            // Redact payload when PII masking is enabled to avoid exposing sensitive data to observers.
            event.msgPayload.clear();
        }

        auto observers = ObserverSnapshot();

        for (auto* notification : observers) {
            notification->OnServerMessage(event);
        }

        ReleaseObserverSnapshot(observers);
    }

    void VoiceControlImplementation::NotifyStreamEnd(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);
        LOGINFO("Notify onStreamEnd %s", eventData->payload);

        Exchange::StreamEndEvent event;
        event.remoteId = params.HasLabel("remoteId") ? static_cast<uint32_t>(params["remoteId"].Number()) : 0;
        event.sessionId = params.HasLabel("sessionId") ? params["sessionId"].String() : "";
        event.reason = params.HasLabel("reason") ? static_cast<uint8_t>(params["reason"].Number()) : 0;

        auto observers = ObserverSnapshot();

        for (auto* notification : observers) {
            notification->OnStreamEnd(event);
        }

        ReleaseObserverSnapshot(observers);
    }

    void VoiceControlImplementation::NotifySessionEnd(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);
        LOGINFO("Notify onSessionEnd %s", _maskPii ? "<***>" : eventData->payload);

        Exchange::SessionEndEvent event;
        event.remoteId = params.HasLabel("remoteId") ? static_cast<uint32_t>(params["remoteId"].Number()) : 0;
        event.sessionId = params.HasLabel("sessionId") ? params["sessionId"].String() : "";
        event.result = params.HasLabel("result") ? stringToEnum<Exchange::SessionResult>(params["result"].String(), Exchange::SessionResult::ERROR) : Exchange::SessionResult::ERROR;

        if (params.HasLabel("serverStats")) {
            JsonObject statsObj = params["serverStats"].Object();
            event.serverStats.dnsTime = statsObj.HasLabel("dnsTime") ? statsObj["dnsTime"].Double() : 0.0;
            // When PII masking is enabled, avoid propagating server IP to observers.
            event.serverStats.serverIp = (!_maskPii && statsObj.HasLabel("serverIp")) ? statsObj["serverIp"].String() : "";
            event.serverStats.connectTime = statsObj.HasLabel("connectTime") ? statsObj["connectTime"].Double() : 0.0;
        }

        auto observers = ObserverSnapshot();

        for (auto* notification : observers) {
            notification->OnSessionEnd(event);
        }

        ReleaseObserverSnapshot(observers);
    }

    // ─── IARM call helper ───

    Core::hresult VoiceControlImplementation::IARMBusCall(const string& method, const string& jsonParams, JsonObject& result)
    {
        const size_t totalsize = sizeof(ctrlm_voice_iarm_call_json_t) + jsonParams.size() + 1;
        ctrlm_voice_iarm_call_json_t* call = (ctrlm_voice_iarm_call_json_t*)calloc(1, totalsize);

        if (call == nullptr) {
            LOGERR("ERROR - Cannot allocate IARM structure - size: %u.", (unsigned)totalsize);
            return Core::ERROR_GENERAL;
        }

        call->api_revision = CTRLM_VOICE_IARM_BUS_API_REVISION;
        const size_t len = jsonParams.copy(call->payload, jsonParams.size());
        call->payload[len] = '\0';

        IARM_Result_t res = IARM_Bus_Call(CTRLM_MAIN_IARM_BUS_NAME, method.c_str(), (void*)call, totalsize);
        if (res != IARM_RESULT_SUCCESS) {
            LOGERR("ERROR - %s Bus Call FAILED, res: %d.", method.c_str(), (int)res);
            free(call);
            return Core::ERROR_RPC_CALL_FAILED;
        }

        result.FromString(call->result);
        free(call);
        return Core::ERROR_NONE;
    }

    // ─── IVoiceControl method implementations ───

    Core::hresult VoiceControlImplementation::GetApiVersionNumber(Exchange::GetApiVersionNumberResponse& response)
    {
        response.version = API_VERSION_NUMBER_MAJOR;
        response.success = true;
        return Core::ERROR_NONE;
    }

    Core::hresult VoiceControlImplementation::SendNotify_(const string& /* eventName */, string& /* parameters */)
    {
        // This method is omitted from JSON-RPC (@json:omit) and not used in the OOP architecture.
        return Core::ERROR_NONE;
    }

    Core::hresult VoiceControlImplementation::GetVoiceStatus(Exchange::VoiceStatusResponse& response, Exchange::IStringIterator*& capabilities)
    {
        capabilities = nullptr;

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_STATUS, "{}", result);
        if (callResult != Core::ERROR_NONE) {
            response.maskPii = _maskPii.load();
            response.urlPtt.clear();
            response.urlHf.clear();
            response.urlMicTap.clear();
            response.prv = false;
            response.wwFeedback = false;
            response.ptt.status.clear();
            response.ff.status.clear();
            response.mic.status.clear();
            response.mic_tap.status.clear();
            response.success = false;
            return Core::ERROR_NONE;
        }

        response.maskPii = result.HasLabel("maskPii") ? result["maskPii"].Boolean() : _maskPii.load();
        _maskPii = response.maskPii;

        response.urlPtt = result.HasLabel("urlPtt") ? result["urlPtt"].String() : "";
        response.urlHf = result.HasLabel("urlHf") ? result["urlHf"].String() : "";
        response.urlMicTap = result.HasLabel("urlMicTap") ? result["urlMicTap"].String() : "";
        response.prv = result.HasLabel("prv") ? result["prv"].Boolean() : false;
        response.wwFeedback = result.HasLabel("wwFeedback") ? result["wwFeedback"].Boolean() : false;

        const auto populateDeviceStatus = [&result](const char label[], Exchange::DeviceStatus& deviceStatus) {
            deviceStatus.status.clear();

            if (result.HasLabel(label)) {
                JsonObject deviceObj = result[label].Object();
                deviceStatus.status = deviceObj.HasLabel("status") ? deviceObj["status"].String() : "";
            }
        };

        populateDeviceStatus("ptt", response.ptt);
        populateDeviceStatus("ff", response.ff);
        populateDeviceStatus("mic", response.mic);
        populateDeviceStatus("mic_tap", response.mic_tap);
        response.success = result.HasLabel("success") ? result["success"].Boolean() : false;

        std::list<string> capabilityList;
        if (result.HasLabel("capabilities")) {
            auto capabilityArray = result["capabilities"].Array();
            for (uint16_t i = 0; i < capabilityArray.Length(); i++) {
                capabilityList.push_back(capabilityArray[i].String());
            }
        }
        capabilities = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(capabilityList);

        return Core::ERROR_NONE;
    }

    Core::hresult VoiceControlImplementation::ConfigureVoice(const string& urlAll, const string& urlPtt, const string& urlHf, const string& urlMicTap, const bool enable, const bool prv, const bool wwFeedback, const Exchange::DeviceSettings& ptt, const Exchange::DeviceSettings& ff, const Exchange::DeviceSettings& mic, Exchange::SuccessResult& result)
    {
        JsonObject params;
        params["urlAll"] = urlAll;
        params["urlPtt"] = urlPtt;
        params["urlHf"] = urlHf;
        params["urlMicTap"] = urlMicTap;
        params["enable"] = enable;
        params["prv"] = prv;
        params["wwFeedback"] = wwFeedback;

        JsonObject pttObj;
        pttObj["enable"] = ptt.enable;
        params["ptt"] = pttObj;

        JsonObject ffObj;
        ffObj["enable"] = ff.enable;
        params["ff"] = ffObj;

        JsonObject micObj;
        micObj["enable"] = mic.enable;
        params["mic"] = micObj;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_CONFIGURE_VOICE, jsonParams, iarmResult);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            return Core::ERROR_NONE;
        }

        result.success = iarmResult.HasLabel("success") ? iarmResult["success"].Boolean() : false;
        return Core::ERROR_NONE;
    }

    Core::hresult VoiceControlImplementation::SetVoiceInit(const string& language, Exchange::IStringIterator* const capabilities, Exchange::SuccessResult& result)
    {
        JsonObject params;
        params["language"] = language;

        if (capabilities != nullptr) {
            JsonArray capArray;
            string cap;
            while (capabilities->Next(cap)) {
                capArray.Add(Core::JSON::Variant(cap));
            }
            params["capabilities"] = capArray;
        }

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_SET_VOICE_INIT, jsonParams, iarmResult);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            return Core::ERROR_NONE;
        }

        result.success = iarmResult.HasLabel("success") ? iarmResult["success"].Boolean() : false;
        return Core::ERROR_NONE;
    }

    Core::hresult VoiceControlImplementation::SendVoiceMessage(const string& msgType, const string& trx, const uint64_t created, const string& msgPayload, Exchange::SuccessResult& result)
    {
        JsonObject params;
        params["msgType"] = msgType;
        if (!trx.empty()) {
            params["trx"] = trx;
        }
        if (created != 0) {
            // created is a uint64_t (Unix timestamp in ms). Casting to double preserves full precision for all realistic timestamps (~1.7e12 ms today, well below the 2^53 limit).
            params["created"] = static_cast<double>(created);
        }
        if (!msgPayload.empty()) {
            JsonValue payload;
            if (tryParseJsonValue(msgPayload, payload) == true) {
                params["msgPayload"] = std::move(payload);
            } else {
                params["msgPayload"] = msgPayload;
            }
        }

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_SEND_VOICE_MESSAGE, jsonParams, iarmResult);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            return Core::ERROR_NONE;
        }

        result.success = iarmResult.HasLabel("success") ? iarmResult["success"].Boolean() : false;
        return Core::ERROR_NONE;
    }

    Core::hresult VoiceControlImplementation::VoiceSessionByText(const string& transcription, const Exchange::DeviceType type, Exchange::SuccessResult& result)
    {
        // Translate the deprecated API to voiceSessionRequest
        string translatedAudioFile;
        Exchange::VoiceSessionRequestType translatedType;

        switch (type) {
            case Exchange::DeviceType::PTT:
                translatedType = Exchange::VoiceSessionRequestType::PTT_TRANSCRIPTION;
                break;
            case Exchange::DeviceType::FF:
                translatedType = Exchange::VoiceSessionRequestType::FF_TRANSCRIPTION;
                break;
            case Exchange::DeviceType::MIC:
                translatedType = Exchange::VoiceSessionRequestType::MIC_TRANSCRIPTION;
                break;
            default:
                translatedType = Exchange::VoiceSessionRequestType::PTT_TRANSCRIPTION;
                break;
        }

        return VoiceSessionRequest(transcription, translatedAudioFile, translatedType, result);
    }

    Core::hresult VoiceControlImplementation::GetVoiceSessionTypes(bool& success, Exchange::IStringIterator*& types)
    {
        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_SESSION_TYPES, "{}", result);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return Core::ERROR_NONE;
        }

        success = result.HasLabel("success") ? result["success"].Boolean() : false;

        std::list<string> typeList;
        if (result.HasLabel("types")) {
            auto arr = result["types"].Array();
            for (uint16_t i = 0; i < arr.Length(); i++) {
                typeList.push_back(arr[i].String());
            }
        }
        types = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(typeList);

        return Core::ERROR_NONE;
    }

    Core::hresult VoiceControlImplementation::VoiceSessionRequest(const string& transcription, const string& audioFile, const Exchange::VoiceSessionRequestType type, Exchange::SuccessResult& result)
    {
        JsonObject params;
        params["type"] = voiceSessionRequestTypeToString(type);
        if (!transcription.empty()) {
            params["transcription"] = transcription;
        }
        if (!audioFile.empty()) {
            params["audioFile"] = audioFile;
        }

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_SESSION_REQUEST, jsonParams, iarmResult);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            return Core::ERROR_NONE;
        }

        result.success = iarmResult.HasLabel("success") ? iarmResult["success"].Boolean() : false;
        return Core::ERROR_NONE;
    }

    Core::hresult VoiceControlImplementation::VoiceSessionTerminate(const string& sessionId, Exchange::SuccessResult& result)
    {
        JsonObject params;
        params["sessionId"] = sessionId;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_SESSION_TERMINATE, jsonParams, iarmResult);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            return Core::ERROR_NONE;
        }

        result.success = iarmResult.HasLabel("success") ? iarmResult["success"].Boolean() : false;
        return Core::ERROR_NONE;
    }

    Core::hresult VoiceControlImplementation::VoiceSessionAudioStreamStart(const string& sessionId, Exchange::SuccessResult& result)
    {
        JsonObject params;
        params["sessionId"] = sessionId;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_SESSION_AUDIO_STREAM_START, jsonParams, iarmResult);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            return Core::ERROR_NONE;
        }

        result.success = iarmResult.HasLabel("success") ? iarmResult["success"].Boolean() : false;
        return Core::ERROR_NONE;
    }

} // namespace Plugin
} // namespace WPEFramework
