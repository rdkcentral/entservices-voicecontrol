# VoiceControl Plugin

A WPE Framework plugin that provides voice assistant management for RDK devices. Enables voice-enabled remote controls to interact with voice servers for voice search, commands, and conversational AI features.

## Overview

The VoiceControl service manages the complete voice interaction lifecycle including:
- Voice session management (PTT, Far-Field, transcription)
- Audio streaming from voice-enabled remote controls
- Voice server communication and configuration
- Real-time event notifications
- Privacy controls (PII masking)

## Key Features

- **Multiple Session Types**: Push-to-Talk, Far-Field, and text-based voice sessions
- **Real-time Audio Streaming**: Low-latency audio capture and transmission
- **Event System**: Comprehensive notifications for session lifecycle events
- **Flexible Configuration**: Customizable voice parameters and server settings
- **Privacy Protection**: Configurable PII masking and privacy controls

## Architecture

The plugin integrates with the CTRLM (Control Manager) voice subsystem via IARM bus, providing a JSON-RPC interface for client applications.

For detailed architecture information, see [ARCHITECTURE.md](ARCHITECTURE.md).

For product features and capabilities, see [PRODUCT.md](PRODUCT.md).

## Building

### Yocto/BitBake Build

```bash
bitbake thunder-plugins
```

### Manual Build

```bash
mkdir build && cd build
cmake ..
make
```

## Versioning

Version info is generated at configure time ([cmake/GitVersion.cmake](cmake/GitVersion.cmake)); there is no version file to edit.
The version comes from `-DPLUGIN_VERSION` (the Yocto recipe passes `${PV}`). Without it, the nearest version git tag is used, then `BUILD_REFERENCE` if it is a version, else `1.0.0`. A version is `x.y.z` with an optional suffix such as `1.0.5.1` or `1.0.2-RDK7.1`; major/minor/patch come from `x.y.z` and the log shows the full string.

- Startup log prints `name <VoiceControl> version <x.y.z> branch <...> commit <hash>`, matching ctrlm and xr-voice-sdk. `++` is appended to the version if the source tree has local modifications.
- `getApiVersionNumber` returns the major number, so only bump major for breaking API changes. It does not affect the `.1.` in JSON-RPC method names.
- Each component must be <= 255 (Thunder stores them as `uint8_t`).

## Testing

### API Version and Quirks

Test common plugin methods:

```bash
# Get API version number
curl -d '{"jsonrpc":"2.0","id":"4","method":"org.rdk.VoiceControl.1.getApiVersionNumber"}' http://127.0.0.1:9998/jsonrpc

# Get plugin quirks
curl --header "Content-Type: application/json" --request POST --data '{"jsonrpc":"2.0","id":"3","method": "org.rdk.VoiceControl.1.getQuirks"}' http://127.0.0.1:9998/jsonrpc
```

### Voice Session Examples

```bash
# Check voice status
curl -d '{"jsonrpc":"2.0","id":"5","method":"org.rdk.VoiceControl.1.voiceStatus"}' http://127.0.0.1:9998/jsonrpc

# Get supported session types
curl -d '{"jsonrpc":"2.0","id":"6","method":"org.rdk.VoiceControl.1.voiceSessionTypes"}' http://127.0.0.1:9998/jsonrpc
```

## Dependencies

- WPE Framework
- IARM Bus
- CTRLM (with voice support)
- jsoncpp

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for details.
