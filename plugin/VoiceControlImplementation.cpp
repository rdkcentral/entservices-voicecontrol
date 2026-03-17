#include "VoiceControlImplementation.h"
#include "libIBusDaemon.h"
#include "UtilsIarm.h"
#include "UtilsJsonRpc.h"

#include <algorithm>

namespace WPEFramework {
namespace Plugin {

    SERVICE_REGISTRATION(VoiceControlImplementation, 1, 0)

    VoiceControlImplementation* VoiceControlImplementation::_instance = nullptr;

    VoiceControlImplementation::VoiceControlImplementation()
        : _adminLock()
        , _service(nullptr)
        , _notifications()
        , _hasOwnProcess(false)
        , _maskPii(false)
    {
        _instance = this;
    }

    VoiceControlImplementation::~VoiceControlImplementation()
    {
        DeinitializeIARM();
        _instance = nullptr;
        _service = nullptr;
    }

    uint32_t VoiceControlImplementation::Configure(PluginHost::IShell* service)
    {
        LOGINFO("Configuring VoiceControlImplementation");
        uint32_t result = Core::ERROR_NONE;
        ASSERT(service != nullptr);
        _service = service;
        _service->AddRef();
        InitializeIARM();

        // Query the initial maskPii setting from the voice status
        JsonObject statusResult;
        Core::hresult hr = IARMBusCall(CTRLM_VOICE_IARM_CALL_STATUS, "{}", statusResult);
        if (hr == Core::ERROR_NONE) {
            _maskPii = statusResult.HasLabel("maskPii") ? statusResult["maskPii"].Boolean() : false;
            LOGINFO("Mask pii set to %s.", (_maskPii ? "True" : "False"));
        }

        return result;
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

    void VoiceControlImplementation::InitializeIARM()
    {
        if (Utils::IARM::init()) {
            _hasOwnProcess = true;
            IARM_Result_t res;
            IARM_CHECK( IARM_Bus_RegisterEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_SESSION_BEGIN,        voiceEventHandler) );
            IARM_CHECK( IARM_Bus_RegisterEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_STREAM_BEGIN,         voiceEventHandler) );
            IARM_CHECK( IARM_Bus_RegisterEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_KEYWORD_VERIFICATION, voiceEventHandler) );
            IARM_CHECK( IARM_Bus_RegisterEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_SERVER_MESSAGE,       voiceEventHandler) );
            IARM_CHECK( IARM_Bus_RegisterEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_STREAM_END,           voiceEventHandler) );
            IARM_CHECK( IARM_Bus_RegisterEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_SESSION_END,          voiceEventHandler) );
        } else {
            _hasOwnProcess = false;
        }
    }

    void VoiceControlImplementation::DeinitializeIARM()
    {
        if (_hasOwnProcess) {
            IARM_Result_t res;
            IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_SESSION_END,          voiceEventHandler) );
            IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_STREAM_END,           voiceEventHandler) );
            IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_KEYWORD_VERIFICATION, voiceEventHandler) );
            IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_SERVER_MESSAGE,       voiceEventHandler) );
            IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_STREAM_BEGIN,         voiceEventHandler) );
            IARM_CHECK( IARM_Bus_RemoveEventHandler(CTRLM_MAIN_IARM_BUS_NAME, CTRLM_VOICE_IARM_EVENT_JSON_SESSION_BEGIN,        voiceEventHandler) );

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

        Exchange::SessionBeginEvent event;
        event.remoteId = params.HasLabel("remoteId") ? static_cast<uint32_t>(params["remoteId"].Number()) : 0;
        event.sessionId = params.HasLabel("sessionId") ? params["sessionId"].String() : "";
        event.deviceType = params.HasLabel("deviceType") ? static_cast<Exchange::DeviceType>(static_cast<uint8_t>(params["deviceType"].Number())) : Exchange::DeviceType::PTT;
        event.keywordVerification = params.HasLabel("keywordVerification") ? params["keywordVerification"].Boolean() : false;

        _adminLock.Lock();
        for (auto* notification : _notifications) {
            notification->OnSessionBegin(event);
        }
        _adminLock.Unlock();
    }

    void VoiceControlImplementation::NotifyStreamBegin(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);

        Exchange::StreamBeginEvent event;
        event.remoteId = params.HasLabel("remoteId") ? static_cast<uint32_t>(params["remoteId"].Number()) : 0;
        event.sessionId = params.HasLabel("sessionId") ? params["sessionId"].String() : "";

        _adminLock.Lock();
        for (auto* notification : _notifications) {
            notification->OnStreamBegin(event);
        }
        _adminLock.Unlock();
    }

    void VoiceControlImplementation::NotifyKeywordVerification(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);

        Exchange::KeywordVerificationEvent event;
        event.remoteId = params.HasLabel("remoteId") ? static_cast<uint32_t>(params["remoteId"].Number()) : 0;
        event.sessionId = params.HasLabel("sessionId") ? params["sessionId"].String() : "";
        event.verified = params.HasLabel("verified") ? params["verified"].Boolean() : false;

        _adminLock.Lock();
        for (auto* notification : _notifications) {
            notification->OnKeywordVerification(event);
        }
        _adminLock.Unlock();
    }

    void VoiceControlImplementation::NotifyServerMessage(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);

        Exchange::ServerMessageEvent event;
        event.msgType = params.HasLabel("msgType") ? params["msgType"].String() : "";
        event.trx = params.HasLabel("trx") ? params["trx"].String() : "";
        event.created = params.HasLabel("created") ? static_cast<uint64_t>(params["created"].Number()) : 0;
        event.msgPayload = params.HasLabel("msgPayload") ? params["msgPayload"].String() : "";

        _adminLock.Lock();
        for (auto* notification : _notifications) {
            notification->OnServerMessage(event);
        }
        _adminLock.Unlock();
    }

    void VoiceControlImplementation::NotifyStreamEnd(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);

        Exchange::StreamEndEvent event;
        event.remoteId = params.HasLabel("remoteId") ? static_cast<uint32_t>(params["remoteId"].Number()) : 0;
        event.sessionId = params.HasLabel("sessionId") ? params["sessionId"].String() : "";
        event.reason = params.HasLabel("reason") ? static_cast<uint8_t>(params["reason"].Number()) : 0;

        _adminLock.Lock();
        for (auto* notification : _notifications) {
            notification->OnStreamEnd(event);
        }
        _adminLock.Unlock();
    }

    void VoiceControlImplementation::NotifySessionEnd(ctrlm_voice_iarm_event_json_t* eventData)
    {
        JsonObject params;
        params.FromString(eventData->payload);

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

        _adminLock.Lock();
        for (auto* notification : _notifications) {
            notification->OnSessionEnd(event);
        }
        _adminLock.Unlock();
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
        response.version = 1;
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

        return response.success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
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

        return success ? Core::ERROR_NONE : Core::ERROR_GENERAL;
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
