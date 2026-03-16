#pragma once

#include "Module.h"
#include <interfaces/IVoiceControl.h>
#include <interfaces/json/JVoiceControl.h>

namespace WPEFramework {
namespace Plugin {

    class VoiceControl : public PluginHost::IPlugin, public PluginHost::JSONRPC {
    public:
        VoiceControl(const VoiceControl&) = delete;
        VoiceControl& operator=(const VoiceControl&) = delete;

        VoiceControl()
            : _implementation(nullptr)
            , _connectionId(0)
            , _service(nullptr)
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

            void OnSessionBegin(const Exchange::SessionBeginEvent& event) override {
                Exchange::JVoiceControl::Event::OnSessionBegin(_parent, event);
            }
            void OnStreamBegin(const Exchange::StreamBeginEvent& event) override {
                Exchange::JVoiceControl::Event::OnStreamBegin(_parent, event);
            }
            void OnKeywordVerification(const Exchange::KeywordVerificationEvent& event) override {
                Exchange::JVoiceControl::Event::OnKeywordVerification(_parent, event);
            }
            void OnServerMessage(const Exchange::ServerMessageEvent& event) override {
                Exchange::JVoiceControl::Event::OnServerMessage(_parent, event);
            }
            void OnStreamEnd(const Exchange::StreamEndEvent& event) override {
                Exchange::JVoiceControl::Event::OnStreamEnd(_parent, event);
            }
            void OnSessionEnd(const Exchange::SessionEndEvent& event) override {
                Exchange::JVoiceControl::Event::OnSessionEnd(_parent, event);
            }

            BEGIN_INTERFACE_MAP(Notification)
                INTERFACE_ENTRY(Exchange::IVoiceControl::INotification)
            END_INTERFACE_MAP

        private:
            VoiceControl& _parent;
        };

        void Deactivated(RPC::IRemoteConnection* connection);

        Exchange::IVoiceControl* _implementation;
        uint32_t _connectionId;
        PluginHost::IShell* _service;
        Core::SinkType<ConnectionNotification> _connectionNotification;
        Core::SinkType<Notification> _notification;
    };

} // namespace Plugin
} // namespace WPEFramework
