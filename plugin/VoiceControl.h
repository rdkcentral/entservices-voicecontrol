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
#include "UtilsLogging.h"
#include <interfaces/IVoiceControl.h>
#include <interfaces/json/JVoiceControl.h>
#include <interfaces/IConfiguration.h>

namespace WPEFramework {
namespace Plugin {

    class VoiceControl : public PluginHost::IPlugin, public PluginHost::JSONRPC {
    public:
        VoiceControl(const VoiceControl&) = delete;
        VoiceControl& operator=(const VoiceControl&) = delete;

        VoiceControl()
            : _adminLock()
            , _implementation(nullptr)
            , _connectionId(0)
            , _service(nullptr)
            , _isShuttingDown(false)
            , _connectionNotification(this)
            , _notification(this)
        {
        }
        ~VoiceControl() override = default;

        BEGIN_INTERFACE_MAP(VoiceControl)
            INTERFACE_ENTRY(PluginHost::IPlugin)
            INTERFACE_ENTRY(PluginHost::IDispatcher)
            INTERFACE_AGGREGATE(Exchange::IVoiceControl, _implementation)
        END_INTERFACE_MAP

        // IPlugin methods
        const string Initialize(PluginHost::IShell* service) override;
        void Deinitialize(PluginHost::IShell* service) override;
        string Information() const override { return {}; }

    private:
        class ConnectionNotification : public RPC::IRemoteConnection::INotification {
        public:
            explicit ConnectionNotification(VoiceControl* parent) : _parent(*parent) {}
            ~ConnectionNotification() override = default;

            void Activated(RPC::IRemoteConnection*) override {}
            void Deactivated(RPC::IRemoteConnection* connection) override {
                _parent.Deactivated(connection);
            }

            BEGIN_INTERFACE_MAP(ConnectionNotification)
                INTERFACE_ENTRY(RPC::IRemoteConnection::INotification)
            END_INTERFACE_MAP

        private:
            VoiceControl& _parent;
        };

        class Notification : public Exchange::IVoiceControl::INotification {
        public:
            explicit Notification(VoiceControl* parent) : _parent(*parent) {}
            ~Notification() override = default;

            void OnSessionBegin(const uint32_t remoteId, const string& sessionId, const Exchange::DeviceType deviceType, const bool keywordVerification) override {
                LOGINFO("Notify onSessionBegin remoteId=%u sessionId=%s deviceType=%u keywordVerification=%u",
                    remoteId,
                    sessionId.c_str(),
                    static_cast<unsigned>(deviceType),
                    keywordVerification ? 1u : 0u);
                Exchange::JVoiceControl::Event::OnSessionBegin(_parent, remoteId, sessionId, deviceType, keywordVerification);
            }
            void OnStreamBegin(const uint32_t remoteId, const string& sessionId) override {
                LOGINFO("Notify onStreamBegin remoteId=%u sessionId=%s", remoteId, sessionId.c_str());
                Exchange::JVoiceControl::Event::OnStreamBegin(_parent, remoteId, sessionId);
            }
            void OnKeywordVerification(const uint32_t remoteId, const string& sessionId, const bool verified) override {
                LOGINFO("Notify onKeywordVerification remoteId=%u sessionId=%s verified=%u", remoteId, sessionId.c_str(), verified ? 1u : 0u);
                Exchange::JVoiceControl::Event::OnKeywordVerification(_parent, remoteId, sessionId, verified);
            }
            void OnServerMessage(const string& msgType, const string& trx, const uint64_t created, const string& msgPayload) override {
                LOGINFO("Notify onServerMessage msgType=%s trx=%s created=%llu msgPayload=%s",
                    msgType.c_str(),
                    trx.c_str(),
                    static_cast<unsigned long long>(created),
                    msgPayload.c_str());
                Exchange::JVoiceControl::Event::OnServerMessage(_parent, msgType, trx, created, msgPayload);
            }
            void OnStreamEnd(const uint32_t remoteId, const string& sessionId, const uint8_t reason) override {
                LOGINFO("Notify onStreamEnd remoteId=%u sessionId=%s reason=%u", remoteId, sessionId.c_str(), static_cast<unsigned>(reason));
                Exchange::JVoiceControl::Event::OnStreamEnd(_parent, remoteId, sessionId, reason);
            }
            void OnSessionEnd(const uint32_t remoteId, const string& sessionId, const Exchange::SessionResult result, const Exchange::ServerStats& serverStats, const Core::OptionalType<string>& success, const Core::OptionalType<string>& error, const Core::OptionalType<string>& abort, const Core::OptionalType<string>& shortUtterance, const Core::OptionalType<string>& stbStats) override {
                LOGINFO("Notify onSessionEnd remoteId=%u sessionId=%s result=%u success=%s error=%s abort=%s shortUtterance=%s stbStats=%s",
                    remoteId,
                    sessionId.c_str(),
                    static_cast<unsigned>(result),
                    success.IsSet() ? success.Value().c_str() : "",
                    error.IsSet() ? error.Value().c_str() : "",
                    abort.IsSet() ? abort.Value().c_str() : "",
                    shortUtterance.IsSet() ? shortUtterance.Value().c_str() : "",
                    stbStats.IsSet() ? stbStats.Value().c_str() : "");
                // Build params directly so optional opaque fields are only included when non-empty
                JsonData::VoiceControl::OnSessionEndParamsData params;
                params.RemoteId = remoteId;
                params.SessionId = sessionId;
                params.Result = result;
                // Only include serverStats when the session completed (success/error).
                // Abort events from ctrlm never include serverStats in the wire format.
                if (result != Exchange::SessionResult::ABORT) {
                    params.ServerStats = serverStats;
                }
                if (success.IsSet() && !success.Value().empty()) {
                    params.Success = success.Value();
                    params.Success.SetQuoted(false);
                }
                if (error.IsSet() && !error.Value().empty()) {
                    params.Error = error.Value();
                    params.Error.SetQuoted(false);
                }
                if (abort.IsSet() && !abort.Value().empty()) {
                    params.Abort = abort.Value();
                    params.Abort.SetQuoted(false);
                }
                if (shortUtterance.IsSet() && !shortUtterance.Value().empty()) {
                    params.ShortUtterance = shortUtterance.Value();
                    params.ShortUtterance.SetQuoted(false);
                }
                if (stbStats.IsSet() && !stbStats.Value().empty()) {
                    params.StbStats = stbStats.Value();
                    params.StbStats.SetQuoted(false);
                }
                Exchange::JVoiceControl::Event::OnSessionEnd(_parent, params);
            }

            BEGIN_INTERFACE_MAP(Notification)
                INTERFACE_ENTRY(Exchange::IVoiceControl::INotification)
            END_INTERFACE_MAP

        private:
            VoiceControl& _parent;
        };

        void Deactivated(RPC::IRemoteConnection* connection);

        Core::CriticalSection _adminLock;
        Exchange::IVoiceControl* _implementation;
        uint32_t _connectionId;
        PluginHost::IShell* _service;
        bool _isShuttingDown;
        Core::Sink<ConnectionNotification> _connectionNotification;
        Core::Sink<Notification> _notification;
        Exchange::IConfiguration* _configure{};
    };

} // namespace Plugin
} // namespace WPEFramework
