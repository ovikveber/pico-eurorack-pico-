// SPDX-License-Identifier: GPL-3.0-or-later
// OV private bus protocol, shared by the Rack port and (by copying) the Pico firmware.
//
// Frame: [0xAA][voice][note][gate][bendL][bendH][CRC8]
// - voice: 0..7
// - note: MIDI note number
// - gate: 0 or 1 (receiver applies gate polarity)
// - bend: int16 little-endian, 0 = center, +/-8192 = full range
// - CRC8: polynomial 0x07 over the first 6 bytes
//
// Expander message: 8 frames + sequence + magic. The Rack port puts the frames
// directly into an expander message; the hardware sends the frames over GP14 UART.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ov {

constexpr int kVoices = 8;
constexpr uint8_t kStart = 0xAA;
constexpr uint32_t kMagic = 0x4F564D32; // 'OVM2'

#pragma pack(push, 1)
struct VoiceFrame {
    uint8_t start;
    uint8_t voice;
    uint8_t note;
    uint8_t gate;
    uint8_t bendL;
    uint8_t bendH;
    uint8_t crc;
};

struct BusMessage {
    VoiceFrame voices[kVoices];
    uint32_t seq;
    uint32_t magic;
};
#pragma pack(pop)

inline uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

inline void encodeFrame(VoiceFrame& f, uint8_t voice, uint8_t note, bool gate, int16_t bend) {
    f.start = kStart;
    f.voice = voice;
    f.note = note;
    f.gate = gate ? 1 : 0;
    f.bendL = (uint8_t)((uint16_t)bend & 0xFF);
    f.bendH = (uint8_t)(((uint16_t)bend >> 8) & 0xFF);
    f.crc = crc8((const uint8_t*)&f, 6);
}

inline bool decodeFrame(const VoiceFrame& f, uint8_t& voice, uint8_t& note, bool& gate, int16_t& bend) {
    if (f.start != kStart)
        return false;
    if (f.crc != crc8((const uint8_t*)&f, 6))
        return false;
    if (f.voice >= kVoices)
        return false;
    voice = f.voice;
    note = f.note;
    gate = f.gate != 0;
    bend = (int16_t)((uint16_t)f.bendL | ((uint16_t)f.bendH << 8));
    return true;
}

// Rack polyphonic transport for the same private bus.
// 16 channels = 8 voices x 2 floats:
//   channel 2v   = voice(3) | note(7) | gate(1), 11 bits
//   channel 2v+1 = bend(16) | CRC8(8), 24 bits
// Both are exactly representable as float (<= 2^24).
constexpr int kBusChannels = 16;

inline float packNoteGate(uint8_t voice, uint8_t note, bool gate) {
    uint32_t v = ((uint32_t)voice << 8) | ((uint32_t)note << 1) | (gate ? 1u : 0u);
    return (float)v;
}

inline float packBendCrc(uint8_t voice, uint8_t note, bool gate, int16_t bend) {
    VoiceFrame f;
    encodeFrame(f, voice, note, gate, bend);
    uint32_t v = ((uint32_t)(uint16_t)bend) | ((uint32_t)f.crc << 16);
    return (float)v;
}

inline bool unpackVoice(float noteGate, float bendCrc, uint8_t& voice, uint8_t& note, bool& gate, int16_t& bend) {
    uint32_t a = (uint32_t)noteGate;
    uint32_t b = (uint32_t)bendCrc;
    voice = (uint8_t)((a >> 8) & 0x7);
    note = (uint8_t)((a >> 1) & 0x7F);
    gate = (a & 1) != 0;
    bend = (int16_t)(b & 0xFFFF);
    uint8_t crc = (uint8_t)((b >> 16) & 0xFF);

    VoiceFrame f;
    encodeFrame(f, voice, note, gate, bend);
    return voice < kVoices && crc == f.crc;
}

} // namespace ov
