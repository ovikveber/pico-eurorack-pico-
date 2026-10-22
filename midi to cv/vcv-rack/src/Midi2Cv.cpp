// SPDX-License-Identifier: GPL-3.0-or-later
// VCV Rack 2 port of the OV-MIDI 8-voice MIDI -> CV/Gate firmware.
// No screen/encoder: settings are in the right-click context menu.
// Hardware behavior is preserved: CV 0..5V, gate 0/5V, 1/2/5/8 V/oct.

#include "plugin.hpp"
#include "PrivateBus.hpp"

#include <algorithm>

struct Midi2Cv : Module {
    enum OutputIds {
        CV1, GATE1,
        CV2, GATE2,
        CV3, GATE3,
        CV4, GATE4,
        CV5, GATE5,
        CV6, GATE6,
        CV7, GATE7,
        CV8, GATE8,
        BUS,
        NUM_OUTPUTS
    };
    enum LightIds {
        NUM_LIGHTS
    };

    midi::InputQueue midiInput;

    // Same defaults as the hardware: voices 1..8 on MIDI channels 1..8.
    uint8_t midiChannel[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t voct = 1;              // 1 / 2 / 5 / 8 V/oct
    bool gateInvert = false;

    uint8_t noteStack[8][16];
    uint8_t noteCnt[8] = {};
    uint8_t curNote[8] = { 24, 24, 24, 24, 24, 24, 24, 24 }; // NOTE0 = 24 (C1)
    bool gate[8] = {};
    int16_t bend[8] = {};

    Midi2Cv() {
        config(0, 0, NUM_OUTPUTS, NUM_LIGHTS);
        for (int v = 0; v < 8; v++) {
            configOutput(CV1 + 2 * v, string::f("Voice %d CV", v + 1));
            configOutput(GATE1 + 2 * v, string::f("Voice %d gate", v + 1));
        }
        // One polyphonic port = the private bus (hardware GP14 3.5mm jack).
        // The 8 CV/Gate outputs above are the general-purpose outputs.
        configOutput(BUS, "Private bus (poly 16ch)");
    }

    void onReset() override {
        midiInput.reset();
        for (int v = 0; v < 8; v++) {
            noteCnt[v] = 0;
            curNote[v] = 24;
            gate[v] = false;
            bend[v] = 0;
        }
    }

    float cvVoltage(uint8_t note) const {
        if (note < 24)
            note = 24;
        float v = (note - 24) / 12.0f * (float)voct;
        if (v < 0.f)
            v = 0.f;
        if (v > 5.f)
            v = 5.f;
        return v;
    }

    void noteOn(uint8_t channel, uint8_t note) {
        for (int v = 0; v < 8; v++) {
            if (midiChannel[v] != channel)
                continue;
            if (noteCnt[v] < 16)
                noteStack[v][noteCnt[v]++] = note;
            curNote[v] = note;
            gate[v] = true;
        }
    }

    void noteOff(uint8_t channel, uint8_t note) {
        for (int v = 0; v < 8; v++) {
            if (midiChannel[v] != channel)
                continue;
            bool found = false;
            for (int i = 0; i < noteCnt[v]; i++) {
                if (noteStack[v][i] == note) {
                    for (int j = i; j < noteCnt[v] - 1; j++)
                        noteStack[v][j] = noteStack[v][j + 1];
                    noteCnt[v]--;
                    found = true;
                    break;
                }
            }
            if (found) {
                if (noteCnt[v] > 0)
                    curNote[v] = noteStack[v][noteCnt[v] - 1];
                else
                    gate[v] = false;
            }
        }
    }

    void handleMessage(const midi::Message& msg) {
        uint8_t status = msg.getStatus();
        uint8_t channel = msg.getChannel() + 1;

        if (status == 0x9 && msg.getValue() > 0) {
            noteOn(channel, msg.getNote());
        }
        else if (status == 0x8 || (status == 0x9 && msg.getValue() == 0)) {
            noteOff(channel, msg.getNote());
        }
        else if (status == 0xE) {
            if (msg.bytes.size() >= 3) {
                uint16_t value = ((uint16_t)msg.getValue() << 7) | msg.bytes[1];
                int16_t b = (int16_t)value - 8192;
                for (int v = 0; v < 8; v++) {
                    if (midiChannel[v] == channel)
                        bend[v] = b;
                }
            }
        }
    }

    void process(const ProcessArgs& args) override {
        midi::Message msg;
        while (midiInput.tryPop(&msg, args.frame)) {
            handleMessage(msg);
        }

        for (int v = 0; v < 8; v++) {
            outputs[CV1 + 2 * v].setVoltage(cvVoltage(curNote[v]));
            outputs[GATE1 + 2 * v].setVoltage((gate[v] ^ gateInvert) ? 5.f : 0.f);
        }

        // Private bus: one polyphonic port, 16 channels = 8 voices x 2 floats.
        outputs[BUS].setChannels(ov::kBusChannels);
        for (int v = 0; v < 8; v++) {
            outputs[BUS].setVoltage(ov::packNoteGate((uint8_t)v, curNote[v], gate[v]), 2 * v);
            outputs[BUS].setVoltage(ov::packBendCrc((uint8_t)v, curNote[v], gate[v], bend[v]), 2 * v + 1);
        }
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "midi", midiInput.toJson());

        json_t* channelsJ = json_array();
        for (int v = 0; v < 8; v++)
            json_array_append_new(channelsJ, json_integer(midiChannel[v]));
        json_object_set_new(rootJ, "channels", channelsJ);

        json_object_set_new(rootJ, "voct", json_integer(voct));
        json_object_set_new(rootJ, "gateInvert", json_boolean(gateInvert));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* midiJ = json_object_get(rootJ, "midi");
        if (midiJ)
            midiInput.fromJson(midiJ);

        json_t* channelsJ = json_object_get(rootJ, "channels");
        if (channelsJ) {
            for (int v = 0; v < 8; v++) {
                json_t* chJ = json_array_get(channelsJ, v);
                if (chJ) {
                    int ch = (int)json_integer_value(chJ);
                    if (ch >= 1 && ch <= 16)
                        midiChannel[v] = (uint8_t)ch;
                }
            }
        }

        json_t* voctJ = json_object_get(rootJ, "voct");
        if (voctJ) {
            int v = (int)json_integer_value(voctJ);
            if (v == 1 || v == 2 || v == 5 || v == 8)
                voct = (uint8_t)v;
        }

        json_t* gateJ = json_object_get(rootJ, "gateInvert");
        if (gateJ)
            gateInvert = json_boolean_value(gateJ);
    }
};

