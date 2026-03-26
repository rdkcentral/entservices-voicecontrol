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

    SERVICE_REGISTRATION(VoiceControlImplementation, API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH);

    VoiceControlImplementation* VoiceControlImplementation::_instance = nullptr;

    VoiceControlImplementation::VoiceControlImplementation()
        : _adminLock()
        , _service(nullptr)
        , _notifications()
        , _hasOwnProcess(false)
        , _handlersRegistered(false)
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

        // Query the initial maskPii setting from the voice status and setting to false if absent
        JsonObject statusResult;
        Core::hresult result = IARMBusCall(CTRLM_VOICE_IARM_CALL_STATUS, "{}", statusResult);
        if (result == Core::ERROR_NONE) {
            _maskPii = statusResult.HasLabel("maskPii") ? statusResult["maskPii"].Boolean() : false;
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
            _handlersRegistered = true;
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
        if (_handlersRegistered) {
            IARM_Result_t res;
            IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_SESSION_END,          voiceEventHandler) );
            IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_STREAM_END,           voiceEventHandler) );
            IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_KEYWORD_VERIFICATION, voiceEventHandler) );
            IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_SERVER_MESSAGE,       voiceEventHandler) );
            IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_STREAM_BEGIN,         voiceEventHandler) );
            IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_SESSION_BEGIN,        voiceEventHandler) );
            _handlersRegistered = false;
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

    void VoiceControlImplementation::NotifySessionBegin(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);
        LOGINFO("Notify onSessionBegin %s", eventData->payload);

        Exchange::SessionBeginEvent event;
        event.remoteId = params.HasLabel("remoteId") ? static_cast<uint32_t>(params["remoteId"].Number()) : 0;
        event.sessionId = params.HasLabel("sessionId") ? params["sessionId"].String() : "";
        event.deviceType = params.HasLabel("deviceType") ? static_cast<Exchange::DeviceType>(static_cast<uint8_t>(params["deviceType"].Number())) : Exchange::DeviceType::PTT;
        event.keywordVerification = params.HasLabel("keywordVerification") ? params["keywordVerification"].Boolean() : false;

        std::vector<Exchange::IVoiceControl::INotification*> observers;
        _adminLock.Lock();
        for (auto* notification : _notifications) {
            notification->AddRef();
            observers.push_back(notification);
        }
        _adminLock.Unlock();

        for (auto* notification : observers) {
            notification->OnSessionBegin(event);
            notification->Release();
        }
    }

    void VoiceControlImplementation::NotifyStreamBegin(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);
        LOGINFO("Notify onStreamBegin %s", eventData->payload);

        Exchange::StreamBeginEvent event;
        event.remoteId = params.HasLabel("remoteId") ? static_cast<uint32_t>(params["remoteId"].Number()) : 0;
        event.sessionId = params.HasLabel("sessionId") ? params["sessionId"].String() : "";

        std::vector<Exchange::IVoiceControl::INotification*> observers;
        _adminLock.Lock();
        for (auto* notification : _notifications) {
            notification->AddRef();
            observers.push_back(notification);
        }
        _adminLock.Unlock();

        for (auto* notification : observers) {
            notification->OnStreamBegin(event);
            notification->Release();
        }
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

        std::vector<Exchange::IVoiceControl::INotification*> observers;
        _adminLock.Lock();
        for (auto* notification : _notifications) {
            notification->AddRef();
            observers.push_back(notification);
        }
        _adminLock.Unlock();

        for (auto* notification : observers) {
            notification->OnKeywordVerification(event);
            notification->Release();
        }
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
        event.msgPayload = params.HasLabel("msgPayload") ? params["msgPayload"].String() : "";

        std::vector<Exchange::IVoiceControl::INotification*> observers;
        _adminLock.Lock();
        for (auto* notification : _notifications) {
            notification->AddRef();
            observers.push_back(notification);
        }
        _adminLock.Unlock();

        for (auto* notification : observers) {
            notification->OnServerMessage(event);
            notification->Release();
        }
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

        std::vector<Exchange::IVoiceControl::INotification*> observers;
        _adminLock.Lock();
        for (auto* notification : _notifications) {
            notification->AddRef();
            observers.push_back(notification);
        }
        _adminLock.Unlock();

        for (auto* notification : observers) {
            notification->OnStreamEnd(event);
            notification->Release();
        }
    }

    void VoiceControlImplementation::NotifySessionEnd(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);
        LOGINFO("Notify onSessionEnd %s", _maskPii ? "<***>" : eventData->payload);

        Exchange::SessionEndEvent event;
        event.remoteId = params.HasLabel("remoteId") ? static_cast<uint32_t>(params["remoteId"].Number()) : 0;
        event.sessionId = params.HasLabel("sessionId") ? params["sessionId"].String() : "";
        event.result = params.HasLabel("result") ? static_cast<Exchange::SessionResult>(static_cast<uint8_t>(params["result"].Number())) : Exchange::SessionResult::ERROR;

        if (params.HasLabel("serverStats")) {
            JsonObject statsObj = params["serverStats"].Object();
            event.serverStats.dnsTime = statsObj.HasLabel("dnsTime") ? statsObj["dnsTime"].Double() : 0.0;
            event.serverStats.serverIp = statsObj.HasLabel("serverIp") ? statsObj["serverIp"].String() : "";
            event.serverStats.connectTime = statsObj.HasLabel("connectTime") ? statsObj["connectTime"].Double() : 0.0;
        }

        std::vector<Exchange::IVoiceControl::INotification*> observers;
        _adminLock.Lock();
        for (auto* notification : _notifications) {
            notification->AddRef();
            observers.push_back(notification);
        }
        _adminLock.Unlock();

        for (auto* notification : observers) {
            notification->OnSessionEnd(event);
            notification->Release();
        }
    }

    // ─── IARM call helper ───

    Core::hresult VoiceControlImplementation::IARMBusCall(const string& method, const string& jsonParams, JsonObject& result)
    {
        size_t totalsize = sizeof(ctrlm_voice_iarm_call_json_t) + jsonParams.size() + 1;
        ctrlm_voice_iarm_call_json_t* call = (ctrlm_voice_iarm_call_json_t*)calloc(1, totalsize);

        if (call == nullptr) {
            LOGERR("ERROR - Cannot allocate IARM structure - size: %u.", (unsigned)totalsize);
            return Core::ERROR_GENERAL;
        }

        call->api_revision = CTRLM_VOICE_IARM_BUS_API_REVISION;
        size_t len = jsonParams.copy(call->payload, jsonParams.size());
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
        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_STATUS, "{}", result);
        if (callResult != Core::ERROR_NONE) {
            response.success = false;
            capabilities = nullptr;
            return callResult;
        }

        response.maskPii = result.HasLabel("maskPii") ? result["maskPii"].Boolean() : false;
        response.urlPtt = result.HasLabel("urlPtt") ? result["urlPtt"].String() : "";
        response.urlHf = result.HasLabel("urlHf") ? result["urlHf"].String() : "";
        response.prv = result.HasLabel("prv") ? result["prv"].Boolean() : false;
        response.wwFeedback = result.HasLabel("wwFeedback") ? result["wwFeedback"].Boolean() : false;
        response.success = result.HasLabel("success") ? result["success"].Boolean() : false;

        if (result.HasLabel("ptt")) {
            JsonObject pttObj = result["ptt"].Object();
            response.ptt.status = pttObj.HasLabel("status") ? pttObj["status"].String() : "";
        }
        if (result.HasLabel("ff")) {
            JsonObject ffObj = result["ff"].Object();
            response.ff.status = ffObj.HasLabel("status") ? ffObj["status"].String() : "";
        }
        if (result.HasLabel("mic")) {
            JsonObject micObj = result["mic"].Object();
            response.mic.status = micObj.HasLabel("status") ? micObj["status"].String() : "";
        }

        std::list<string> capList;
        if (result.HasLabel("capabilities")) {
            auto arr = result["capabilities"].Array();
            for (uint16_t i = 0; i < arr.Length(); i++) {
                capList.push_back(arr[i].String());
            }
        }
        capabilities = Core::Service<RPC::StringIterator>::Create<Exchange::IStringIterator>(capList);

        // Update internal maskPii state
        _maskPii = response.maskPii;

        return Core::ERROR_NONE;
    }

    Core::hresult VoiceControlImplementation::ConfigureVoice(const Exchange::ConfigureVoiceRequest& request, bool& success)
    {
        JsonObject params;
        params["urlAll"] = request.urlAll;
        params["urlPtt"] = request.urlPtt;
        params["urlHf"] = request.urlHf;
        params["urlMicTap"] = request.urlMicTap;
        params["enable"] = request.enable;
        params["prv"] = request.prv;
        params["wwFeedback"] = request.wwFeedback;

        JsonObject pttObj;
        pttObj["enable"] = request.ptt.enable;
        params["ptt"] = pttObj;

        JsonObject ffObj;
        ffObj["enable"] = request.ff.enable;
        params["ff"] = ffObj;

        JsonObject micObj;
        micObj["enable"] = request.mic.enable;
        params["mic"] = micObj;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_CONFIGURE_VOICE, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return callResult;
        }

        success = result.HasLabel("success") ? result["success"].Boolean() : false;
        return success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult VoiceControlImplementation::SetVoiceInit(const string& language, Exchange::IStringIterator* const capabilities, bool& success)
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

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_SET_VOICE_INIT, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return callResult;
        }

        success = result.HasLabel("success") ? result["success"].Boolean() : false;
        return success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult VoiceControlImplementation::SendVoiceMessage(const Exchange::ServerMessageEvent& request, bool& success)
    {
        JsonObject params;
        params["msgType"] = request.msgType;
        params["trx"] = request.trx;
        // created is a uint64_t (Unix timestamp in ms). Casting to double preserves full precision for all realistic timestamps (~1.7e12 ms today, well below the 2^53 limit).
        params["created"] = static_cast<double>(request.created);
        params["msgPayload"] = request.msgPayload;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_SEND_VOICE_MESSAGE, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return callResult;
        }

        success = result.HasLabel("success") ? result["success"].Boolean() : false;
        return success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult VoiceControlImplementation::VoiceSessionByText(const Exchange::VoiceSessionByTextRequest& request, bool& success)
    {
        // Translate the deprecated API to voiceSessionRequest
        Exchange::VoiceSessionRequestData translated;
        translated.transcription = request.transcription;

        switch (request.type) {
            case Exchange::DeviceType::PTT:
                translated.type = "ptt_transcription";
                break;
            case Exchange::DeviceType::FF:
                translated.type = "ff_transcription";
                break;
            case Exchange::DeviceType::MIC:
                translated.type = "mic_transcription";
                break;
            default:
                translated.type = "ptt_transcription";
                break;
        }

        return VoiceSessionRequest(translated, success);
    }

    Core::hresult VoiceControlImplementation::GetVoiceSessionTypes(bool& success, Exchange::IStringIterator*& types)
    {
        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_SESSION_TYPES, "{}", result);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return callResult;
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

    Core::hresult VoiceControlImplementation::VoiceSessionRequest(const Exchange::VoiceSessionRequestData& request, bool& success)
    {
        JsonObject params;
        params["type"] = request.type;
        if (!request.transcription.empty()) {
            params["transcription"] = request.transcription;
        }
        if (!request.audioFile.empty()) {
            params["audioFile"] = request.audioFile;
        }

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_SESSION_REQUEST, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return callResult;
        }

        success = result.HasLabel("success") ? result["success"].Boolean() : false;
        return success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult VoiceControlImplementation::VoiceSessionTerminate(const Exchange::VoiceSessionTerminateRequest& request, bool& success)
    {
        JsonObject params;
        params["sessionId"] = request.sessionId;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_SESSION_TERMINATE, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return callResult;
        }

        success = result.HasLabel("success") ? result["success"].Boolean() : false;
        return success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

    Core::hresult VoiceControlImplementation::VoiceSessionAudioStreamStart(const Exchange::VoiceSessionTerminateRequest& request, bool& success)
    {
        JsonObject params;
        params["sessionId"] = request.sessionId;

        string jsonParams;
        params.ToString(jsonParams);

        JsonObject result;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_SESSION_AUDIO_STREAM_START, jsonParams, result);
        if (callResult != Core::ERROR_NONE) {
            success = false;
            return callResult;
        }

        success = result.HasLabel("success") ? result["success"].Boolean() : false;
        return success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
    }

} // namespace Plugin
} // namespace WPEFramework
