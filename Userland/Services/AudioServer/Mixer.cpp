/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Mixer.h"
#include <AK/Array.h>
#include <AK/Format.h>
#include <AK/MemoryStream.h>
#include <AK/NumericLimits.h>
#include <AudioServer/ConnectionFromClient.h>
#include <AudioServer/Mixer.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Timer.h>
#include <pthread.h>
#include <sys/ioctl.h>

namespace AudioServer {

Mixer::Mixer(NonnullRefPtr<Core::ConfigFile> config)
    // FIXME: Allow AudioServer to use other audio channels as well
    : m_device(Core::File::construct("/dev/audio/0", this))
    , m_sound_thread(Threading::Thread::construct(
          [this] {
              mix();
              return 0;
          },
          "AudioServer[mixer]"sv))
    , m_config(move(config))
{
    if (!m_device->open(Core::OpenMode::WriteOnly)) {
        dbgln("Can't open audio device: {}", m_device->error_string());
        return;
    }

    m_muted = m_config->read_bool_entry("Master", "Mute", false);
    m_main_volume = static_cast<double>(m_config->read_num_entry("Master", "Volume", 100)) / 100.0;

    m_sound_thread->start();
}

NonnullRefPtr<ClientAudioStream> Mixer::create_queue(ConnectionFromClient& client)
{
    auto queue = adopt_ref(*new ClientAudioStream(client));
    {
        Threading::MutexLocker const locker(m_pending_mutex);
        m_pending_mixing.append(*queue);
    }
    // Signal the mixer thread to start back up, in case nobody was connected before.
    m_mixing_necessary.signal();

    return queue;
}

void Mixer::mix()
{
    decltype(m_pending_mixing) active_mix_queues;

    for (;;) {
        {
            Threading::MutexLocker const locker(m_pending_mutex);
            // While we have nothing to mix, wait on the condition.
            m_mixing_necessary.wait_while([this, &active_mix_queues]() { return m_pending_mixing.is_empty() && active_mix_queues.is_empty(); });
            if (!m_pending_mixing.is_empty()) {
                active_mix_queues.extend(move(m_pending_mixing));
                m_pending_mixing.clear();
            }
        }

        active_mix_queues.remove_all_matching([&](auto& entry) { return !entry->is_connected(); });

        Array<Audio::Sample, HARDWARE_BUFFER_SIZE> mixed_buffer;

        m_main_volume.advance_time();

        // Mix the buffers together into the output
        for (auto& queue : active_mix_queues) {
            if (!queue->client()) {
                queue->clear();
                continue;
            }
            queue->volume().advance_time();

            for (auto& mixed_sample : mixed_buffer) {
                Audio::Sample sample;
                if (!queue->get_next_sample(sample))
                    break;
                if (queue->is_muted())
                    continue;
                sample.log_multiply(SAMPLE_HEADROOM);
                sample.log_multiply(static_cast<float>(queue->volume()));
                mixed_sample += sample;
            }
        }

        // Even though it's not realistic, the user expects no sound at 0%.
        if (m_muted || m_main_volume < 0.01) {
            m_device->write(m_zero_filled_buffer.data(), static_cast<int>(m_zero_filled_buffer.size()));
        } else {
            OutputMemoryStream stream { m_stream_buffer };

            for (auto& mixed_sample : mixed_buffer) {
                mixed_sample.log_multiply(static_cast<float>(m_main_volume));
                mixed_sample.clip();

                LittleEndian<i16> out_sample;
                out_sample = static_cast<i16>(mixed_sample.left * NumericLimits<i16>::max());
                stream << out_sample;

                out_sample = static_cast<i16>(mixed_sample.right * NumericLimits<i16>::max());
                stream << out_sample;
            }

            VERIFY(stream.is_end());
            VERIFY(!stream.has_any_error());
            m_device->write(stream.data(), static_cast<int>(stream.size()));
        }
    }
}

void Mixer::set_main_volume(double volume)
{
    if (volume < 0)
        m_main_volume = 0;
    else if (volume > 2)
        m_main_volume = 2;
    else
        m_main_volume = volume;

    m_config->write_num_entry("Master", "Volume", static_cast<int>(volume * 100));
    request_setting_sync();

    ConnectionFromClient::for_each([&](ConnectionFromClient& client) {
        client.did_change_main_mix_volume({}, main_volume());
    });
}

void Mixer::set_muted(bool muted)
{
    if (m_muted == muted)
        return;
    m_muted = muted;

    m_config->write_bool_entry("Master", "Mute", m_muted);
    request_setting_sync();

    ConnectionFromClient::for_each([muted](ConnectionFromClient& client) {
        client.did_change_main_mix_muted_state({}, muted);
    });
}

int Mixer::audiodevice_set_sample_rate(u32 sample_rate)
{
    int code = ioctl(m_device->fd(), SOUNDCARD_IOCTL_SET_SAMPLE_RATE, sample_rate);
    if (code != 0)
        dbgln("Error while setting sample rate to {}: ioctl error: {}", sample_rate, strerror(errno));
    return code;
}

u32 Mixer::audiodevice_get_sample_rate() const
{
    u32 sample_rate = 0;
    int code = ioctl(m_device->fd(), SOUNDCARD_IOCTL_GET_SAMPLE_RATE, &sample_rate);
    if (code != 0)
        dbgln("Error while getting sample rate: ioctl error: {}", strerror(errno));
    return sample_rate;
}

void Mixer::request_setting_sync()
{
    if (m_config_write_timer.is_null() || !m_config_write_timer->is_active()) {
        m_config_write_timer = Core::Timer::create_single_shot(
            AUDIO_CONFIG_WRITE_INTERVAL,
            [this] {
                if (auto result = m_config->sync(); result.is_error())
                    dbgln("Failed to write audio mixer config: {}", result.error());
            },
            this);
        m_config_write_timer->start();
    }
}

ClientAudioStream::ClientAudioStream(ConnectionFromClient& client)
    : m_client(client)
{
}

}