struct Midi2CvWidget : ModuleWidget {
    Midi2CvWidget(Midi2Cv* module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Midi2Cv.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        for (int v = 0; v < 8; v++) {
            float y = 18.0f + v * 13.0f;
            addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(12.7f, y)), module, Midi2Cv::CV1 + 2 * v));
            addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(38.1f, y)), module, Midi2Cv::GATE1 + 2 * v));
        }
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(25.4f, 114.0f)), module, Midi2Cv::BUS));
    }

    void appendContextMenu(Menu* menu) override {
        Midi2Cv* module = dynamic_cast<Midi2Cv*>(this->module);
        if (!module)
            return;

        menu->addChild(new MenuSeparator);
        menu->addChild(createSubmenuItem("V/oct", string::f("%d", module->voct), [=](Menu* menu) {
            for (int v : { 1, 2, 5, 8 }) {
                menu->addChild(createCheckMenuItem(string::f("%d V/oct", v), "",
                    [=]() { return module->voct == v; },
                    [=]() { module->voct = (uint8_t)v; }
                ));
            }
        }));

        menu->addChild(createBoolPtrMenuItem("Invert gates", "", &module->gateInvert));

        menu->addChild(createSubmenuItem("MIDI channels", "", [=](Menu* menu) {
            for (int v = 0; v < 8; v++) {
                menu->addChild(createSubmenuItem(string::f("Voice %d", v + 1),
                    string::f("Ch %d", module->midiChannel[v]), [=](Menu* menu) {
                    for (int ch = 1; ch <= 16; ch++) {
                        menu->addChild(createCheckMenuItem(string::f("Channel %d", ch), "",
                            [=]() { return module->midiChannel[v] == ch; },
                            [=]() { module->midiChannel[v] = (uint8_t)ch; }
                        ));
                    }
                }));
            }
        }));

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuItem("All notes off", "", [=]() {
            for (int v = 0; v < 8; v++) {
                module->noteCnt[v] = 0;
                module->curNote[v] = 24;
                module->gate[v] = false;
            }
        }));

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuItem("All notes off", "", [=]() {
            for (int v = 0; v < 8; v++) {
                module->noteCnt[v] = 0;
                module->curNote[v] = 24;
                module->gate[v] = false;
                module->bend[v] = 0;
            }
        }));

        menu->addChild(new MenuSeparator);
        appendMidiMenu(menu, &module->midiInput);
    }
};

Model* modelMidi2Cv = createModel<Midi2Cv, Midi2CvWidget>("Midi2Cv");
