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

```bash
mkdir build && cd build
cmake ..
make
```

## Dependencies

- WPE Framework
- IARM Bus
- CTRLM (with voice support)
- jsoncpp

## API Version

Current API Version: 1.4.0

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for details.
