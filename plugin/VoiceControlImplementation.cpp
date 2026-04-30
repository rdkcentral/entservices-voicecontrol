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

        // --- DeviceType: ctrlm sends lowercase "ptt"/"ff"/"mic" ---
        template <>
        Exchange::DeviceType stringToEnum<Exchange::DeviceType>(const string& str, Exchange::DeviceType defaultValue) {
            if (str == "ptt") return Exchange::DeviceType::PTT;
            if (str == "ff")  return Exchange::DeviceType::FF;
            if (str == "mic") return Exchange::DeviceType::MIC;
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

        const char* deviceTypeToString(const Exchange::DeviceType type)
        {
            switch (type) {
            case Exchange::DeviceType::PTT: return "ptt";
            case Exchange::DeviceType::FF:  return "ff";
            case Exchange::DeviceType::MIC: return "mic";
            default:                        return "ptt";
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

    Core::CriticalSection VoiceControlImplementation::_instanceLock;
    VoiceControlImplementation* VoiceControlImplementation::_instance = nullptr;

    VoiceControlImplementation::VoiceControlImplementation()
        : _adminLock()
        , _service(nullptr)
        , _notifications()
        , _hasOwnProcess(false)
        , _handlersRegistered(0)
        , _maskPii(true)  // Defaults to 'true' as Configure() will load the real value
    {
        _instanceLock.Lock();
        _instance = this;
        _instanceLock.Unlock();
    }

    VoiceControlImplementation::~VoiceControlImplementation()
    {
        _instanceLock.Lock();
        _instance = nullptr;
        _instanceLock.Unlock();

        DeinitializeIARM();

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
        if (notification == nullptr) {
            return Core::ERROR_BAD_REQUEST;
        }

        _adminLock.Lock();
        const auto it = std::find(_notifications.begin(), _notifications.end(), notification);
        if (it == _notifications.end()) {
            notification->AddRef();
            _notifications.push_back(notification);
            LOGINFO("[VCDiag] Register INotification observer: totalObservers=%zu", _notifications.size());
        }
        _adminLock.Unlock();
        return Core::ERROR_NONE;
    }

    Core::hresult VoiceControlImplementation::Unregister(const Exchange::IVoiceControl::INotification* notification)
    {
        if (notification == nullptr) {
            return Core::ERROR_BAD_REQUEST;
        }

        _adminLock.Lock();
        const auto it = std::find(_notifications.begin(), _notifications.end(), notification);
        if (it != _notifications.end()) {
            (*it)->Release();
            _notifications.erase(it);
            LOGINFO("[VCDiag] Unregister INotification observer: totalObservers=%zu", _notifications.size());
        }
        _adminLock.Unlock();
        return Core::ERROR_NONE;
    }

    // ─── IARM lifecycle ───

    bool VoiceControlImplementation::InitializeIARM()
    {
        const bool alreadyConnected = Utils::IARM::isConnected();
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
        _instanceLock.Lock();
        VoiceControlImplementation* instance = _instance;
        if (instance != nullptr) {
            instance->iarmEventHandler(owner, eventId, data, len);
        } else {
            LOGWARN("WARNING - cannot handle IARM events without a VoiceControlImplementation instance!");
        }
        _instanceLock.Unlock();
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
                LOGINFO("Got CTRLM_VOICE_IARM_EVENT_JSON_SESSION_BEGIN event.");
                NotifySessionBegin(eventData);
                break;
            case CTRLM_VOICE_IARM_EVENT_JSON_STREAM_BEGIN:
                LOGINFO("Got CTRLM_VOICE_IARM_EVENT_JSON_STREAM_BEGIN event.");
                NotifyStreamBegin(eventData);
                break;
            case CTRLM_VOICE_IARM_EVENT_JSON_KEYWORD_VERIFICATION:
                LOGINFO("Got CTRLM_VOICE_IARM_EVENT_JSON_KEYWORD_VERIFICATION event.");
                NotifyKeywordVerification(eventData);
                break;
            case CTRLM_VOICE_IARM_EVENT_JSON_SERVER_MESSAGE:
                LOGINFO("Got CTRLM_VOICE_IARM_EVENT_JSON_SERVER_MESSAGE event.");
                NotifyServerMessage(eventData);
                break;
            case CTRLM_VOICE_IARM_EVENT_JSON_STREAM_END:
                LOGINFO("Got CTRLM_VOICE_IARM_EVENT_JSON_STREAM_END event.");
                NotifyStreamEnd(eventData);
                break;
            case CTRLM_VOICE_IARM_EVENT_JSON_SESSION_END:
                LOGINFO("Got CTRLM_VOICE_IARM_EVENT_JSON_SESSION_END event.");
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

        const uint32_t remoteId = params.HasLabel("remoteId") ? static_cast<uint32_t>(params["remoteId"].Number()) : 0;
        const string sessionId = params.HasLabel("sessionId") ? params["sessionId"].String() : "";
        const Exchange::DeviceType deviceType = params.HasLabel("deviceType") ? stringToEnum<Exchange::DeviceType>(params["deviceType"].String(), Exchange::DeviceType::PTT) : Exchange::DeviceType::PTT;
        const bool keywordVerification = params.HasLabel("keywordVerification") ? params["keywordVerification"].Boolean() : false;

        auto observers = ObserverSnapshot();
        LOGINFO("[VCDiag] NotifySessionBegin: observerCount=%zu remoteId=%u sessionId=%s deviceType=%d",
                observers.size(), remoteId, sessionId.c_str(), static_cast<int>(deviceType));

        for (auto* notification : observers) {
            notification->OnSessionBegin(remoteId, sessionId, deviceType, keywordVerification);
        }

        ReleaseObserverSnapshot(observers);
    }

    void VoiceControlImplementation::NotifyStreamBegin(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);
        LOGINFO("Notify onStreamBegin %s", eventData->payload);

        const uint32_t remoteId = params.HasLabel("remoteId") ? static_cast<uint32_t>(params["remoteId"].Number()) : 0;
        const string sessionId = params.HasLabel("sessionId") ? params["sessionId"].String() : "";

        auto observers = ObserverSnapshot();

        for (auto* notification : observers) {
            notification->OnStreamBegin(remoteId, sessionId);
        }

        ReleaseObserverSnapshot(observers);
    }

    void VoiceControlImplementation::NotifyKeywordVerification(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);
        LOGINFO("Notify onKeywordVerification %s", eventData->payload);

        const uint32_t remoteId = params.HasLabel("remoteId") ? static_cast<uint32_t>(params["remoteId"].Number()) : 0;
        const string sessionId = params.HasLabel("sessionId") ? params["sessionId"].String() : "";
        const bool verified = params.HasLabel("verified") ? params["verified"].Boolean() : false;

        auto observers = ObserverSnapshot();

        for (auto* notification : observers) {
            notification->OnKeywordVerification(remoteId, sessionId, verified);
        }

        ReleaseObserverSnapshot(observers);
    }

    void VoiceControlImplementation::NotifyServerMessage(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);
        LOGINFO("Notify onServerMessage %s", _maskPii ? "<***>" : eventData->payload);

        const string msgType = params.HasLabel("msgType") ? params["msgType"].String() : "";
        const string trx = params.HasLabel("trx") ? params["trx"].String() : "";
        const uint64_t created = params.HasLabel("created") ? static_cast<uint64_t>(params["created"].Number()) : 0;
        string msgPayload = params.HasLabel("msgPayload") ? jsonValueToString(params["msgPayload"]) : "";
        if (_maskPii) {
            // Redact payload when PII masking is enabled to avoid exposing sensitive data to observers.
            msgPayload.clear();
        }

        auto observers = ObserverSnapshot();
        LOGINFO("[VCDiag] NotifyServerMessage: observerCount=%zu msgType=%s trx=%s payloadLen=%zu", observers.size(), msgType.c_str(), trx.c_str(), msgPayload.size());
        if (msgType == "vrexResponse" && !_maskPii && msgPayload.size() > 0) {
            LOGINFO("[VCDiag] vrexResponse payload (first 200): %.200s", msgPayload.c_str());
        }

        for (auto* notification : observers) {
            notification->OnServerMessage(msgType, trx, created, msgPayload);
        }

        ReleaseObserverSnapshot(observers);
    }

    void VoiceControlImplementation::NotifyStreamEnd(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);
        LOGINFO("Notify onStreamEnd %s", eventData->payload);

        const uint32_t remoteId = params.HasLabel("remoteId") ? static_cast<uint32_t>(params["remoteId"].Number()) : 0;
        const string sessionId = params.HasLabel("sessionId") ? params["sessionId"].String() : "";
        const uint8_t reason = params.HasLabel("reason") ? static_cast<uint8_t>(params["reason"].Number()) : 0;

        auto observers = ObserverSnapshot();

        for (auto* notification : observers) {
            notification->OnStreamEnd(remoteId, sessionId, reason);
        }

        ReleaseObserverSnapshot(observers);
    }

    void VoiceControlImplementation::NotifySessionEnd(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);
        LOGINFO("Notify onSessionEnd %s", _maskPii ? "<***>" : eventData->payload);

        const uint32_t remoteId = params.HasLabel("remoteId") ? static_cast<uint32_t>(params["remoteId"].Number()) : 0;
        const string sessionId = params.HasLabel("sessionId") ? params["sessionId"].String() : "";
        const Exchange::SessionResult result = params.HasLabel("result") ? stringToEnum<Exchange::SessionResult>(params["result"].String(), Exchange::SessionResult::ERROR) : Exchange::SessionResult::ERROR;

        Exchange::ServerStats serverStats{};
        if (params.HasLabel("serverStats")) {
            JsonObject statsObj = params["serverStats"].Object();
            serverStats.dnsTime = statsObj.HasLabel("dnsTime") ? statsObj["dnsTime"].Double() : 0.0;
            // When PII masking is enabled, avoid propagating server IP to observers.
            serverStats.serverIp = (!_maskPii && statsObj.HasLabel("serverIp")) ? statsObj["serverIp"].String() : "";
            serverStats.connectTime = statsObj.HasLabel("connectTime") ? statsObj["connectTime"].Double() : 0.0;
        }

        // Extract result-specific subobjects and stbStats as raw JSON strings for passthrough
        string successData;
        string errorData;
        string abortData;
        string shortUtteranceData;
        string stbStatsData;

        if (params.HasLabel("success")) {
            JsonObject obj = params["success"].Object();
            obj.ToString(successData);
        }
        if (params.HasLabel("error")) {
            JsonObject obj = params["error"].Object();
            obj.ToString(errorData);
        }
        if (params.HasLabel("abort")) {
            JsonObject obj = params["abort"].Object();
            obj.ToString(abortData);
        }
        if (params.HasLabel("shortUtterance")) {
            JsonObject obj = params["shortUtterance"].Object();
            obj.ToString(shortUtteranceData);
        }
        if (params.HasLabel("stbStats")) {
            JsonObject obj = params["stbStats"].Object();
            obj.ToString(stbStatsData);
        }

        auto observers = ObserverSnapshot();
        LOGINFO("[VCDiag] NotifySessionEnd: observerCount=%zu sessionId=%s result=%d successLen=%zu errorLen=%zu",
                observers.size(), sessionId.c_str(), static_cast<int>(result), successData.size(), errorData.size());

        for (auto* notification : observers) {
            notification->OnSessionEnd(remoteId, sessionId, result, serverStats, successData, errorData, abortData, shortUtteranceData, stbStatsData);
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

    Core::hresult VoiceControlImplementation::GetApiVersionNumber(Exchange::VoiceControlGetApiVersionNumberResponse& response)
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

    Core::hresult VoiceControlImplementation::GetVoiceStatus(Exchange::VoiceStatusResponse& response)
    {
        LOGINFO("params={}");

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
            response.micTap.status.clear();
            response.capabilities = "[]";
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
        populateDeviceStatus("mic_tap", response.micTap);
        response.success = result.HasLabel("success") ? result["success"].Boolean() : false;

        if (result.HasLabel("capabilities")) {
            auto capabilityArray = result["capabilities"].Array();
            capabilityArray.ToString(response.capabilities);
        } else {
            response.capabilities = "[]";
        }

        return Core::ERROR_NONE;
    }

    Core::hresult VoiceControlImplementation::ConfigureVoice(const string& payload, Exchange::VoiceControlSuccessResult& result)
    {
        LOGINFO("params=%s", payload.empty() ? "{}" : payload.c_str());
        // Pass the caller's JSON through unchanged — preserves all optional fields exactly as provided.
        const string& jsonParams = payload.empty() ? string("{}") : payload;

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_CONFIGURE_VOICE, jsonParams, iarmResult);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            return Core::ERROR_NONE;
        }

        result.success = iarmResult.HasLabel("success") ? iarmResult["success"].Boolean() : false;
        return Core::ERROR_NONE;
    }

    Core::hresult VoiceControlImplementation::SetVoiceInit(const string& payload, Exchange::VoiceControlSuccessResult& result)
    {
        LOGINFO("params=%s", payload.empty() ? "{}" : payload.c_str());
        const string& jsonParams = payload.empty() ? string("{}") : payload;

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_SET_VOICE_INIT, jsonParams, iarmResult);
        if (callResult != Core::ERROR_NONE) {
            result.success = false;
            return Core::ERROR_NONE;
        }

        result.success = iarmResult.HasLabel("success") ? iarmResult["success"].Boolean() : false;
        return Core::ERROR_NONE;
    }

    Core::hresult VoiceControlImplementation::SendVoiceMessage(const string& msgType, const string& trx, const uint64_t created, const string& msgPayload, Exchange::VoiceControlSuccessResult& result)
    {
        LOGINFO("params: msgType=%s, trx=%s, created=%llu, msgPayload=%s",
                msgType.c_str(),
                trx.empty() ? "<not set>" : trx.c_str(),
                (unsigned long long)created,
                msgPayload.empty() ? "<not set>" : (_maskPii ? "<***>" : msgPayload.c_str()));
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

    Core::hresult VoiceControlImplementation::VoiceSessionByText(const string& transcription, const Exchange::DeviceType type, Exchange::VoiceControlSuccessResult& result)
    {
        LOGINFO("params: transcription=%s, type=%s",
                _maskPii ? "<***>" : transcription.c_str(),
                deviceTypeToString(type));
        // Translate the deprecated API to voiceSessionRequest
        const char* translatedType;

        switch (type) {
            case Exchange::DeviceType::PTT:
                translatedType = "ptt_transcription";
                break;
            case Exchange::DeviceType::FF:
                translatedType = "ff_transcription";
                break;
            case Exchange::DeviceType::MIC:
                translatedType = "mic_transcription";
                break;
            default:
                translatedType = "ptt_transcription";
                break;
        }

        JsonObject params;
        params["type"] = translatedType;
        if (!transcription.empty()) {
            params["transcription"] = transcription;
        }
        string payload;
        params.ToString(payload);

        string rawResult;
        Core::hresult hr = VoiceSessionRequest(payload, rawResult);
        if (hr == Core::ERROR_NONE) {
            JsonObject parsed;
            parsed.FromString(rawResult);
            result.success = parsed.HasLabel("success") ? parsed["success"].Boolean() : false;
        } else {
            result.success = false;
        }
        return hr;
    }

    Core::hresult VoiceControlImplementation::GetVoiceSessionTypes(bool& success, Exchange::IStringIterator*& types)
    {
        LOGINFO("params={}");
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

    Core::hresult VoiceControlImplementation::VoiceSessionRequest(const string& payload, string& result)
    {
        LOGINFO("params=%s", payload.empty() ? "{}" : payload.c_str());
        const string& jsonParams = payload.empty() ? string("{}") : payload;

        JsonObject iarmResult;
        Core::hresult callResult = IARMBusCall(CTRLM_VOICE_IARM_CALL_SESSION_REQUEST, jsonParams, iarmResult);
        if (callResult != Core::ERROR_NONE) {
            result = "{\"success\":false}";
            return Core::ERROR_NONE;
        }

        iarmResult.ToString(result);
        return Core::ERROR_NONE;
    }

    Core::hresult VoiceControlImplementation::VoiceSessionTerminate(const string& sessionId, Exchange::VoiceControlSuccessResult& result)
    {
        LOGINFO("params: sessionId=%s", sessionId.c_str());
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

    Core::hresult VoiceControlImplementation::VoiceSessionAudioStreamStart(const string& sessionId, Exchange::VoiceControlSuccessResult& result)
    {
        LOGINFO("params: sessionId=%s", sessionId.c_str());
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
