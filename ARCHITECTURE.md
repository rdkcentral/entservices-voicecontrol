# VoiceControl Plugin Architecture

## Overview

The VoiceControl plugin is a WPE Framework service that provides voice assistant management functionality for RDK devices. It acts as an interface between client applications and the CTRLM (Control Manager) voice subsystem through IARM (Inter Application Resource Manager) bus communication, enabling voice-enabled remote controls and voice assistant integrations.

## System Architecture

### High-Level Components

```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────────┐
│   Client Apps   │◄──►│  VoiceControl    │◄──►│ CTRLM Voice System  │
│   (JSON-RPC)    │    │   WPE Plugin     │    │    (via IARM)       │
└─────────────────┘    └──────────────────┘    └─────────────────────┘
                              │                           │
                              ▼                           ▼
                       ┌─────────────┐           ┌───────────────┐
                       │ IARM Bus    │           │ Voice Server  │
                       └─────────────┘           └───────────────┘
```

### Core Components

#### 1. JSON-RPC Interface
- **Purpose**: Provides standardized API endpoints for voice control operations
- **Location**: `plugin/VoiceControl.cpp`
- **Key Methods**:
    - `voiceStatus()` - Query current voice system status and configuration
    - `configureVoice()` - Configure voice assistant settings and parameters
    - `setVoiceInit()` - Initialize voice system with specific parameters
    - `sendVoiceMessage()` - Send control messages to voice system
    - `voiceSessionTypes()` - Query supported voice session types
    - `voiceSessionRequest()` - Initiate voice sessions (PTT, transcription, etc.)
    - `voiceSessionTerminate()` - End active voice sessions
    - `voiceSessionAudioStreamStart()` - Start audio streaming for voice sessions
    - `voiceSessionByText()` - Legacy text-based session interface (deprecated)

#### 2. IARM Communication Layer
- **Purpose**: Interfaces with system-level voice control manager (CTRLM)
- **Protocol**: Uses IARM bus for inter-process communication
- **Key Features**:
    - Asynchronous event handling for voice sessions
    - Dynamic memory allocation for variable-sized payloads
    - API revision checking for compatibility
    - Bidirectional communication for control and events

#### 3. CTRLM Voice Integration
- **Purpose**: Provides low-level voice hardware and server management
- **Dependencies**:
    - `ctrlm_ipc.h` - Main control manager interface
    - `ctrlm_ipc_voice.h` - Voice-specific operations and structures
- **Functionality**:
    - Voice session lifecycle management
    - Audio stream handling from remote controls
    - Voice server communication and configuration
    - Keyword verification and wake word detection
    - Audio encoding and transmission

## Software Architecture

### Plugin Framework Integration
The VoiceControl plugin inherits from WPE Framework's `PluginHost::JSONRPC` class, providing:
- Automatic JSON-RPC method registration and dispatch
- Standardized error handling and response formatting
- Plugin lifecycle management (Initialize/Deinitialize)
- Configuration management through WPE Framework
- Event notification system for real-time updates

### Threading Model
- **Main Thread**: JSON-RPC request handling and response processing
- **IARM Thread**: Asynchronous system communication with CTRLM
- **Event Thread**: Voice session event processing and notifications

### Event Handling System
The plugin handles six primary voice event types:
- **Session Begin**: Voice session initialization from remote control
- **Stream Begin**: Audio stream start notification
- **Keyword Verification**: Wake word/keyword detection confirmation
- **Server Message**: Voice server response and intermediate results
- **Stream End**: Audio stream completion notification
- **Session End**: Voice session termination with final results

### Error Handling
- Comprehensive error checking for IARM bus operations
- Memory allocation validation for large payloads
- API revision compatibility checking
- Timeout protection for long-running operations
- Detailed logging for debugging and monitoring
- Graceful degradation when voice hardware unavailable

## Configuration

### Build-time Configuration
- CMake options in `plugin/CMakeLists.txt`
- Conditional compilation based on CTRLM voice support
- API version management (currently 1.4.0)
- Optional features through preprocessor definitions

### Runtime Configuration
- Plugin configuration via `VoiceControl.config`
- IARM bus connection parameters
- CTRLM voice subsystem settings
- Privacy controls (PII masking)

## Dependencies

### System Dependencies
- **IARM Bus**: Inter-application communication framework
- **CTRLM**: Control Manager with voice subsystem support
- **WPE Framework**: Plugin hosting framework
- **libIBus**: IARM bus client library

### Utility Dependencies
- `UtilsJsonRpc.h` - JSON-RPC helper functions and macros
- `UtilsIarm.h` - IARM communication utilities
- `UtilsString.h` - String manipulation utilities
- `UtilsLogging.h` - Centralized logging framework

## Data Flow

### Voice Session Request Flow
1. **Client Request**: JSON-RPC voice session request received
2. **Parameter Validation**: Request parameters validated and serialized
3. **Memory Allocation**: Dynamic allocation for IARM payload structure
4. **IARM Communication**: Request forwarded to CTRLM voice subsystem
5. **Server Processing**: Voice server processes audio/text input
6. **Event Notifications**: Real-time events sent back to clients
7. **Response Processing**: Final results formatted and returned

### Event Notification Flow
1. **Hardware Event**: Voice event generated by remote control
2. **CTRLM Processing**: Event processed by control manager
3. **IARM Event**: Event broadcast on IARM bus
4. **Plugin Handler**: VoiceControl receives and validates event
5. **JSON Conversion**: Event payload converted to JSON
6. **Client Notification**: Event sent to subscribed clients via JSON-RPC

## Privacy and Security Considerations

### Privacy Protection
- **PII Masking**: Configurable masking of personally identifiable information
- **Selective Logging**: Privacy-aware logging with configurable detail levels
- **Data Minimization**: Only necessary data transmitted to voice servers
- **Local Processing**: Session management performed locally on device

### Security Measures
- Input validation on all JSON-RPC parameters
- Memory safety through careful allocation management
- API revision checking to prevent version mismatch attacks
- Error information sanitization in responses
- Access control through WPE Framework security model

## Performance Characteristics

### Response Times
- **Status Queries**: Sub-millisecond local operations
- **Session Initiation**: 100-300ms including IARM roundtrip
- **Audio Streaming**: Real-time streaming with minimal latency
- **Event Notifications**: Near-instant delivery to subscribed clients

### Resource Efficiency
- **Memory**: Dynamic allocation based on payload size
- **CPU**: Event-driven architecture with minimal polling
- **Network**: Optimized audio encoding for bandwidth efficiency
- **Process Isolation**: Can run in-process or as separate service

### Scalability
- Supports multiple simultaneous voice sessions
- Efficient event distribution to multiple subscribers
- Configurable timeout mechanisms for resource management
- Graceful handling of high event rates
